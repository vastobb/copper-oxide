#pragma once

// OpenGL ES backend implementation of StateManager.
//
// The base class is deliberately Vulkan-shaped: it hands 16 Vulkan vertex
// bindings, 32 descriptor sets, an index "type" and a topology enum to the
// backend. GLES expresses most of that differently:
//
//   - A "pipeline" is a linked GL program (see GLESShaderManager); this manager
//     resolves the handle and issues glUseProgram().
//   - Vertex and index buffer names are bound to GL_ARRAY_BUFFER /
//     GL_ELEMENT_BUFFER. BufferManager handles are opaque ids, so the GL names
//     come from GLESBufferManager::glObject(). Per-binding offsets live in the
//     vertex array object, which the caller owns.
//   - GLES has no descriptor sets at all. The sets are recorded for diagnostics
//     and nothing else.
//   - Topology is a glDrawArrays/glDrawElements argument, not state, so it is
//     recorded and translated by to_gl_primitive() at draw time.
//
// GL calls run without this class's mutex held: only the calling thread can own
// the context, and the base already serialises applyState() under its own lock.
// The mutex here guards only the mirror of the GL state that the diagnostics
// getters expose.

#include <GLES3/gl32.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

#include "state_manager.h"

namespace copper {

class RendererBase;
class GLESBufferManager;
class GLESShaderManager;

class GLESCStateManager : public StateManager {
public:
    explicit GLESCStateManager(RendererBase* renderer);

    // The manager owns a mutex and the renderer pointer, so it is neither
    // copyable nor movable.
    GLESCStateManager(const GLESCStateManager&) = delete;
    GLESCStateManager& operator=(const GLESCStateManager&) = delete;
    GLESCStateManager(GLESCStateManager&&) = delete;
    GLESCStateManager& operator=(GLESCStateManager&&) = delete;

    // Vulkan primitive enum -> GL primitive. Returns 0 when the topology has no
    // GLES equivalent, which callers must treat as "cannot draw this".
    static GLenum to_gl_primitive(uint32_t topology);
    // Vulkan VK_INDEX_TYPE_* -> GL type. Returns 0 for an unknown value.
    static GLenum to_gl_index_type(uint32_t vulkan_index_type);

    // Mirror of the GL state for diagnostics. The base already exposes the
    // Vulkan-shaped pipeline handle through getBoundPipeline().
    GLuint get_bound_program() const;
    uint64_t get_index_buffer() const;
    uint32_t get_index_type() const;
    uint32_t get_topology() const;
    std::array<uint64_t, 16> get_vertex_buffers() const;
    std::array<uint32_t, 16> get_vertex_offsets() const;
    std::array<uint64_t, 32> get_descriptor_sets() const;
    bool is_scissor_enabled() const;

protected:
    void onBindPipeline(uint64_t pipeline) override;
    void onBindVertexBuffers(const std::array<uint64_t, 16>& buffers,
                             const std::array<uint32_t, 16>& offsets) override;
    void onBindIndexBuffer(uint64_t buffer, uint32_t index_type) override;
    void onBindDescriptorSets(const std::array<uint64_t, 32>& descriptor_sets,
                              const std::vector<uint32_t>& dynamic_offsets) override;
    void onSetViewport(float x, float y, float width, float height, float min_depth,
                       float max_depth) override;
    void onSetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) override;
    void onSetTopology(uint32_t topology) override;
    void onBindFramebuffer(uint64_t framebuffer) override;

private:
    // True when this thread can issue GL calls at all.
    bool hasContext() const;
    // hasContext() plus the logged early-out every hook needs.
    bool requireContext(const char* operation) const;
    // Cached ShaderManager lookup; null when the renderer has not been given one.
    GLESShaderManager* shader_manager() const;
    // Maps a ShaderManager pipeline handle onto the GL program to bind.
    GLuint resolve_program(uint64_t pipeline) const;

    // Cached BufferManager lookup, used to turn an opaque BufferManager handle
    // into the GL buffer name. Never reinterpret_cast a handle into a GL name:
    // the handle space is owned by the buffer manager, not by GL.
    GLESBufferManager* buffer_manager() const;
    GLuint resolve_buffer(uint64_t buffer_handle) const;

    RendererBase* renderer_ = nullptr;

    // Lock order is always base StateManager mutex -> this mutex. No hook ever
    // calls back into a base public method, so the two can never invert.
    mutable std::mutex mutex_;
    mutable GLESShaderManager* shader_manager_ = nullptr;
    mutable GLESBufferManager* buffer_manager_ = nullptr;
    GLuint bound_program_ = 0;
    std::array<uint64_t, 16> vertex_buffers_{};
    std::array<uint32_t, 16> vertex_offsets_{};
    uint64_t index_buffer_ = 0;
    uint32_t index_type_ = 0;
    uint32_t topology_ = 0;
    std::array<uint64_t, 32> descriptor_sets_{};
    size_t dynamic_offset_count_ = 0;
    bool scissor_enabled_ = false;
    bool viewport_has_depth_range_ = false;
    float depth_range_min_ = 0.0f;
    float depth_range_max_ = 1.0f;
    // One-shot warning latches. These fire on per-frame paths and would
    // otherwise flood logcat; they are mutable because some of them are read and
    // written from the const resolve path.
    mutable bool warned_descriptor_sets_ = false;
    mutable bool warned_pipeline_resolution_ = false;
    mutable bool warned_buffer_resolution_ = false;
};

} // namespace copper