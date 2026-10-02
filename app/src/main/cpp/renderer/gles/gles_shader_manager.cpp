#include "gles_shader_manager.h"

#include "renderer_base.h"
#include "renderer_config.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <android/log.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define LOG_TAG "CopperOxide-Shader"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

namespace {

// SPIR-V module magic: word 0 of the module header.
constexpr uint32_t kSpirvMagic = 0x07230203u;
// A SPIR-V header is 5 words; anything shorter cannot be a module.
constexpr size_t kSpirvHeaderWords = 5;

constexpr int kSpirvUnknown = -1;
constexpr int kSpirvUnsupported = 0;
constexpr int kSpirvSupported = 1;

// glSpecializeShader is core in desktop GL 4.6 but only reachable through
// GL_OES_gl_spirv / GL_ARB_gl_spirv on ES, so the entry point is typedef'd and
// resolved at runtime instead of being linked against directly.
using PfnGlSpecializeShader = void(GL_APIENTRY*)(GLuint shader, const char* p_entry_point,
                                                 GLuint specialization_count,
                                                 const GLuint* p_specialization_values);

// Resolved once per process: the entry point lives in the driver and cannot
// change for the lifetime of the process.
PfnGlSpecializeShader specialize_shader() {
    static std::mutex resolve_mutex;
    static PfnGlSpecializeShader cached = nullptr;
    static bool resolved = false;
    std::lock_guard<std::mutex> lock(resolve_mutex);
    if (!resolved) {
        cached = reinterpret_cast<PfnGlSpecializeShader>(eglGetProcAddress("glSpecializeShader"));
        resolved = true;
    }
    return cached;
}

void query_gles_version(int& major, int& minor) {
    major = 0;
    minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
}

// Extension probe that stays valid on ES 3.2+, where the space-separated
// GL_EXTENSIONS string is deprecated in favour of glGetStringi().
bool device_has_extension(const char* needle) {
    int major = 0;
    int minor = 0;
    query_gles_version(major, minor);

    if (major > 3 || (major == 3 && minor >= 2)) {
        GLint count = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        for (GLint i = 0; i < count; ++i) {
            const GLubyte* extension = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
            if (extension == nullptr) {
                continue;
            }
            const char* extension_name = reinterpret_cast<const char*>(extension);
            if (std::string(extension_name).find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    const GLubyte* extensions = glGetString(GL_EXTENSIONS);
    if (extensions == nullptr) {
        return false;
    }
    return std::string(reinterpret_cast<const char*>(extensions)).find(needle) != std::string::npos;
}

const char* stage_name(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return "vertex";
        case ShaderStage::Fragment: return "fragment";
        case ShaderStage::Compute: return "compute";
        case ShaderStage::Geometry: return "geometry";
        case ShaderStage::TessellationControl: return "tessellation_control";
        case ShaderStage::TessellationEvaluation: return "tessellation_evaluation";
        case ShaderStage::Mesh: return "mesh";
        case ShaderStage::Task: return "task";
        case ShaderStage::RayGeneration: return "raygen";
        case ShaderStage::AnyHit: return "any_hit";
        case ShaderStage::ClosestHit: return "closest_hit";
        case ShaderStage::Miss: return "miss";
        case ShaderStage::Intersection: return "intersection";
        case ShaderStage::Callable: return "callable";
    }
    return "unknown";
}

std::string trim_whitespace(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::string();
    }
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool starts_with_version_directive(const std::string& line) {
    const size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return false;
    }
    return line.compare(first, 8, "#version") == 0;
}

// Turns ShaderManager::createShaderFromGLSL()'s defines into a #define block.
//
// GLSL requires the #version directive to be the first token of a shader, so
// the block is inserted *after* an existing #version line. Prepending blindly
// produces a compile error whose diagnostics point at the defines instead of
// at whatever the caller actually got wrong.
std::string inject_defines(const std::string& source, const std::vector<std::string>& defines) {
    if (defines.empty()) {
        return source;
    }

    std::string block;
    for (const std::string& define : defines) {
        const std::string trimmed = trim_whitespace(define);
        if (trimmed.empty()) {
            continue;
        }
        // A caller that already spelled out a directive is passed through: a
        // "#define #extension ..." wrapper would not compile.
        if (trimmed.front() == '#') {
            block += trimmed;
            block += "\n";
            continue;
        }
        block += "#define ";
        block += trimmed;
        block += "\n";
    }
    if (block.empty()) {
        return source;
    }

    size_t insert_pos = 0;
    const size_t first_line_end = source.find('\n');
    const std::string first_line =
        (first_line_end == std::string::npos) ? source : source.substr(0, first_line_end);
    if (starts_with_version_directive(first_line)) {
        insert_pos = (first_line_end == std::string::npos) ? source.size() : first_line_end + 1;
    }

    // No #version means ES 1.00 semantics. Inventing one here would silently
    // change the language version the source is compiled against, so the source
    // is compiled exactly as given and a compile failure stays a compile failure.
    return source.substr(0, insert_pos) + block + source.substr(insert_pos);
}

} // namespace

GLESShaderManager::GLESShaderManager(RendererBase* renderer) : renderer_(renderer) {
    // initialize() only caches the renderer pointer and reads the shader cache
    // size from the config, so running it here makes the manager usable the
    // moment it exists. initializeManagers() calling it again is harmless.
    ShaderManager::initialize(renderer);
}

GLESShaderManager::~GLESShaderManager() {
    // RendererBase::shutdown() destroys the managers through unique_ptr::reset()
    // without calling manager->shutdown(), so the GL objects have to be released
    // here. If no context is current the hooks log and skip: GLESCRenderer tears
    // the context down first, and the objects die with it, so nothing leaks.
    ShaderManager::shutdown();
}

GLuint GLESShaderManager::get_gl_shader(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = shaders_.find(handle);
    return it == shaders_.end() ? 0u : it->second.gl_object;
}

GLuint GLESShaderManager::get_gl_program(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = programs_.find(handle);
    return it == programs_.end() ? 0u : it->second.gl_program;
}

std::vector<uint32_t> GLESShaderManager::get_shader_spirv(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = shaders_.find(handle);
    if (it != shaders_.end()) {
        return it->second.spirv;
    }
    auto failed = failed_spirv_.find(handle);
    return failed == failed_spirv_.end() ? std::vector<uint32_t>() : failed->second;
}

std::unordered_map<std::string, uint32_t> GLESShaderManager::get_specialization_constants(
    uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = shaders_.find(handle);
    if (it == shaders_.end()) {
        return {};
    }
    return it->second.specialization_constants;
}

size_t GLESShaderManager::get_live_shader_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shaders_.size();
}

size_t GLESShaderManager::get_live_program_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return programs_.size();
}

bool GLESShaderManager::onCreateShader(uint64_t handle, ShaderStage stage,
                                       const std::vector<uint32_t>& spirv,
                                       const std::string& entry_point) {
    const GLenum gl_stage = to_gl_shader_stage(stage);
    if (gl_stage == 0) {
        LOGW("shader %llu: %s shaders have no GLES equivalent, use GLSL",
             static_cast<unsigned long long>(handle), stage_name(stage));
        return false;
    }

    if (spirv.size() < kSpirvHeaderWords || spirv[0] != kSpirvMagic) {
        LOGE("shader %llu: not a SPIR-V module (first word 0x%08x, %zu words)",
             static_cast<unsigned long long>(handle),
             spirv.empty() ? 0u : spirv[0], spirv.size());
        return false;
    }

    if (!requireContext("onCreateShader")) {
        return false;
    }

    // glCreateShader(GL_GEOMETRY_SHADER) is only legal on ES 3.2.
    if (stage == ShaderStage::Geometry && !es_version_at_least(3, 2)) {
        LOGW("shader %llu: geometry shaders need ES 3.2", static_cast<unsigned long long>(handle));
        return false;
    }

    if (!spirv_binary_supported()) {
        // Honest failure: ES 3.0 and most ES 3.1 drivers cannot consume SPIR-V,
        // and reporting success here would hand a module-less "shader" to the
        // pipeline builder and lose the device at glUseProgram() time. The words
        // are kept so a translator path can still pick them up later.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            store_failed_spirv(handle, spirv);
        }
        LOGW("shader %llu: device cannot consume SPIR-V (needs ES 3.1 + GL_OES_gl_spirv/"
             "GL_ARB_gl_spirv); module kept for a translator path, use "
             "createShaderFromGLSL() for this backend",
             static_cast<unsigned long long>(handle));
        return false;
    }

    PfnGlSpecializeShader specialize = specialize_shader();
    if (specialize == nullptr) {
        LOGE("shader %llu: glSpecializeShader entry point missing",
             static_cast<unsigned long long>(handle));
        return false;
    }

    // GL work runs unlocked: glShaderBinary() can already be expensive and the
    // caller (the base) serialises shader destruction with its own mutex.
    const GLuint gl_shader = glCreateShader(gl_stage);
    if (gl_shader == 0) {
        LOGE("shader %llu: glCreateShader failed (GL error 0x%x)",
             static_cast<unsigned long long>(handle), glGetError());
        return false;
    }

    // glShaderBinary() has no format parameter: GL_SHADER_BINARY_FORMAT_SPIR_V
    // (0x9551) is fixed by GL_OES_gl_spirv / GL_ARB_gl_spirv for the binary
    // format accepted by this entry point.
    glShaderBinary(gl_shader, 1, spirv.data(),
                   static_cast<GLsizei>(spirv.size() * sizeof(uint32_t)));
    specialize(gl_shader, entry_point.c_str(), 0, nullptr);

    GLint compiled = GL_FALSE;
    glGetShaderiv(gl_shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        log_shader_info_log(gl_shader, "glSpecializeShader");
        glDeleteShader(gl_shader);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ShaderObject object;
        object.gl_object = gl_shader;
        object.state = ShaderState::Ready;
        object.stage = stage;
        object.entry_point = entry_point;
        object.spirv = spirv;
        shaders_[handle] = std::move(object);
        failed_spirv_.erase(handle);
    }
    return true;
}

bool GLESShaderManager::onCreateShaderFromGLSL(uint64_t handle, ShaderStage stage,
                                               const std::string& glsl_source,
                                               const std::string& entry_point,
                                               const std::vector<std::string>& defines) {
    const GLenum gl_stage = to_gl_shader_stage(stage);
    if (gl_stage == 0) {
        LOGW("shader %llu: %s shaders have no GLES equivalent, use GLSL",
             static_cast<unsigned long long>(handle), stage_name(stage));
        return false;
    }
    if (glsl_source.empty()) {
        LOGW("shader %llu: empty GLSL source", static_cast<unsigned long long>(handle));
        return false;
    }
    if (!requireContext("onCreateShaderFromGLSL")) {
        return false;
    }

    // glCreateShader(GL_GEOMETRY_SHADER) is only legal on ES 3.2.
    if (stage == ShaderStage::Geometry && !es_version_at_least(3, 2)) {
        LOGW("shader %llu: geometry shaders need ES 3.2", static_cast<unsigned long long>(handle));
        return false;
    }

    if (entry_point != "main") {
        // glCreateShader() builds a monolithic shader: the entry point is fixed
        // by the source itself. Recording the request beats silently ignoring
        // it, but it cannot change what the driver compiles.
        LOGW("shader %llu: entry point '%s' ignored on GLES, the monolithic GLSL "
             "shader always uses the name in the source",
             static_cast<unsigned long long>(handle), entry_point.c_str());
    }

    const std::string final_source = inject_defines(glsl_source, defines);
    const GLchar* source_ptr = final_source.c_str();

    // GL work runs unlocked: glCompileShader() runs driver code that can block
    // for tens of milliseconds and may re-enter.
    const GLuint gl_shader = glCreateShader(gl_stage);
    if (gl_shader == 0) {
        LOGE("shader %llu: glCreateShader failed (GL error 0x%x)",
             static_cast<unsigned long long>(handle), glGetError());
        return false;
    }

    glShaderSource(gl_shader, 1, &source_ptr, nullptr);
    glCompileShader(gl_shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(gl_shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        log_shader_info_log(gl_shader, "glCompileShader");
        glDeleteShader(gl_shader);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ShaderObject object;
        object.gl_object = gl_shader;
        object.state = ShaderState::Ready;
        object.stage = stage;
        object.entry_point = entry_point;
        shaders_[handle] = std::move(object);
        failed_spirv_.erase(handle);
    }
    return true;
}

void GLESShaderManager::onDestroyShader(uint64_t handle) {
    GLuint gl_shader = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = shaders_.find(handle);
        if (it != shaders_.end()) {
            gl_shader = it->second.gl_object;
            shaders_.erase(it);
        }
        failed_spirv_.erase(handle);
    }

    if (gl_shader == 0) {
        // Already destroyed, or never created on this device. Returning here
        // instead of calling glDeleteShader(0) keeps a double destroy harmless
        // and never touches a name this manager does not own.
        return;
    }
    if (!requireContext("onDestroyShader")) {
        return;
    }

    // A shader still attached to a live program is only flagged for deletion;
    // the driver frees it when the last program is gone, which is exactly the
    // deferred behaviour Vulkan's VkShaderModule needs here.
    glDeleteShader(gl_shader);
}

bool GLESShaderManager::onCreateGraphicsPipeline(uint64_t handle, uint64_t vertex_shader,
                                                 uint64_t fragment_shader,
                                                 const PipelineLayoutDesc& layout) {
    if (!requireContext("onCreateGraphicsPipeline")) {
        return false;
    }
    report_unsupported_layout(layout);

    // GLES has no pipeline object: a "pipeline" here is a linked GL program.
    // The base still owns Pipeline::handle, so the mapping handle -> GL program
    // lives in programs_ and is exposed through get_gl_program().
    return link_program(handle, {vertex_shader, fragment_shader});
}

bool GLESShaderManager::onCreateComputePipeline(uint64_t handle, uint64_t compute_shader,
                                                const PipelineLayoutDesc& layout) {
    if (!requireContext("onCreateComputePipeline")) {
        return false;
    }
    report_unsupported_layout(layout);

    // Compute shaders are ES 3.1+; on ES 3.0 glCreateShader(GL_COMPUTE_SHADER)
    // is a hard GL_INVALID_ENUM, so the version is checked up front.
    if (!es_version_at_least(3, 1)) {
        LOGE("pipeline %llu: compute shaders need ES 3.1, this context is older",
             static_cast<unsigned long long>(handle));
        return false;
    }

    return link_program(handle, {compute_shader});
}

void GLESShaderManager::onDestroyPipeline(uint64_t handle) {
    GLuint gl_program = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = programs_.find(handle);
        if (it != programs_.end()) {
            gl_program = it->second.gl_program;
            programs_.erase(it);
        }
    }

    if (gl_program == 0) {
        // Unknown or already destroyed handle: tolerated so shutdown() and an
        // explicit destroyPipeline() can both run.
        return;
    }
    if (!requireContext("onDestroyPipeline")) {
        return;
    }

    glDeleteProgram(gl_program);
}

void GLESShaderManager::onSetShaderDebugName(uint64_t handle, const std::string& name) {
    bool known = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = shaders_.find(handle);
        if (it != shaders_.end()) {
            it->second.debug_name = name;
            known = true;
        }
    }

    if (!known) {
        return;
    }

    // No-op on the GL side on purpose: object naming is GL_KHR_debug, an ES 3.2
    // extension, and doing it half-way (only when the extension happens to be
    // present) would make debug markers device dependent. The name is kept for
    // logs and for a future KHR_debug path.
    if (debug_names_enabled()) {
        LOGI("shader %llu named '%s' (GL debug labels are not available on ES < 3.2)",
             static_cast<unsigned long long>(handle), name.c_str());
    }
}

void GLESShaderManager::onSetPipelineDebugName(uint64_t handle, const std::string& name) {
    bool known = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = programs_.find(handle);
        if (it != programs_.end()) {
            it->second.debug_name = name;
            known = true;
        }
    }

    if (!known) {
        return;
    }

    if (debug_names_enabled()) {
        LOGI("pipeline %llu (GL program %u) named '%s' (GL debug labels are not "
             "available on ES < 3.2)",
             static_cast<unsigned long long>(handle),
             static_cast<unsigned>(get_gl_program(handle)), name.c_str());
    }
}

void GLESShaderManager::onAddSpecializationConstant(uint64_t shader_handle, const std::string& name,
                                                    uint32_t value) {
    bool first_time = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = shaders_.find(shader_handle);
        if (it == shaders_.end()) {
            return;
        }
        auto existing = it->second.specialization_constants.find(name);
        // Re-recording an unchanged value would spam the log on every rebind.
        if (existing != it->second.specialization_constants.end() && existing->second == value) {
            return;
        }
        first_time = existing == it->second.specialization_constants.end();
        it->second.specialization_constants[name] = value;
    }

    if (first_time) {
        // Specialization constants are a SPIR-V concept: glSpecializeShader() only
        // understands them, and the GLSL path has no equivalent. The value is
        // recorded so a caller can see what was asked for, and reported once per
        // name so a shader that relied on it does not fail silently.
        LOGW("shader %llu: specialization constant '%s' = %u has NO effect on GLES "
             "(recorded only); use a #define or a uniform block on this backend",
             static_cast<unsigned long long>(shader_handle), name.c_str(),
             static_cast<unsigned>(value));
    }
}

bool GLESShaderManager::link_program(uint64_t handle,
                                     const std::vector<uint64_t>& shader_handles) {
    std::vector<GLuint> gl_shaders;
    gl_shaders.reserve(shader_handles.size());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint64_t shader_handle : shader_handles) {
            auto it = shaders_.find(shader_handle);
            if (it == shaders_.end() || it->second.gl_object == 0 ||
                it->second.state != ShaderState::Ready) {
                LOGW("pipeline %llu: shader %llu has no usable GL object",
                     static_cast<unsigned long long>(handle),
                     static_cast<unsigned long long>(shader_handle));
                return false;
            }
            gl_shaders.push_back(it->second.gl_object);
        }
    }

    // glLinkProgram() executes driver code that can block for milliseconds and
    // re-enter arbitrary code, so it runs with our mutex released. That is safe
    // here because the base ShaderManager holds its own mutex for the whole
    // createGraphicsPipeline()/createComputePipeline() call, which means no
    // concurrent destroyShader() can pull the objects out from under us.
    const GLuint program = glCreateProgram();
    if (program == 0) {
        LOGE("pipeline %llu: glCreateProgram failed (GL error 0x%x)",
             static_cast<unsigned long long>(handle), glGetError());
        return false;
    }

    for (GLuint gl_shader : gl_shaders) {
        glAttachShader(program, gl_shader);
    }
    glLinkProgram(program);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        log_program_info_log(program);
        glDeleteProgram(program);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        programs_[handle].gl_program = program;
    }
    return true;
}

GLenum GLESShaderManager::to_gl_shader_stage(ShaderStage stage) const {
    switch (stage) {
        case ShaderStage::Vertex:
            return GL_VERTEX_SHADER;
        case ShaderStage::Fragment:
            return GL_FRAGMENT_SHADER;
        case ShaderStage::Compute:
            return GL_COMPUTE_SHADER;
        case ShaderStage::Geometry:
            // Geometry shaders are ES 3.2+; the create hooks run the version
            // check (it needs a current context) so the reported failure names
            // the version instead of an unrelated "unknown stage".
            return GL_GEOMETRY_SHADER;
        default:
            // Tessellation/mesh/task/ray stages have no GLES core equivalent.
            return 0;
    }
}

bool GLESShaderManager::hasContext() const {
    if (renderer_ == nullptr) {
        return false;
    }
    if (!renderer_->isInitialized()) {
        return false;
    }
    // RendererBase::isInitialized() only says the renderer came up. EGL context
    // binding is per-thread state and the render thread is taken from a
    // coroutine pool, so this thread may not own the context even though the
    // renderer is running: without this check every GL call below would be a
    // no-op on some drivers and a crash on others.
    return eglGetCurrentContext() != EGL_NO_CONTEXT;
}

bool GLESShaderManager::requireContext(const char* operation) const {
    if (hasContext()) {
        return true;
    }
    LOGW("%s skipped: no GL context is current on this thread (renderer %s)", operation,
         renderer_ == nullptr ? "missing" : "up");
    return false;
}

bool GLESShaderManager::spirv_binary_supported() const {
    const int cached = spirv_support_.load(std::memory_order_relaxed);
    if (cached != kSpirvUnknown) {
        return cached == kSpirvSupported;
    }
    if (!hasContext()) {
        // Do not cache a negative answer that was produced without a context.
        return false;
    }

    int result = kSpirvUnsupported;
    // glShaderBinary is ES 3.1 core; glSpecializeShader and the SPIR-V binary
    // format need GL_OES_gl_spirv / GL_ARB_gl_spirv on top of that.
    if (es_version_at_least(3, 1) &&
        (renderer_->isExtensionSupported("GL_OES_gl_spirv") ||
         renderer_->isExtensionSupported("GL_ARB_gl_spirv") ||
         device_has_extension("gl_spirv")) &&
        specialize_shader() != nullptr) {
        result = kSpirvSupported;
    }
    spirv_support_.store(result, std::memory_order_relaxed);
    return result == kSpirvSupported;
}

bool GLESShaderManager::es_version_at_least(int major, int minor) const {
    int context_major = 0;
    int context_minor = 0;
    query_gles_version(context_major, context_minor);
    return context_major > major ||
           (context_major == major && context_minor >= minor);
}

void GLESShaderManager::log_shader_info_log(GLuint gl_shader, const char* reason) const {
    GLint log_length = 0;
    glGetShaderiv(gl_shader, GL_INFO_LOG_LENGTH, &log_length);
    if (log_length <= 1) {
        LOGE("shader compile failed after %s with no info log (GL error 0x%x)", reason,
             glGetError());
        return;
    }

    std::string log(static_cast<size_t>(log_length), '\0');
    GLsizei written = 0;
    glGetShaderInfoLog(gl_shader, log_length, &written, log.data());
    log.resize(static_cast<size_t>(written > 0 ? written : 0));
    LOGE("shader compile failed after %s: %s", reason, log.c_str());
}

void GLESShaderManager::log_program_info_log(GLuint gl_program) const {
    GLint log_length = 0;
    glGetProgramiv(gl_program, GL_INFO_LOG_LENGTH, &log_length);
    if (log_length <= 1) {
        LOGE("program link failed with no info log (GL error 0x%x)", glGetError());
        return;
    }

    std::string log(static_cast<size_t>(log_length), '\0');
    GLsizei written = 0;
    glGetProgramInfoLog(gl_program, log_length, &written, log.data());
    log.resize(static_cast<size_t>(written > 0 ? written : 0));
    LOGE("program link failed: %s", log.c_str());
}

void GLESShaderManager::report_unsupported_layout(const PipelineLayoutDesc& layout) const {
    // PipelineLayoutDesc is Vulkan-shaped. GLES programs discover their inputs
    // by name, so neither field has anything to bind against here:
    //   - set_layouts are VkDescriptorSetLayout ids. Textures, samplers and
    //     uniform blocks are bound by name through glUniform*/glBindTexture.
    //   - push_constant_ranges have no GL equivalent at all. Emulating them
    //     requires a uniform block that the caller sets up and updates with
    //     glUniformBlockBinding/glUniformBufferOffset, so silently creating one
    //     here would hand the caller a layout it never described.
    if (!layout.set_layouts.empty()) {
        LOGI("pipeline: %zu descriptor set layouts ignored on GLES (resources are "
             "bound by name through the linked program)",
             layout.set_layouts.size());
    }
    if (!layout.push_constant_ranges.empty()) {
        LOGW("pipeline: %zu push constant ranges ignored on GLES; use a uniform "
             "block with glUniformBufferOffset instead",
             layout.push_constant_ranges.size());
    }
}

void GLESShaderManager::store_failed_spirv(uint64_t handle,
                                           const std::vector<uint32_t>& spirv) {
    // Caller holds mutex_. The base throws the handle away when createShader()
    // returns 0, so nothing will ever ask for these entries again: they are kept
    // only as long as a translator path could still reach them, and the oldest
    // ones are dropped to keep a failing pipeline from growing memory forever.
    failed_spirv_[handle] = spirv;
    while (failed_spirv_.size() > kMaxFailedSpirvEntries) {
        auto oldest = failed_spirv_.begin();
        LOGI("dropping retained SPIR-V for shader %llu (only the %zu most recent "
             "failed modules are kept)",
             static_cast<unsigned long long>(oldest->first), kMaxFailedSpirvEntries);
        failed_spirv_.erase(oldest);
    }
}

bool GLESShaderManager::debug_names_enabled() const {
    // RendererConfig::enableDebugMarkers rather than NDEBUG: the Android build
    // always defines NDEBUG, so a compile-time check would silence this forever.
    return renderer_ != nullptr && renderer_->getConfig().enableDebugMarkers;
}

} // namespace copper