#pragma once

// WHY: the platform guard has to be defined before vulkan.h is pulled in,
// otherwise the Android surface/window types stay invisible.
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <vector>

#include "command_buffer.h"

namespace copper {

class VulkanRenderer;
class VulkanSyncManager;

// Vulkan backend for CommandBuffer.
//
// ---------------------------------------------------------------------------
// DOES THE BASE STILL RECORD INTO A VECTOR THAT IS NEVER REPLAYED?
// ---------------------------------------------------------------------------
// YES, with a caveat. Read carefully, because common/command_buffer.cpp has
// changed shape and the two halves of the answer differ:
//
//   1. EVERY recording method (draw, bindPipeline, pipelineBarrier, pushConstants,
//      ...) unconditionally pushes an Impl::Command onto pImpl->commands. The
//      Impl comment claims "when a sink is set, commands are forwarded immediately
//      and nothing is accumulated" -- that is NOT what the code does. There is no
//      early-out on pImpl->sink in any of those methods. So yes: everything still
//      lands in the vector.
//
//   2. The vector IS now replayable: CommandBuffer::execute() walks it and
//      dispatches each entry to a CommandSink. But execute() is a NEW public
//      method that nothing calls for you. In particular:
//        * CommandBuffer::submit() does NOT call execute(); it goes straight to
//          onSubmit().
//        * begin() clears the vector, so an execute() that was skipped for a frame
//          loses that frame's work silently.
//
//   The only backend hooks remain onBegin / onEnd / onSubmit / onWait. There is
//   still no hook for the individual commands, so a Vulkan subclass that
//   implements those four correctly will begin and end a real VkCommandBuffer and
//   submit it -- with an EMPTY command list unless something calls execute() with
//   a sink installed.
//
// Consequence for this class: onBegin/onEnd/onSubmit/onWait are implemented as
// real Vulkan, and the caller must install a CommandSink (see set_command_sink)
// AND call execute() after recording and before end(), or the recorded work never
// reaches the GPU. VulkanRenderer::onEndFrame sidesteps all of this by recording
// vkCmdBeginRenderPass / vkCmdClearAttachments / vkCmdEndRenderPass by hand into
// command_buffers_[current_frame_] and never using this class at all -- which
// remains a valid integration, just not the one that exercises the base API.
//
// The concrete diff that removes the footgun (auto-replay on submit) is in the
// integration report. It is deliberately NOT applied here: common/*.cpp are
// shared files and modifying them was out of scope for this task.
//
// ---------------------------------------------------------------------------
// THREADING
// ---------------------------------------------------------------------------
// CommandBuffer::begin/end/submit call the on* hooks WHILE HOLDING THE BASE'S
// OWN MUTEX. These hooks must never call back into a base public method
// (CommandBuffer::reset(), wait(), submit(), ...): the base mutex is not
// recursive, so the second acquisition self-deadlocks. Only mutex_ below may be
// taken, and it is never held across vkQueueSubmit or vkWaitForFences -- both
// can block for a full frame, and holding a lock across either is how a renderer
// deadlocks against its own present thread.
//
// INTEGRATOR NOTE: vulkan_renderer.h still contains an inline stub of this class
// name (together with VulkanFramebufferManager / VulkanStateManager /
// VulkanSyncManager / VulkanResourcePool / VulkanProfiler). Those stubs have to
// be deleted from vulkan_renderer.h and replaced with an include of this header,
// otherwise the two definitions of `copper::VulkanCommandBuffer` collide.
class VulkanCommandBuffer : public CommandBuffer {
public:
    explicit VulkanCommandBuffer(VulkanRenderer* renderer);
    ~VulkanCommandBuffer() override;

    VulkanCommandBuffer(const VulkanCommandBuffer&) = delete;
    VulkanCommandBuffer& operator=(const VulkanCommandBuffer&) = delete;

    // Selects the frame's command buffer and submits to that frame's fence.
    //
    // WHY a setter rather than deriving it from initialize(): the base's
    // initialize() is not virtual, so a backend has no init hook, and the renderer's
    // command_buffers_ vector is not allocated until create_sync_objects() runs.
    // The renderer calls this from onBeginFrame once the vector is populated.
    void set_command_buffer(VkCommandBuffer command_buffer, uint32_t frame_index);
    VkCommandBuffer command_buffer() const;
    uint32_t frame_index() const;

    // Wires in the sync manager whose pending waits/signals onSubmit builds the
    // VkSubmitInfo from. Optional: with no sync manager, onSubmit submits with an
    // empty wait/signal list, which is correct for a batch that needs none but
    // WRONG for one that does -- an unsynchronised swapchain write is a real
    // visual bug, not a benign omission.
    void set_sync_manager(VulkanSyncManager* sync_manager);

    // When true, submit() does NOT submit to the queue: VulkanRenderer already
    // owns the frame's acquire/submit/present sequence, and a second
    // vkQueueSubmit for the same command buffer would put it in flight twice,
    // which Vulkan does not permit. In that mode submit() only reports success
    // and the renderer submits.
    void set_renderer_submit(bool renderer_submit);
    bool renderer_submit() const;

    // Installs a CommandSink for the commands recorded through the base API
    // (draw(), bindPipeline(), ...). This class does not implement CommandSink
    // itself; the integrator provides a VulkanCommandSink that translates into
    // vkCmd* against command_buffer() here.
    //
    // THE CONTRACT, and it is not optional:
    //   begin()
    //   ...record via draw() / bindPipeline() / ...   (they land in the base's
    //                                                    vector, NOT in Vulkan)
    //   execute()        <-- replays the vector through the sink into the
    //                          VkCommandBuffer. Without this call the recorded
    //                          work is silently dropped.
    //   end()
    //   submit()
    //
    // So a caller that installs a sink but forgets execute() gets an empty batch
    // submitted and no error anywhere. See the class comment.
    void set_command_sink(CommandSink* sink);

    // Bounded timeout for onWait, in nanoseconds. Never UINT64_MAX by default:
    // onWait() blocks the calling thread, and an infinite wait on a fence that a
    // lost submission will never signal hangs the render thread for good. The
    // default comes from RendererConfig::frameTimeoutMs.
    void set_wait_timeout_ns(uint64_t timeout_ns);
    uint64_t wait_timeout_ns() const;

    // True once vkEndCommandBuffer has succeeded and the batch is ready to go.
    bool is_recording() const;
    bool is_submitted() const;

    // Number of times vkQueueSubmit failed since the last reset. Non-zero here
    // with a clean log means a driver error the renderer should surface.
    uint32_t submit_failure_count() const;

protected:
    bool onBegin() override;
    void onEnd() override;
    bool onSubmit() override;
    void onWait() override;

private:
    // Resolves the VkFence for the current frame: the renderer's per-frame
    // in_flight_fences_ entry when it exists, otherwise a private fence this
    // object creates. Caller holds mutex_.
    VkFence resolve_fence_locked();
    bool create_private_fence_locked();

    VulkanRenderer* renderer_ = nullptr;
    VulkanSyncManager* sync_manager_ = nullptr;
    bool renderer_submit_ = false;

    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    uint32_t frame_index_ = 0;

    // Only used when the renderer exposes no per-frame fence (for example before
    // create_sync_objects() has run). Tracked separately because freeing it is
    // this object's responsibility in that case and not otherwise.
    VkFence private_fence_ = VK_NULL_HANDLE;
    bool owns_private_fence_ = false;

    uint64_t wait_timeout_ns_ = 0;      // 0 => derive from RendererConfig::frameTimeoutMs
    bool recording_ = false;
    bool submitted_ = false;
    uint32_t submit_failures_ = 0;

    // The base calls the on* hooks while holding ITS lock; this only guards the
    // fields above. Never held across vkQueueSubmit or vkWaitForFences.
    mutable std::mutex mutex_;
};

} // namespace copper