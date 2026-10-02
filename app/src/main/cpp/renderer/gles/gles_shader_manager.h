#pragma once

// OpenGL ES backend implementation of ShaderManager.
//
// Two structural differences from the Vulkan backend leak into this API, and
// both are documented at the point where they matter:
//
//   1. GLES has no pipeline objects. ShaderManager::Pipeline::handle stays a
//      backend-agnostic id owned by the base class; get_gl_program() maps that
//      id onto the GL program object which the state manager glUseProgram()s.
//      The caller-facing API is unchanged: callers keep creating "pipelines"
//      and binding them by handle.
//   2. GLES cannot ingest SPIR-V on every device. createShader() reports
//      failure when the device lacks ES 3.1 + GL_OES_gl_spirv/GL_ARB_gl_spirv
//      instead of pretending the module compiled. createShaderFromGLSL() is
//      the always-available path.
//
// Every GL object is created, linked and destroyed on the thread that has the
// EGL context current; hasContext() is checked before any GL call because the
// render thread is picked from a coroutine pool and may change between frames.

#include <GLES3/gl32.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "shader_manager.h"

namespace copper {

class RendererBase;

class GLESShaderManager : public ShaderManager {
public:
    explicit GLESShaderManager(RendererBase* renderer);
    ~GLESShaderManager() override;

    // The manager owns a mutex and the renderer pointer. Copying would duplicate
    // both, and moving would leave the source destroying GL objects the
    // destination also believes it owns.
    GLESShaderManager(const GLESShaderManager&) = delete;
    GLESShaderManager& operator=(const GLESShaderManager&) = delete;
    GLESShaderManager(GLESShaderManager&&) = delete;
    GLESShaderManager& operator=(GLESShaderManager&&) = delete;

    // GL-side views onto the Vulkan-shaped handles. 0 means "no GL object"
    // (unknown handle, or a module the device could not create).
    GLuint get_gl_shader(uint64_t handle) const;
    GLuint get_gl_program(uint64_t handle) const;

    // The SPIR-V words are kept next to the GL object so a translator path that
    // never hands the module to the driver can still consume them later.
    std::vector<uint32_t> get_shader_spirv(uint64_t handle) const;

    // Recorded for diagnostics only - GLES GLSL has no SPIR-V specialization
    // constants. See onAddSpecializationConstant().
    std::unordered_map<std::string, uint32_t> get_specialization_constants(uint64_t handle) const;

    // Bookkeeping counts, used by leak assertions and debug dumps.
    size_t get_live_shader_count() const;
    size_t get_live_program_count() const;

protected:
    bool onCreateShader(uint64_t handle, ShaderStage stage, const std::vector<uint32_t>& spirv,
                        const std::string& entry_point) override;
    bool onCreateShaderFromGLSL(uint64_t handle, ShaderStage stage, const std::string& glsl_source,
                                const std::string& entry_point,
                                const std::vector<std::string>& defines) override;
    void onDestroyShader(uint64_t handle) override;
    bool onCreateGraphicsPipeline(uint64_t handle, uint64_t vertex_shader,
                                  uint64_t fragment_shader,
                                  const PipelineLayoutDesc& layout) override;
    bool onCreateComputePipeline(uint64_t handle, uint64_t compute_shader,
                                 const PipelineLayoutDesc& layout) override;
    void onDestroyPipeline(uint64_t handle) override;
    void onSetShaderDebugName(uint64_t handle, const std::string& name) override;
    void onSetPipelineDebugName(uint64_t handle, const std::string& name) override;
    void onAddSpecializationConstant(uint64_t shader_handle, const std::string& name,
                                     uint32_t value) override;

private:
    enum class ShaderState : uint8_t {
        Pending,  // recorded, no GL object yet
        Ready,    // usable by glAttachShader
        Failed,   // device refused it, SPIR-V kept for a future path
    };

    struct ShaderObject {
        GLuint gl_object = 0;
        ShaderState state = ShaderState::Pending;
        ShaderStage stage = ShaderStage::Vertex;
        std::string entry_point;
        std::string debug_name;
        std::vector<uint32_t> spirv;
        std::unordered_map<std::string, uint32_t> specialization_constants;
    };

    struct ProgramObject {
        GLuint gl_program = 0;
        std::string debug_name;
    };

    // Shared implementation of the two pipeline hooks: a GLES "pipeline" is a
    // linked GL program, so both paths end up in the same place.
    bool link_program(uint64_t handle, const std::vector<uint64_t>& shader_handles);

    // Returns 0 when the stage has no GLES equivalent.
    GLenum to_gl_shader_stage(ShaderStage stage) const;

    // True when this thread can issue GL calls at all.
    bool hasContext() const;
    // hasContext() plus the logged early-out every hook needs.
    bool requireContext(const char* operation) const;
    // Device SPIR-V support, probed once per process (needs a current context).
    bool spirv_binary_supported() const;
    bool es_version_at_least(int major, int minor) const;

    void log_shader_info_log(GLuint gl_shader, const char* reason) const;
    void log_program_info_log(GLuint gl_program) const;
    void report_unsupported_layout(const PipelineLayoutDesc& layout) const;
    void store_failed_spirv(uint64_t handle, const std::vector<uint32_t>& spirv);
    bool debug_names_enabled() const;

    // RendererBase only, not GLESCRenderer: the manager needs nothing beyond
    // the isInitialized()/isExtensionSupported()/getConfig() query surface, and
    // depending on the backend header would drag the legacy manager
    // declarations into every translation unit.
    RendererBase* renderer_ = nullptr;

    // Guards the maps below and nothing else. No GL call ever runs with it held:
    // glLinkProgram() and glCompileShader() execute driver code that may
    // re-enter, and holding a lock across them is how a stalled compile becomes
    // a deadlock. Lock order is always base ShaderManager mutex -> this mutex,
    // so a hook can never call back into the base public API.
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, ShaderObject> shaders_;
    std::unordered_map<uint64_t, ProgramObject> programs_;
    // SPIR-V the device refused. Ordered so eviction is oldest-first, and capped
    // because the base discards the handle of a failed createShader() and will
    // therefore never ask us to destroy these entries.
    std::map<uint64_t, std::vector<uint32_t>> failed_spirv_;
    static constexpr size_t kMaxFailedSpirvEntries = 16;
    // -1 not probed yet, 0 unsupported, 1 supported.
    mutable std::atomic<int> spirv_support_{-1};
};

} // namespace copper