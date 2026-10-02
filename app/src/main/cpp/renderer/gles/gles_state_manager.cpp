#include "gles_state_manager.h"

#include "gles_buffer_manager.h"
#include "gles_missing_es31.h"
#include "gles_shader_manager.h"
#include "renderer_base.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <android/log.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

#define LOG_TAG "CopperOxide-State"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

GLESCStateManager::GLESCStateManager(RendererBase* renderer) : renderer_(renderer) {
    // initialize() only caches the renderer pointer, so doing it here makes the
    // manager usable immediately; initializeManagers() calling it again is
    // harmless.
    StateManager::initialize(renderer);
}

GLenum GLESCStateManager::to_gl_primitive(uint32_t topology) {
    // VkPrimitiveTopology values. Only the topologies GL core can rasterise are
    // mapped; patch lists and the adjacency variants have no ES equivalent and
    // return 0 so the caller reports the draw as unsupported instead of
    // silently rasterising the wrong primitive.
    switch (topology) {
        case 0: return GL_POINTS;            // VK_PRIMITIVE_TOPOLOGY_POINT_LIST
        case 1: return GL_LINES;             // VK_PRIMITIVE_TOPOLOGY_LINE_LIST
        case 2: return GL_LINE_STRIP;        // VK_PRIMITIVE_TOPOLOGY_LINE_STRIP
        case 3: return GL_TRIANGLES;         // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
        case 4: return GL_TRIANGLE_STRIP;    // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
        case 5: return GL_TRIANGLE_FAN;      // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN
        default:
            return 0;
    }
}

GLenum GLESCStateManager::to_gl_index_type(uint32_t vulkan_index_type) {
    // VK_INDEX_TYPE_UINT16 / VK_INDEX_TYPE_UINT32.
    switch (vulkan_index_type) {
        case 1: return GL_UNSIGNED_SHORT;
        case 2: return GL_UNSIGNED_INT;
        default:
            return 0;
    }
}

GLuint GLESCStateManager::get_bound_program() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bound_program_;
}

uint64_t GLESCStateManager::get_index_buffer() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_buffer_;
}

uint32_t GLESCStateManager::get_index_type() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_type_;
}

uint32_t GLESCStateManager::get_topology() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return topology_;
}

std::array<uint64_t, 16> GLESCStateManager::get_vertex_buffers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return vertex_buffers_;
}

std::array<uint32_t, 16> GLESCStateManager::get_vertex_offsets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return vertex_offsets_;
}

std::array<uint64_t, 32> GLESCStateManager::get_descriptor_sets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return descriptor_sets_;
}

bool GLESCStateManager::is_scissor_enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return scissor_enabled_;
}

void GLESCStateManager::onBindPipeline(uint64_t pipeline) {
    if (!requireContext("onBindPipeline")) {
        return;
    }

    GLuint program = 0;
    if (pipeline != 0) {
        program = resolve_program(pipeline);
        if (program == 0) {
            // Binding 0 here would turn a lookup miss into "everything renders
            // black"; leaving the current program in place keeps the failure
            // visible as a wrong-but-alive frame instead.
            return;
        }
    }
    // pipeline == 0 is an explicit unbind (resetState() produces it), and
    // glUseProgram(0) is the legal way to express that.

    glUseProgram(program);

    std::lock_guard<std::mutex> lock(mutex_);
    bound_program_ = program;
}

void GLESCStateManager::onBindVertexBuffers(const std::array<uint64_t, 16>& buffers,
                                            const std::array<uint32_t, 16>& offsets) {
    if (!requireContext("onBindVertexBuffers")) {
        return;
    }

    size_t bound = 0;
    for (size_t i = 0; i < buffers.size(); ++i) {
        // Zero means "this binding is unused" (Vulkan's unbound state). GLES has
        // no indexed buffer bindings in ES 3.0, so there is nothing to clear -
        // binding 0 here would wipe the array buffer a previous binding needed
        // for glVertexAttribPointer replays.
        if (buffers[i] == 0) {
            continue;
        }
        const GLuint gl_buffer = resolve_buffer(buffers[i]);
        if (gl_buffer == 0) {
            continue;
        }
        glBindBuffer(GL_ARRAY_BUFFER, gl_buffer);
        ++bound;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        vertex_buffers_ = buffers;
        vertex_offsets_ = offsets;
    }
    if (bound == 0) {
        LOGW("no vertex binding produced a GL buffer name (of %zu slots); nothing "
             "was bound to GL_ARRAY_BUFFER",
             buffers.size());
    }

    // The offsets are recorded, not applied: on GLES the offset/stride pair
    // belongs to glVertexAttribPointer (or glBindVertexBuffer on ES 3.2) and is
    // therefore part of the vertex array object the caller owns. Recording them
    // here keeps the whole binding state inspectable in one place.
}

void GLESCStateManager::onBindIndexBuffer(uint64_t buffer, uint32_t index_type) {
    if (!requireContext("onBindIndexBuffer")) {
        return;
    }

    // GL_ELEMENT_BUFFER binding is vertex array object state on GLES, not
    // context state: this call edits whichever VAO is bound right now. Callers
    // that swap VAOs must therefore bind the index buffer after binding the VAO,
    // which is the same ordering constraint the Vulkan backend has.
    const GLuint gl_buffer = buffer == 0 ? 0u : resolve_buffer(buffer);
    // An unresolvable buffer is unbound rather than left pointing at the
    // previous index data: a stale index buffer produces silently wrong
    // geometry, whereas an unbound one makes the draw fail loudly.
    glBindBuffer(GL_ELEMENT_BUFFER, gl_buffer);

    if (to_gl_index_type(index_type) == 0) {
        LOGW("index buffer bound with unsupported index type %u; GLES draw calls "
             "take the type as an argument, so this is recorded but unused",
             static_cast<unsigned>(index_type));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    index_buffer_ = buffer;
    index_type_ = index_type;
}

void GLESCStateManager::onBindDescriptorSets(const std::array<uint64_t, 32>& descriptor_sets,
                                             const std::vector<uint32_t>& dynamic_offsets) {
    // GLES has no descriptor sets, no descriptor pools and no dynamic offsets.
    // A linked GL program exposes its inputs by name and they are bound with
    // glUniform* / glBindTexture / glBindBufferBase, which the caller issues per
    // draw. Interpreting the Vulkan-shaped handles as texture or buffer names
    // here would fabricate a binding the caller never asked for, so the correct
    // behaviour is to record the request and move on.
    bool warn = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        descriptor_sets_ = descriptor_sets;
        dynamic_offset_count_ = dynamic_offsets.size();
        // Once per manager: this runs on a per-frame path, and an all-zero
        // request (the common case, since most passes bind none) is not worth a
        // warning.
        if (!warned_descriptor_sets_ && descriptor_sets != std::array<uint64_t, 32>{}) {
            warned_descriptor_sets_ = true;
            warn = true;
        }
    }

    if (warn) {
        LOGW("descriptor sets ignored on GLES: %zu sets and %zu dynamic offsets "
             "recorded for diagnostics only; bind resources by name on the program "
             "instead",
             static_cast<size_t>(descriptor_sets.size()),
             static_cast<size_t>(dynamic_offsets.size()));
    }
}

void GLESCStateManager::onSetViewport(float x, float y, float width, float height,
                                      float min_depth, float max_depth) {
    if (!requireContext("onSetViewport")) {
        return;
    }

    // glViewport() takes integers and rejects negative width/height with
    // GL_INVALID_VALUE, so an invalid viewport is clamped to an empty one and
    // reported. A 0x0 viewport is legal (and is exactly what resetState() leaves
    // behind), so it is not worth a warning.
    if (width < 0.0f || height < 0.0f) {
        LOGW("viewport %gx%g has a negative extent, clamping to 0x0",
             static_cast<double>(width), static_cast<double>(height));
    }
    const GLsizei gl_width = width > 0.0f ? static_cast<GLsizei>(width) : 0;
    const GLsizei gl_height = height > 0.0f ? static_cast<GLsizei>(height) : 0;
    // x/y are passed through verbatim. GLES measures them from the bottom-left
    // of the framebuffer while Vulkan measures them from the top-left, so a
    // caller doing Vulkan-style y flipping has to pre-compute the offset; the
    // manager does not second-guess it here.
    glViewport(static_cast<GLint>(x), static_cast<GLint>(y), gl_width, gl_height);

    // Vulkan's viewport min/max depth maps onto glDepthRangef. That is context
    // wide state on GLES rather than per-viewport, so it is only re-issued when
    // it actually changes - otherwise every viewport update would reset a depth
    // range another pass had set up.
    bool update_depth_range = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!viewport_has_depth_range_ || depth_range_min_ != min_depth ||
            depth_range_max_ != max_depth) {
            viewport_has_depth_range_ = true;
            depth_range_min_ = min_depth;
            depth_range_max_ = max_depth;
            update_depth_range = true;
        }
    }
    if (update_depth_range) {
        glDepthRangef(min_depth, max_depth);
    }
}

void GLESCStateManager::onSetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    if (!requireContext("onSetScissor")) {
        return;
    }

    // A zero-area scissor in Vulkan means "clip everything away". The GL
    // equivalent that keeps the rest of the frame intact is to switch the test
    // off, which is also what resetState() needs to produce a full-frame draw.
    const bool enable = width > 0 && height > 0;
    if (enable) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(static_cast<GLint>(x), static_cast<GLint>(y),
                  static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    } else {
        glDisable(GL_SCISSOR_TEST);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    scissor_enabled_ = enable;
}

void GLESCStateManager::onSetTopology(uint32_t topology) {
    // Nothing to do on the GL side: topology is an argument of
    // glDrawArrays/glDrawElements, not bindable state. The value is recorded and
    // translated through to_gl_primitive() by whoever issues the draw; doing a
    // dummy draw here to "apply" it would be pure overhead.
    if (to_gl_primitive(topology) == 0) {
        LOGW("topology %u has no GLES equivalent; the draw will be skipped by the "
             "caller",
             static_cast<unsigned>(topology));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    topology_ = topology;
}

void GLESCStateManager::onBindFramebuffer(uint64_t framebuffer) {
    if (!requireContext("onBindFramebuffer")) {
        return;
    }

    // Handle 0 is the default framebuffer, which on GLES is the EGL window
    // surface itself. The swapchain path renders straight into it, so this is a
    // normal value rather than an error.
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer == 0 ? 0u : static_cast<GLuint>(framebuffer));
}

bool GLESCStateManager::hasContext() const {
    if (renderer_ == nullptr) {
        return false;
    }
    if (!renderer_->isInitialized()) {
        return false;
    }
    // Per-thread EGL state: the renderer being up says nothing about whether
    // *this* thread owns the context, and only the owner may issue GL calls.
    return eglGetCurrentContext() != EGL_NO_CONTEXT;
}

bool GLESCStateManager::requireContext(const char* operation) const {
    if (hasContext()) {
        return true;
    }
    LOGW("%s skipped: no GL context is current on this thread (renderer %s)", operation,
         renderer_ == nullptr ? "missing" : "up");
    return false;
}

GLESShaderManager* GLESCStateManager::shader_manager() const {
    if (shader_manager_ != nullptr) {
        return shader_manager_;
    }
    if (renderer_ == nullptr) {
        return nullptr;
    }
    // dynamic_cast rather than static_cast: RendererBase::getShaderManager()
    // hands back whatever backend was installed, and a wrong one must degrade to
    // "no program" instead of reinterpreting a pointer.
    // Racing threads compute the same value and the renderer owns the manager
    // for its whole lifetime, so the benign race on this cache is safe.
    shader_manager_ = dynamic_cast<GLESShaderManager*>(renderer_->getShaderManager());
    return shader_manager_;
}

GLESBufferManager* GLESCStateManager::buffer_manager() const {
    if (buffer_manager_ != nullptr) {
        return buffer_manager_;
    }
    if (renderer_ == nullptr) {
        return nullptr;
    }
    // Same argument as shader_manager(): a foreign BufferManager must degrade to
    // "no GL name" rather than have its handle space reinterpreted as GL names.
    // Racing threads compute the same pointer and the renderer owns the manager
    // for its whole lifetime.
    buffer_manager_ = dynamic_cast<GLESBufferManager*>(renderer_->getBufferManager());
    return buffer_manager_;
}

GLuint GLESCStateManager::resolve_buffer(uint64_t buffer_handle) const {
    GLESBufferManager* buffers = buffer_manager();
    const GLuint gl_buffer = buffers != nullptr ? buffers->glObject(buffer_handle) : 0;
    if (gl_buffer != 0) {
        return gl_buffer;
    }

    bool first_failure = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        first_failure = !warned_buffer_resolution_;
        warned_buffer_resolution_ = true;
    }
    if (first_failure) {
        if (buffers != nullptr) {
            LOGW("buffer %llu is unknown to the GLES buffer manager; nothing bound",
                 static_cast<unsigned long long>(buffer_handle));
        } else {
            LOGW("cannot resolve buffer %llu: the renderer has no GLES buffer manager",
                 static_cast<unsigned long long>(buffer_handle));
        }
    }
    return 0;
}

GLuint GLESCStateManager::resolve_program(uint64_t pipeline) const {
    GLESShaderManager* shaders = shader_manager();
    const GLuint program = shaders != nullptr ? shaders->get_gl_program(pipeline) : 0;
    if (program != 0) {
        return program;
    }

    // Resolution failures repeat every frame while the pass is set up wrong, so
    // the explanation is logged once and the latch keeps it out of logcat.
    bool first_failure = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        first_failure = !warned_pipeline_resolution_;
        warned_pipeline_resolution_ = true;
    }
    if (first_failure) {
        if (shaders != nullptr) {
            LOGW("pipeline %llu has no linked GL program; bind skipped",
                 static_cast<unsigned long long>(pipeline));
        } else {
            LOGW("cannot resolve pipeline %llu: the renderer has no GLES shader "
                 "manager",
                 static_cast<unsigned long long>(pipeline));
        }
    }
    return 0;
}

} // namespace copper