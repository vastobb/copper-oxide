#include "shader_manager.h"
#include "renderer_base.h"

#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>

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

uint64_t ShaderManager::createShaderFromGLSL(ShaderStage stage, const std::string& glsl_source, const std::string& entry_point, const std::vector<std::string>& defines) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    // In a real implementation, this would use glslang to compile GLSL to SPIR-V
    // For now, return a placeholder
    uint64_t handle = pImpl->next_shader_handle++;

    Impl::Shader shader;
    shader.handle = handle;
    shader.stage = stage;
    shader.entry_point = entry_point;

    if (!onCreateShaderFromGLSL(handle, stage, glsl_source, entry_point, defines)) {
        return 0;
    }

    pImpl->shaders[handle] = std::move(shader);
    return handle;
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
    // Compilation currently runs inline. Callers must treat this as blocking:
    // a background pool is wired up with the real shader translation backend.
    const uint64_t handle = getOrCreateShader(
        key, [this, stage, &source]() { return createShaderFromGLSL(stage, source, "main", {}); });
    if (callback) {
        callback(handle);
    }
}

} // namespace copper