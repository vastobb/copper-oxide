#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>

#include <mutex>

#include "renderer_config.h"

namespace copper {

class RendererBase;
class ShaderTranslator;

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

    struct ShaderModule {
        uint64_t handle = 0;
        ShaderStage stage = ShaderStage::Vertex;
        std::vector<uint32_t> spirv;
        std::string entry_point = "main";
        std::string debug_name;
    };

    struct ShaderProgram {
        uint64_t handle = 0;
        uint64_t vertex_shader = 0;
        uint64_t fragment_shader = 0;
        uint64_t compute_shader = 0;
        std::vector<ShaderStage> stages;
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
    virtual ~ShaderManager();

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
    /**
     * Compiles and caches `source` for `stage`, invoking `callback` with the
     * resulting handle, or 0 on failure.
     *
     * NOT asynchronous: compilation reaches into this manager's handle space
     * and its cache, so it has to happen where those are valid. The cache is
     * what makes the repeat case free - getOrCreateShader consults it before
     * createShaderFromGLSL is ever called. The name is the existing callers'
     * contract and is kept rather than churned for its own sake.
     */
    virtual void compileAsync(const std::string& key, ShaderStage stage, const std::string& source, std::function<void(uint64_t)> callback);

    /// Enables the on-disk half of the SPIR-V cache. Call before the first
    /// shader is compiled. Safe to call more than once.
    void openShaderCache(const std::string& directory);

    /// Why the most recent shader compilation failed, or an empty string when it
    /// succeeded.
    ///
    /// createShaderFromGLSL reports failure by returning an invalid handle, which
    /// on its own cannot distinguish "the compiler rejected this source" from "no
    /// compiler is linked into this build" from "the backend refused the module".
    /// This is what a caller logs or shows a user.
    const std::string& lastShaderError() const;

    /// Compilation and cache counters, for telemetry and for the tests that
    /// prove the cache works.
    struct ShaderCompileStats {
        uint64_t compiled = 0;     ///< full GLSL -> SPIR-V compilations run
        uint64_t memory_hits = 0;  ///< served from the in-memory cache
        uint64_t disk_hits = 0;    ///< served from the on-disk cache
        uint64_t failures = 0;     ///< compiler or validation failures
        uint64_t total_ms = 0;     ///< summed compile time
    };
    ShaderCompileStats shaderCompileStats() const;

protected:
    /**
     * True when this backend compiles GLSL itself and must not receive SPIR-V.
     *
     * OpenGL ES returns true: glCompileShader consumes GLSL directly, and a
     * GLSL -> SPIR-V -> ESSL round trip would be a long way to change the shader
     * the driver sees and would put a compiler on the GLES path for no gain.
     * Vulkan returns false, so createShaderFromGLSL() compiles to SPIR-V first
     * and then calls onCreateShader().
     */
    virtual bool compilesGlslNatively() const { return false; }

    /// Compiles `glsl_source` to SPIR-V, consulting the cache first. The
    /// default implementation uses the process-wide ShaderTranslator. Backends
    /// that translate themselves never reach it.
    virtual bool translateGlslToSpirv(ShaderStage stage, const std::string& glsl_source,
                                      const std::string& entry_point,
                                      const std::vector<std::string>& defines,
                                      std::vector<uint32_t>* spirv_out, std::string* error_out);

    /// A stable 32-bit digest of a translator's identity, folded into the cache
    /// key so an artifact built by one compiler is never reused by another.
    static uint32_t toolchain_version(ShaderTranslator* translator);

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
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace copper