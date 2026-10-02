#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace copper {

class RendererBase;

enum class ShaderStage : uint8_t {
    Vertex = 0,
    Fragment = 1,
    Compute = 2,
    Geometry = 3,
    TessellationControl = 4,
    TessellationEvaluation = 5,
    Mesh = 6,
    Task = 7,
    RayGeneration = 8,
    AnyHit = 9,
    ClosestHit = 10,
    Miss = 11,
    Intersection = 12,
    Callable = 13,
};

struct PipelineLayoutDesc {
    std::vector<uint32_t> set_layouts;
    std::vector<uint32_t> push_constant_ranges;
};

class ShaderManager {
public:
    struct Shader {
        uint64_t handle = 0;
        ShaderStage stage = ShaderStage::Vertex;
        std::vector<uint32_t> spirv;
        std::string entry_point = "main";
        std::string debug_name;
    };

    struct Pipeline {
        uint64_t handle = 0;
        uint64_t vertex_shader = 0;
        uint64_t fragment_shader = 0;
        uint64_t compute_shader = 0;
        std::vector<ShaderStage> stages;
        std::string debug_name;
    };

    ShaderManager();
    ~ShaderManager();

    ShaderManager(const ShaderManager&) = delete;
    ShaderManager& operator=(const ShaderManager&) = delete;
    ShaderManager(ShaderManager&&) noexcept = default;
    ShaderManager& operator=(ShaderManager&&) noexcept = default;

    bool initialize(RendererBase* renderer);
    void shutdown();

    virtual uint64_t createShader(ShaderStage stage, const std::vector<uint32_t>& spirv, const std::string& entry_point = "main");
    virtual uint64_t createShaderFromGLSL(ShaderStage stage, const std::string& glsl_source, const std::string& entry_point, const std::vector<std::string>& defines);
    virtual void destroyShader(uint64_t handle);

    virtual uint64_t createGraphicsPipeline(uint64_t vertex_shader, uint64_t fragment_shader, const PipelineLayoutDesc& layout);
    virtual uint64_t createComputePipeline(uint64_t compute_shader, const PipelineLayoutDesc& layout);
    virtual void destroyPipeline(uint64_t handle);

    virtual uint64_t getOrCreateShader(const std::string& key, std::function<uint64_t()> creator);
    virtual uint64_t getOrCreatePipeline(const std::string& key, std::function<uint64_t()> creator);

    virtual void setShaderDebugName(uint64_t handle, const std::string& name);
    virtual void setPipelineDebugName(uint64_t handle, const std::string& name);
    virtual void addSpecializationConstant(uint64_t shader_handle, const std::string& name, uint32_t value);
    virtual void compileAsync(const std::string& key, ShaderStage stage, const std::string& source, std::function<void(uint64_t)> callback);

protected:
    virtual bool onCreateShader(uint64_t handle, ShaderStage stage, const std::vector<uint32_t>& spirv, const std::string& entry_point) = 0;
    virtual bool onCreateShaderFromGLSL(uint64_t handle, ShaderStage stage, const std::string& glsl_source, const std::string& entry_point, const std::vector<std::string>& defines) = 0;
    virtual void onDestroyShader(uint64_t handle) = 0;
    virtual bool onCreateGraphicsPipeline(uint64_t handle, uint64_t vertex_shader, uint64_t fragment_shader, const PipelineLayoutDesc& layout) = 0;
    virtual bool onCreateComputePipeline(uint64_t handle, uint64_t compute_shader, const PipelineLayoutDesc& layout) = 0;
    virtual void onDestroyPipeline(uint64_t handle) = 0;
    virtual void onSetShaderDebugName(uint64_t handle, const std::string& name) = 0;
    virtual void onSetPipelineDebugName(uint64_t handle, const std::string& name) = 0;
    virtual void onAddSpecializationConstant(uint64_t shader_handle, const std::string& name, uint32_t value) = 0;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace copper