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
                              toolchainVersion(translator));

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

const std::string& ShaderManager::lastShaderError() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->last_error;
}

} // namespace copper
