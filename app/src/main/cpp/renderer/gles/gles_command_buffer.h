#pragma once

#include "command_buffer.h"
#include "profiler.h"
#include "state_manager.h"

#include <GLES3/gl32.h>

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>

namespace copper {

// GLES backend for CommandBuffer.
//
// THERE IS NO COMMAND BUFFER OBJECT IN GLES. glDraw* / glBind* / glClear* execute
// against the current context as soon as they are called; there is no
// vkCmdDraw* to record into a VkCommandBuffer and no queue to submit it to. So
// the four lifecycle hooks map like this:
//
//   begin()  -> state reset for the frame's GL work
//   end()    -> nothing to close (no render-pass object to end)
//   submit() -> glFlush()   ; eglSwapBuffers() in GLESCRenderer::onPresent()
//                             is what actually presents
//   wait()   -> returns immediately, because there is no queue to wait on
//
// WHY wait() DOES NOT CALL glFinish() BY DEFAULT: glFinish() blocks the calling
// thread until every previously issued command has completed. It destroys the
// CPU/GPU overlap the frame pacing depends on, and on a tiled GLES
// implementation it is frequently slower than just letting the next
// eglSwapBuffers() do the ordering. Callers that genuinely need a stall (a
// readback, a context teardown, a screenshot) should call
// RendererBase::waitIdle(), which is the documented place for it, or turn on
// setFinishOnWait(true) and get the documented stall.
//
// GLES backend for CommandSink.
//
// This is the class that makes a draw real. CommandBuffer records the frame's
// work; the sink translates each recorded command into the equivalent GL call
// against the currently-bound context. OpenGL ES is immediate-mode, so the sink
// issues the call at replay time rather than recording anything itself.
//
// State that already has a home (program, vertex buffers, framebuffer,
// viewport) is delegated to StateManager, which owns the dirty-flag logic and
// knows the currently bound GL objects. Only the commands StateManager has no
// equivalent for are issued here.
class GLESCommandSink : public CommandSink {
public:
    GLESCommandSink();
    ~GLESCommandSink() override;

    // The state manager owns program/binding/framebuffer state. It may be null,
    // in which case the sink skips those commands instead of binding garbage.
    void setStateManager(StateManager* state);
    void setProfiler(Profiler* profiler);
    void setContextAvailable(bool available);
    bool contextAvailable() const;

    // Commands whose GL equivalent does not exist are counted here so a render
    // loop can assert it is not silently dropping work.
    uint64_t droppedCommandCount() const;

    void beginRenderPass(uint64_t render_pass, uint64_t framebuffer,
                         const std::array<float, 4>& clear_color, float clear_depth,
                         uint32_t clear_stencil) override;
    void endRenderPass() override;

    void bindPipeline(uint64_t pipeline) override;
    void bindVertexBuffers(uint32_t first_binding, const std::vector<uint64_t>& buffers,
                            const std::vector<uint32_t>& offsets) override;
    void bindIndexBuffer(uint64_t buffer, uint32_t index_type) override;
    void bindDescriptorSets(uint32_t first_set, const std::vector<uint64_t>& descriptor_sets,
                             const std::vector<uint32_t>& dynamic_offsets) override;

    void setViewport(float x, float y, float width, float height, float min_depth,
                     float max_depth) override;
    void setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) override;

    void draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
              uint32_t first_instance) override;
    void drawIndexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                     int32_t vertex_offset, uint32_t first_instance) override;
    void drawIndirect(uint64_t buffer, uint32_t offset, uint32_t draw_count,
                      uint32_t stride) override;
    void dispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) override;

    void copyBuffer(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset,
                    uint64_t dst_offset) override;
    void copyImage(uint64_t src, uint64_t dst, uint32_t width, uint32_t height, uint32_t depth,
                   uint32_t mip_level, uint32_t array_layer) override;
    void pipelineBarrier(uint32_t src_stage, uint32_t dst_stage, uint32_t dependency_flags,
                         const std::vector<uint64_t>& buffers,
                         const std::vector<uint64_t>& images) override;
    void pushConstants(uint32_t stage_flags, uint32_t offset, uint32_t size,
                       const void* data) override;

    // Index type / topology the last bindIndexBuffer recorded, so the draw
    // hooks know which glDrawElements overload to call.
    GLenum drawIndexType() const;

private:
    mutable std::mutex mutex_;
    StateManager* state_ = nullptr;
    Profiler* profiler_ = nullptr;
    bool context_available_ = false;
    GLenum index_type_ = GL_UNSIGNED_SHORT;
    uint64_t dropped_ = 0;
};

class GLESCCommandBuffer : public CommandBuffer {
public:
    GLESCCommandBuffer();
    ~GLESCCommandBuffer() override;

    // When true, wait() issues glFinish() and therefore STALLS the calling
    // thread until the GPU has drained. Off by default; see the class comment.
    void setFinishOnWait(bool finish);
    bool finishOnWait() const;

    // Drains and logs the GL error queue once per frame boundary. Off by
    // default: glGetError can force a driver flush, which is not free on a
    // per-frame basis.
    void setErrorChecking(bool checking);
    bool errorChecking() const;

    // Last error seen by the drain, or 0 when the drain is off / clean.
    GLenum lastGlError() const;

    // Frame lifecycle counters, for a render loop that wants to assert that the
    // begin/end/submit triple is balanced.
    uint64_t beginCount() const;
    uint64_t submitCount() const;
    uint64_t waitCount() const;
    uint64_t finishWaitCount() const;
    bool isRecording() const;

    // GLES has no secondary command buffers, so a deferred command has to run
    // where it is recorded. The base instead stores the std::function forever
    // and never invokes it, which keeps dangling the caller's stack references.
    void defer(std::function<void()> command) override;

protected:
    bool onBegin() override;
    void onEnd() override;
    bool onSubmit() override;
    void onWait() override;

private:
    // Drain the GL error queue into last_gl_error_. Only called when the context
    // is known to be current, i.e. between RendererBase::beginFrame() and
    // endFrame().
    void drainGlErrorsLocked();

    // Guards everything below. Never held while calling into the base.
    mutable std::mutex mutex_;
    bool finish_on_wait_ = false;
    bool error_checking_ = false;
    bool recording_ = false;
    GLenum last_gl_error_ = 0;
    uint64_t begin_count_ = 0;
    uint64_t submit_count_ = 0;
    uint64_t wait_count_ = 0;
    uint64_t finish_wait_count_ = 0;
};

} // namespace copper
