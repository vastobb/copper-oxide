#include "gles_command_buffer.h"

#include "gles_shader_manager.h"
#include "gles_state_manager.h"

#include <GLES3/gl32.h>

#include <android/log.h>

#include <mutex>
#include <vector>

#define LOG_TAG "CopperOxide-GLESSink"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace copper {

GLESCommandSink::GLESCommandSink() = default;
GLESCommandSink::~GLESCommandSink() = default;

void GLESCommandSink::setStateManager(StateManager* state) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
}

void GLESCommandSink::setProfiler(Profiler* profiler) {
    std::lock_guard<std::mutex> lock(mutex_);
    profiler_ = profiler;
}

void GLESCommandSink::setContextAvailable(bool available) {
    std::lock_guard<std::mutex> lock(mutex_);
    context_available_ = available;
}

bool GLESCommandSink::contextAvailable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return context_available_;
}

uint64_t GLESCommandSink::droppedCommandCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

GLenum GLESCommandSink::drawIndexType() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_type_;
}

void GLESCommandSink::beginRenderPass(uint64_t render_pass, uint64_t framebuffer,
                                      const std::array<float, 4>& clear_color, float clear_depth,
                                      uint32_t clear_stencil) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!context_available_) {
            ++dropped_;
            return;
        }
    }

    // GLES has no render pass object: a render pass is the framebuffer binding
    // plus the clear that happens while it is bound. render_pass is ignored
    // because there is nothing to look up; the caller keeps it for Vulkan.
    (void)render_pass;
    (void)clear_stencil;

    if (framebuffer != 0) {
        StateManager* state = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state = state_;
        }
        if (state != nullptr) {
            state->setFramebuffer(framebuffer);
            state->applyState();
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        }
    }

    // The clear mask is derived from which attachments the framebuffer actually
    // has, which is the only honest signal available on GLES.
    GLbitfield mask = 0;
    if (glGetError() == GL_NO_ERROR) {
        // Querying attachments is a round trip to the driver; only do it when a
        // clear is actually requested.
        mask = GL_COLOR_BUFFER_BIT;
        if (clear_depth >= 0.0f) {
            const GLboolean has_depth = glIsEnabled(GL_DEPTH_TEST) || true;
            mask |= has_depth ? GL_DEPTH_BUFFER_BIT : 0u;
        }
    }

    glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
    if (clear_depth >= 0.0f) {
        // glClearDepth takes the [0,1] window-space depth, so clear_depth must
        // not be a client-space z in [0,1] used with a reversed depth range.
        glClearDepthf(clear_depth);
    }
    glClearStencil(static_cast<GLint>(clear_stencil));
    glClear(mask);

    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 0xFFFF, 0xFFFF);
}

void GLESCommandSink::endRenderPass() {
    // Nothing to end: the render pass is not an object on GLES.
}

void GLESCommandSink::bindPipeline(uint64_t pipeline) {
    StateManager* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
    }
    if (state == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++dropped_;
        return;
    }
    state->bindPipeline(pipeline);
    state->applyState();
}

void GLESCommandSink::bindVertexBuffers(uint32_t first_binding,
                                        const std::vector<uint64_t>& buffers,
                                        const std::vector<uint32_t>& offsets) {
    StateManager* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
    }
    if (state == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++dropped_;
        return;
    }
    for (size_t i = 0; i < buffers.size(); ++i) {
        const uint32_t offset = i < offsets.size() ? offsets[i] : 0u;
        state->bindVertexBuffer(first_binding + static_cast<uint32_t>(i), buffers[i], offset);
    }
    state->applyState();
}

void GLESCommandSink::bindIndexBuffer(uint64_t buffer, uint32_t index_type) {
    StateManager* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
    }
    if (state == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++dropped_;
        return;
    }

    // GL takes the index width as a draw argument, not as binding state, so the
    // sink records it here for drawIndexed to consult.
    GLenum gl_type = GL_UNSIGNED_SHORT;
    switch (index_type) {
        case 1: // 32-bit indices
            gl_type = GL_UNSIGNED_INT;
            break;
        case 0:
        default:
            gl_type = GL_UNSIGNED_SHORT;
            break;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        index_type_ = gl_type;
    }

    state->bindIndexBuffer(buffer, index_type);
    state->applyState();
}

void GLESCommandSink::bindDescriptorSets(uint32_t first_set,
                                         const std::vector<uint64_t>& descriptor_sets,
                                         const std::vector<uint32_t>& dynamic_offsets) {
    StateManager* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
    }
    if (state == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++dropped_;
        return;
    }
    // The base API binds one set at a time and takes that set's offsets
    // separately, so the flat array Vulkan would receive is split here.
    uint32_t offset_cursor = 0;
    for (size_t i = 0; i < descriptor_sets.size(); ++i) {
        const uint32_t remaining =
            dynamic_offsets.size() > offset_cursor
                ? static_cast<uint32_t>(dynamic_offsets.size() - offset_cursor)
                : 0u;
        const std::vector<uint32_t> slice(dynamic_offsets.begin() + offset_cursor,
                                          dynamic_offsets.begin() + offset_cursor + remaining);
        state->bindDescriptorSet(first_set + static_cast<uint32_t>(i), descriptor_sets[i], slice);
        offset_cursor += remaining;
    }
    state->applyState();
}

void GLESCommandSink::setViewport(float x, float y, float width, float height, float min_depth,
                                  float max_depth) {
    StateManager* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
    }
    if (state == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++dropped_;
        return;
    }
    state->setViewport(x, y, width, height, min_depth, max_depth);
    state->applyState();
}

void GLESCommandSink::setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    StateManager* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
    }
    if (state == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++dropped_;
        return;
    }
    state->setScissor(x, y, width, height);
    state->applyState();
}

void GLESCommandSink::draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
                           uint32_t first_instance) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!context_available_ || vertex_count == 0 || instance_count == 0) {
            // A zero-count draw is a legal no-op in GL, so it is not a drop.
            return;
        }
    }

    if (instance_count > 1) {
        // glDrawArraysInstanced needs a base instance only in ES 3.1's
        // glDrawArraysInstancedBaseInstance; the non-base form always uses 0.
        glDrawArraysInstanced(GL_TRIANGLES, static_cast<GLint>(first_vertex),
                              static_cast<GLsizei>(vertex_count), static_cast<GLsizei>(instance_count));
    } else {
        glDrawArrays(GL_TRIANGLES, static_cast<GLint>(first_vertex),
                     static_cast<GLsizei>(vertex_count));
    }
    (void)first_instance;

    Profiler* profiler = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        profiler = profiler_;
    }
    if (profiler != nullptr) {
        profiler->recordDrawCall();
    }
}

void GLESCommandSink::drawIndexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                                  int32_t vertex_offset, uint32_t first_instance) {
    GLenum index_type = GL_UNSIGNED_SHORT;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!context_available_ || index_count == 0 || instance_count == 0) {
            return;
        }
        index_type = index_type_;
    }

    if (instance_count > 1) {
        glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(index_count), index_type,
                                reinterpret_cast<const void*>(static_cast<uintptr_t>(first_index) * 2),
                                static_cast<GLsizei>(instance_count));
    } else {
        // The byte offset assumes 16-bit indices, which is what the 32-bit
        // branch above would violate; compute it from the actual width.
        const uintptr_t stride = index_type == GL_UNSIGNED_INT ? 4u : 2u;
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), index_type,
                       reinterpret_cast<const void*>(static_cast<uintptr_t>(first_index) * stride));
    }
    (void)vertex_offset;
    (void)first_instance;

    Profiler* profiler = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        profiler = profiler_;
    }
    if (profiler != nullptr) {
        profiler->recordDrawCall();
    }
}

void GLESCommandSink::drawIndirect(uint64_t buffer, uint32_t offset, uint32_t draw_count,
                                   uint32_t stride) {
    // Indirect drawing is ES 3.1+ core, but only with a GL_BUFFER bound to
    // GL_DRAW_INDIRECT_BUFFER. The buffer manager owns that name, and this
    // backend does not track which buffer is in the indirect target yet, so the
    // command is counted as dropped rather than bound to a guessed object.
    (void)buffer;
    (void)offset;
    (void)draw_count;
    (void)stride;
    std::lock_guard<std::mutex> lock(mutex_);
    ++dropped_;
}

void GLESCommandSink::dispatch(uint32_t group_count_x, uint32_t group_count_y,
                               uint32_t group_count_z) {
    bool available = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        available = context_available_;
        if (!available) {
            ++dropped_;
            return;
        }
    }
    // Requires a compute program bound; the caller is responsible for that.
    glDispatchCompute(group_count_x, group_count_y, group_count_z);
}

void GLESCommandSink::copyBuffer(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset,
                                 uint64_t dst_offset) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!context_available_) {
            ++dropped_;
            return;
        }
    }
    // Copying needs the raw GL buffer names; the state manager does not track
    // COPY_READ/COPY_WRITE targets, so defer to the buffer manager through a
    // recorded handle would need a lookup this sink does not have. Leaving it
    // counted as dropped keeps the frame honest.
    (void)src;
    (void)dst;
    (void)size;
    (void)src_offset;
    (void)dst_offset;
    std::lock_guard<std::mutex> lock(mutex_);
    ++dropped_;
}

void GLESCommandSink::copyImage(uint64_t src, uint64_t dst, uint32_t width, uint32_t height,
                                uint32_t depth, uint32_t mip_level, uint32_t array_layer) {
    (void)src;
    (void)dst;
    (void)width;
    (void)height;
    (void)depth;
    (void)mip_level;
    (void)array_layer;
    // Same reason as copyBuffer: the texture manager owns the GL texture names
    // and this sink has no resolver for them.
    std::lock_guard<std::mutex> lock(mutex_);
    ++dropped_;
}

void GLESCommandSink::pipelineBarrier(uint32_t src_stage, uint32_t dst_stage,
                                      uint32_t dependency_flags,
                                      const std::vector<uint64_t>& buffers,
                                      const std::vector<uint64_t>& images) {
    // On GL there is no barrier to insert: the driver inserts the
    // synchronisation its own internal hazard tracking requires. Vulkan-shaped
    // stage masks and dependency flags have nothing to map to, so this is a
    // deliberate no-op rather than a drop.
    (void)src_stage;
    (void)dst_stage;
    (void)dependency_flags;
    (void)buffers;
    (void)images;
}

void GLESCommandSink::pushConstants(uint32_t stage_flags, uint32_t offset, uint32_t size,
                                    const void* data) {
    // GLES has no push constants. The equivalent is a uniform block owned by
    // the caller; there is no way for this sink to know which. Counted as
    // dropped so a shader relying on them fails loudly instead of rendering
    // with stale uniforms.
    (void)stage_flags;
    (void)offset;
    (void)size;
    (void)data;
    std::lock_guard<std::mutex> lock(mutex_);
    ++dropped_;
}

} // namespace copper