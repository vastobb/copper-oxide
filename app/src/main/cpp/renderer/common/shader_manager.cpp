#include "shader_manager.h"

#include "renderer_base.h"
#include "shader_cache.h"
#include "shader_translator.h"

#include <android/log.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define LOG_TAG "CopperOxide-Shader"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// The cache key's target tag. A backend that compiles GLSL natively never
// reaches the translator, so only Vulkan uses this today.
static constexpr char kShaderCacheTargetApi[] = "vulkan";

namespace copper {

class ShaderManager::Impl {
public:
    struct Shader {
        uint64_t handle = 0;
        ShaderStage stage = ShaderStage::Vertex;
        std::vector<uint32_t> spirv;
        std::string entry_point = "main";
        std::string debug_name;
        std::unordered_map<std::string, uint32_t> specialization_constants;
    };

    struct Pipeline {
        uint64_t handle = 0;
        uint64_t vertex_shader = 0;
        uint64_t fragment_shader = 0;
        uint64_t compute_shader = 0;
        std::vector<ShaderStage> stages;
        std::string debug_name;
    };

    std::unordered_map<uint64_t, Shader> shaders;
    std::unordered_map<uint64_t, Pipeline> pipelines;
    std::unordered_map<std::string, uint64_t> shader_cache;
    std::unordered_map<std::string, uint64_t> pipeline_cache;
    uint64_t next_shader_handle = 1;
    uint64_t next_pipeline_handle = 1;
    std::mutex mutex;
    RendererBase* renderer = nullptr;
    size_t max_cache_size_mb = 64;
    size_t current_cache_size_mb = 0;

    // Per-manager state only. The compiled SPIR-V and its counters are
    // process-wide (spirvCache(), translationStats()), because the key is a
    // content hash: two managers asking for the same shader want the same
    // artifact, and one copy is the only way a hit can happen across them.
    std::string last_error;
};

ShaderManager::ShaderManager() : pImpl(std::make_unique<Impl>()) {}
ShaderManager::~ShaderManager() = default;

bool ShaderManager::initialize(RendererBase* renderer) {
    pImpl->renderer = renderer;
    if (renderer) {
        const auto& config = renderer->getConfig();
        pImpl->max_cache_size_mb = config.shaderCacheSizeMb;
    }
    return true;
}

void ShaderManager::shutdown() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    for (auto& [handle, pipeline] : pImpl->pipelines) {
        onDestroyPipeline(handle);
    }
    for (auto& [handle, shader] : pImpl->shaders) {
        onDestroyShader(handle);
    }
    pImpl->pipelines.clear();
    pImpl->shaders.clear();
    pImpl->shader_cache.clear();
    pImpl->pipeline_cache.clear();
    pImpl->current_cache_size_mb = 0;
}

uint64_t ShaderManager::createShader(ShaderStage stage, const std::vector<uint32_t>& spirv, const std::string& entry_point) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    uint64_t handle = pImpl->next_shader_handle++;

    Impl::Shader shader;
    shader.handle = handle;
    shader.stage = stage;
    shader.spirv = spirv;
    shader.entry_point = entry_point;

    if (!onCreateShader(handle, stage, spirv, entry_point)) {
        return 0;
    }

    pImpl->shaders[handle] = std::move(shader);
    return handle;
}

bool ShaderManager::translateGlslToSpirv(ShaderStage stage, const std::string& glsl_source,
                                          const std::string& entry_point,
                                          const std::vector<std::string>& defines,
                                          std::vector<uint32_t>* spirv_out,
                                          std::string* error_out) {
    // Rejected before the compiler is invoked, so an unsupported stage produces
    // "this backend does not support a mesh shader" rather than a parse error
    // about something that is not the author's fault.
    if (!isShaderStageSupported(stage)) {
        if (error_out != nullptr) {
            *error_out = std::string("the Vulkan backend does not support a ") +
                         shaderStageName(stage) + " shader";
        }
        return false;
    }

    ShaderTranslator* translator = shaderTranslator();
    if (translator == nullptr || !translator->available()) {
        if (error_out != nullptr) {
            *error_out =
                "no GLSL to SPIR-V compiler is linked into this build, so the Vulkan backend "
                "cannot accept GLSL source";
        }
        return false;
    }

    // One key for the whole operation: the lookup and the store must agree, and
    // building it twice invites a mismatch that would silently never hit.
    const std::string cache_key =
        ShaderCache::buildKey(stage, glsl_source, defines, kShaderCacheTargetApi,
                              toolchain_version(translator));

    {
        // A cached hit is the common case once a shader has been seen once, and
        // it costs no compilation at all. The cache is internally synchronised,
        // so only the stats update needs this lock.
        std::vector<uint32_t> cached;
        if (spirvCache().lookup(cache_key, &cached)) {
            recordTranslation(true, 0, true, /*disk_hit=*/false);
            *spirv_out = std::move(cached);
            return true;
        }
    }

    TranslationRequest request;
    request.stage = stage;
    request.source = glsl_source;
    request.entry_point = entry_point;
    request.defines = defines;
    request.target_api = kShaderCacheTargetApi;

    const TranslationResult result = translator->translate(request);
    recordTranslation(result.success, result.compile_ms, false, false);

    if (!result.success) {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->last_error = result.error;
        if (error_out != nullptr) {
            *error_out = result.error;
        }
        return false;
    }

    *spirv_out = result.spirv;

    // Storing is best effort: a cache that cannot be written must not fail a
    // shader that compiled correctly. The next process pays for the miss.
    spirvCache().store(cache_key, stage, *spirv_out);
    return true;
}

uint32_t ShaderManager::toolchain_version(ShaderTranslator* translator) {
    // FNV-1a over the compiler identity, truncated to 32 bits. Stable across
    // processes and platforms, unlike std::hash.
    const std::string id = translator != nullptr ? translator->toolchainId() : std::string("none");
    uint32_t hash = 2166136261u;
    for (const char character : id) {
        hash ^= static_cast<uint8_t>(character);
        hash *= 16777619u;
    }
    return hash == 0 ? 1u : hash;
}

uint64_t ShaderManager::createShaderFromGLSL(ShaderStage stage, const std::string& glsl_source,
                                             const std::string& entry_point,
                                             const std::vector<std::string>& defines) {
    if (glsl_source.empty()) {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->last_error = "createShaderFromGLSL was given an empty source";
        return 0;
    }

    uint64_t handle = 0;
    {
        // The handle is reserved under the lock and the work is done outside it.
        // A compile can take hundreds of milliseconds; holding this mutex across
        // it would stall every other shader operation, including destroy.
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        handle = pImpl->next_shader_handle++;
        pImpl->last_error.clear();
    }

    if (compilesGlslNatively()) {
        // OpenGL ES: hand the source to the driver's own GLSL compiler.
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        if (!onCreateShaderFromGLSL(handle, stage, glsl_source, entry_point, defines)) {
            return 0;
        }
        Impl::Shader shader;
        shader.handle = handle;
        shader.stage = stage;
        shader.entry_point = entry_point;
        pImpl->shaders[handle] = std::move(shader);
        return handle;
    }

    // Vulkan: GLSL is not a Vulkan dialect, so it has to become SPIR-V first.
    std::vector<uint32_t> spirv;
    std::string error;
    if (!translateGlslToSpirv(stage, glsl_source, entry_point, defines, &spirv, &error)) {
        LOGE("GLSL -> SPIR-V failed for a %s shader: %s", shaderStageName(stage), error.c_str());
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->last_error = error;
        return 0;
    }

    if (!onCreateShader(handle, stage, spirv, entry_point)) {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->last_error = "the backend refused the compiled SPIR-V";
        return 0;
    }

    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Shader shader;
    shader.handle = handle;
    shader.stage = stage;
    shader.spirv = std::move(spirv);
    shader.entry_point = entry_point;
    pImpl->shaders[handle] = std::move(shader);
    LOGI("compiled a %s shader from GLSL: %zu SPIR-V words", shaderStageName(stage),
         pImpl->shaders[handle].spirv.size());
    return handle;
}

void ShaderManager::openShaderCache(const std::string& directory) {
    // Deliberately not under pImpl->mutex: open() touches the filesystem. The
    // cache is internally synchronised, and a lookup racing this simply misses
    // until the directory exists.
    spirvCache().open(directory);
}

const std::string& ShaderManager::lastShaderError() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->last_error;
}

ShaderManager::ShaderCompileStats ShaderManager::shaderCompileStats() const {
    // Process-wide, so a pre-warm through compileToSpirv() and a later
    // createShader() are counted in one place rather than two that disagree.
    const TranslationStats stats = translationStats();
    ShaderCompileStats out;
    out.compiled = stats.compiled;
    out.memory_hits = stats.memory_hits;
    out.disk_hits = stats.disk_hits;
    out.failures = stats.failures;
    out.total_ms = stats.total_ms;
    return out;
}

void ShaderManager::destroyShader(uint64_t handle) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    // Destroying a shader that a live pipeline references leaves the pipeline
    // holding a dangling VkShaderModule.
    for (const auto& entry : pImpl->pipelines) {
        const Impl::Pipeline& pipeline = entry.second;
        if (pipeline.vertex_shader == handle || pipeline.fragment_shader == handle ||
            pipeline.compute_shader == handle) {
            return;
        }
    }
    auto it = pImpl->shaders.find(handle);
    if (it == pImpl->shaders.end()) {
        return;
    }

    onDestroyShader(handle);

    for (auto cache_it = pImpl->shader_cache.begin(); cache_it != pImpl->shader_cache.end(); ++cache_it) {
        if (cache_it->second == handle) {
            pImpl->shader_cache.erase(cache_it);
            break;
        }
    }

    pImpl->shaders.erase(it);
}

uint64_t ShaderManager::createGraphicsPipeline(uint64_t vertex_shader, uint64_t fragment_shader, const PipelineLayoutDesc& layout) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    // A dangling shader handle would be handed straight to the driver.
    if (vertex_shader == 0 || fragment_shader == 0 ||
        pImpl->shaders.find(vertex_shader) == pImpl->shaders.end() ||
        pImpl->shaders.find(fragment_shader) == pImpl->shaders.end()) {
        return 0;
    }
    uint64_t handle = pImpl->next_pipeline_handle++;

    Impl::Pipeline pipeline;
    pipeline.handle = handle;
    pipeline.vertex_shader = vertex_shader;
    pipeline.fragment_shader = fragment_shader;
    pipeline.stages = {ShaderStage::Vertex, ShaderStage::Fragment};

    if (!onCreateGraphicsPipeline(handle, vertex_shader, fragment_shader, layout)) {
        return 0;
    }

    pImpl->pipelines[handle] = std::move(pipeline);
    return handle;
}

uint64_t ShaderManager::createComputePipeline(uint64_t compute_shader, const PipelineLayoutDesc& layout) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (compute_shader == 0 || pImpl->shaders.find(compute_shader) == pImpl->shaders.end()) {
        return 0;
    }
    uint64_t handle = pImpl->next_pipeline_handle++;

    Impl::Pipeline pipeline;
    pipeline.handle = handle;
    pipeline.compute_shader = compute_shader;
    pipeline.stages = {ShaderStage::Compute};

    if (!onCreateComputePipeline(handle, compute_shader, layout)) {
        return 0;
    }

    pImpl->pipelines[handle] = std::move(pipeline);
    return handle;
}

void ShaderManager::destroyPipeline(uint64_t handle) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->pipelines.find(handle);
    if (it == pImpl->pipelines.end()) {
        return;
    }

    onDestroyPipeline(handle);

    for (auto cache_it = pImpl->pipeline_cache.begin(); cache_it != pImpl->pipeline_cache.end(); ++cache_it) {
        if (cache_it->second == handle) {
            pImpl->pipeline_cache.erase(cache_it);
            break;
        }
    }

    pImpl->pipelines.erase(it);
}

uint64_t ShaderManager::getOrCreateShader(const std::string& key, std::function<uint64_t()> creator) {
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        auto it = pImpl->shader_cache.find(key);
        if (it != pImpl->shader_cache.end()) {
            return it->second;
        }
    }
    // creator() locks internally; calling it under the lock self-deadlocks.
    const uint64_t handle = creator();
    if (handle != 0) {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->shader_cache.emplace(key, handle);
    }
    return handle;
}

uint64_t ShaderManager::getOrCreatePipeline(const std::string& key, std::function<uint64_t()> creator) {
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        auto it = pImpl->pipeline_cache.find(key);
        if (it != pImpl->pipeline_cache.end()) {
            return it->second;
        }
    }
    const uint64_t handle = creator();
    if (handle != 0) {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->pipeline_cache.emplace(key, handle);
    }
    return handle;
}

void ShaderManager::setShaderDebugName(uint64_t handle, const std::string& name) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->shaders.find(handle);
    if (it != pImpl->shaders.end()) {
        it->second.debug_name = name;
        onSetShaderDebugName(handle, name);
    }
}

void ShaderManager::setPipelineDebugName(uint64_t handle, const std::string& name) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->pipelines.find(handle);
    if (it != pImpl->pipelines.end()) {
        it->second.debug_name = name;
        onSetPipelineDebugName(handle, name);
    }
}

void ShaderManager::addSpecializationConstant(uint64_t shader_handle, const std::string& name, uint32_t value) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->shaders.find(shader_handle);
    if (it != pImpl->shaders.end()) {
        it->second.specialization_constants[name] = value;
        onAddSpecializationConstant(shader_handle, name, value);
    }
}

void ShaderManager::compileAsync(const std::string& key, ShaderStage stage, const std::string& source, std::function<void(uint64_t)> callback) {
    // Runs inline. Deliberate: compilation reaches into this manager's handle
    // space and its cache, so it has to happen where those are valid. The cache
    // is what keeps the repeat case cheap.
    const uint64_t handle = getOrCreateShader(
        key, [this, stage, &source]() { return createShaderFromGLSL(stage, source, "main", {}); });
    if (callback) {
        callback(handle);
    }
}

} // namespace copper