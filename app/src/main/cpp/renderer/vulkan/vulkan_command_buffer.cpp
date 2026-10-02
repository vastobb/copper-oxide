#include "vulkan_command_buffer.h"

// The VulkanRenderer accessors used below (device(), graphicsQueue(),
// commandPool(), framesInFlight(), inFlightFences(), commandBuffers(),
// getConfig()) are the ones the integrator must add to VulkanRenderer; see the
// class comment in the header.
#include "vulkan_renderer.h"

#include "vulkan_sync_manager.h"

#include <android/log.h>

#include <cstdint>
#include <vector>

#define LOG_TAG "CopperOxide-VK"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

namespace {

const char* result_name(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        default: return "unexpected VkResult";
    }
}

// Nanoseconds, with UINT64_MAX (an "infinite" wait in both APIs) mapped to a
// bounded value. See the header on why onWait must never wait forever.
uint64_t clamp_timeout_ns(uint64_t requested_ns, uint64_t configured_ms) {
    uint64_t fallback_ns = configured_ms * 1'000'000ULL;
    if (fallback_ns == 0) {
        // RendererConfig::frameTimeoutMs is 5000 in practice; this only fires if a
        // config left it at 0, in which case an unbounded wait is the worst
        // possible choice.
        fallback_ns = 5'000'000'000ULL;
    }
    if (requested_ns == UINT64_MAX || requested_ns == 0) {
        return fallback_ns;
    }
    return requested_ns;
}

} // namespace

VulkanCommandBuffer::VulkanCommandBuffer(VulkanRenderer* renderer) : renderer_(renderer) {}

VulkanCommandBuffer::~VulkanCommandBuffer() {
    const VkDevice device = (renderer_ != nullptr) ? renderer_->device() : VK_NULL_HANDLE;
    if (device == VK_NULL_HANDLE) {
        // Device gone, so the fence died with it.
        return;
    }
    if (owns_private_fence_ && private_fence_ != VK_NULL_HANDLE) {
        // Only a fence this object created is destroyed here. The renderer's
        // in_flight_fences_ are owned by VulkanRenderer::shutdown(); touching
        // them would be a double free.
        vkDestroyFence(device, private_fence_, nullptr);
        private_fence_ = VK_NULL_HANDLE;
        owns_private_fence_ = false;
    }
}

void VulkanCommandBuffer::set_command_buffer(VkCommandBuffer command_buffer, uint32_t frame_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    command_buffer_ = command_buffer;
    frame_index_ = frame_index;
    recording_ = false;
    submitted_ = false;
}

VkCommandBuffer VulkanCommandBuffer::command_buffer() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return command_buffer_;
}

uint32_t VulkanCommandBuffer::frame_index() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frame_index_;
}

void VulkanCommandBuffer::set_sync_manager(VulkanSyncManager* sync_manager) {
    std::lock_guard<std::mutex> lock(mutex_);
    sync_manager_ = sync_manager;
}

void VulkanCommandBuffer::set_command_sink(CommandSink* sink) {
    // Called once at init, not from an on* hook, so forwarding straight to the
    // base setter is fine. It must not be called with mutex_ held: setCommandSink
    // takes the base's lock, and taking base-then-ours here would invert the
    // lock order the rest of this class maintains (on* hooks run under the base
    // lock and then take mutex_).
    setCommandSink(sink);
    if (sink != nullptr) {
        // One warning is enough: install the sink once and this fires once.
        LOGW("a CommandSink is installed, but the base still accumulates every command in a"
             " vector. Call execute() after recording and before end(), or the recorded work"
             " reaches no VkCommandBuffer");
    }
}

void VulkanCommandBuffer::set_wait_timeout_ns(uint64_t timeout_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    wait_timeout_ns_ = timeout_ns;
}

uint64_t VulkanCommandBuffer::wait_timeout_ns() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (wait_timeout_ns_ != 0) {
        return wait_timeout_ns_;
    }
    if (renderer_ == nullptr) {
        return clamp_timeout_ns(0, 0);
    }
    return clamp_timeout_ns(0, renderer_->getConfig().frameTimeoutMs);
}

bool VulkanCommandBuffer::is_recording() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recording_;
}

bool VulkanCommandBuffer::is_submitted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return submitted_;
}

uint32_t VulkanCommandBuffer::submit_failure_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return submit_failures_;
}

bool VulkanCommandBuffer::create_private_fence_locked() {
    if (private_fence_ != VK_NULL_HANDLE) {
        return true;
    }
    const VkDevice device = (renderer_ != nullptr) ? renderer_->device() : VK_NULL_HANDLE;
    if (device == VK_NULL_HANDLE) {
        return false;
    }
    VkFenceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    // Unsigned at creation. VulkanRenderer::create_sync_objects() creates its own
    // fences SIGNALED so the very first wait is a no-op, and this fallback only
    // runs when the renderer has no fence for this frame. Signalling here would
    // be wrong on every reuse, not just the first: onWait would pass
    // immediately and report a batch as finished before the GPU had seen it.
    // onSubmit resets immediately before each submit instead.
    const VkResult result = vkCreateFence(device, &create_info, nullptr, &private_fence_);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateFence failed: %s", result_name(result));
        private_fence_ = VK_NULL_HANDLE;
        return false;
    }
    owns_private_fence_ = true;
    return true;
}

VkFence VulkanCommandBuffer::resolve_fence_locked() {
    if (renderer_ != nullptr) {
        const std::vector<VkFence>& fences = renderer_->inFlightFences();
        if (frame_index_ < fences.size() && fences[frame_index_] != VK_NULL_HANDLE) {
            return fences[frame_index_];
        }
    }
    // No renderer fence for this frame. Fall back to a private one rather than
    // submitting with VK_NULL_HANDLE (which is legal but leaves the batch with no
    // completion signal at all).
    create_private_fence_locked();
    return private_fence_;
}

// ---------------------------------------------------------------------------
// base hooks
// ---------------------------------------------------------------------------

bool VulkanCommandBuffer::onBegin() {
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        command_buffer = command_buffer_;
        recording_ = false;
        submitted_ = false;
    }

    if (command_buffer == VK_NULL_HANDLE) {
        LOGE("begin() with no VkCommandBuffer; call set_command_buffer(cb, frame) from"
             " VulkanRenderer::onBeginFrame first");
        return false;
    }
    const VkDevice device = (renderer_ != nullptr) ? renderer_->device() : VK_NULL_HANDLE;
    if (device == VK_NULL_HANDLE) {
        LOGE("begin() with no VkDevice");
        return false;
    }

    // Reset before beginning. A command buffer left EXECUTABLE cannot be
    // re-recorded, and vkBeginCommandBuffer on one is a usage error rather than
    // an implicit reset -- so this step is mandatory for frames 2..N.
    //
    // Note this is NOT vkResetCommandPool: the pool is shared by every frame in
    // flight, so resetting it would discard the batches still queued for the other
    // frames.
    const VkResult reset_result = vkResetCommandBuffer(command_buffer, 0);
    if (reset_result != VK_SUCCESS) {
        LOGE("vkResetCommandBuffer failed: %s", result_name(reset_result));
        return false;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    // ONE_TIME_SUBMIT: this buffer is recorded, submitted and forgotten within one
    // frame. It lets the driver pick the optimal layout instead of preserving a
    // restorable state it will never restore from.
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    const VkResult result = vkBeginCommandBuffer(command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        LOGE("vkBeginCommandBuffer failed: %s", result_name(result));
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    recording_ = true;
    return true;
}

void VulkanCommandBuffer::onEnd() {
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        command_buffer = command_buffer_;
        if (!recording_) {
            // end() without a successful begin(). Nothing to close; the base has
            // already flipped its own recording flag.
            return;
        }
        recording_ = false;
    }

    const VkResult result = vkEndCommandBuffer(command_buffer);
    if (result != VK_SUCCESS) {
        LOGE("vkEndCommandBuffer failed: %s", result_name(result));
        return;
    }
}

void VulkanCommandBuffer::set_renderer_submit(bool renderer_submit) {
    std::lock_guard<std::mutex> lock(mutex_);
    renderer_submit_ = renderer_submit;
}

bool VulkanCommandBuffer::renderer_submit() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return renderer_submit_;
}

bool VulkanCommandBuffer::onSubmit() {
    // Snapshot under the lock, then release it. vkQueueSubmit can block when the
    // queue is full, and holding this object's mutex across it would deadlock any
    // other thread that touched command_buffer() (the renderer's present path does)
    // while the submission was in flight.
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VulkanSyncManager* sync_manager = nullptr;
    VkFence fence = VK_NULL_HANDLE;
    uint32_t frame_index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        command_buffer = command_buffer_;
        sync_manager = sync_manager_;
        fence = resolve_fence_locked();
        frame_index = frame_index_;
    }

    bool renderer_submit = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        renderer_submit = renderer_submit_;
    }
    if (renderer_submit) {
        // The renderer submits this frame itself, right after the recorded work
        // is closed. Submitting here as well would use the same command buffer
        // twice in flight, which Vulkan does not permit.
        return true;
    }
    if (renderer_ == nullptr || renderer_->device() == VK_NULL_HANDLE) {
        LOGE("submit() with no live Vulkan device");
        return false;
    }
    if (command_buffer == VK_NULL_HANDLE) {
        LOGE("submit() with no VkCommandBuffer");
        return false;
    }
    if (sync_manager == nullptr) {
        // A batch that needs no waits and no signals is fine unsynchronised; one
        // that does is a real visual bug. This class cannot tell the two apart,
        // so it says so rather than quietly submitting an unsynchronised swapchain
        // write. The integrator should call set_sync_manager() once, at init.
        LOGW("submit() without a sync manager (call set_sync_manager): this batch carries no wait"
             " or signal semaphores, which is correct only if it genuinely needs none");
    }

    // The sync manager holds the batch's dependencies recorded through
    // submitWaitSemaphores()/submitSignalSemaphores(). The returned VkSubmitInfo
    // borrows the sync manager's internal storage and stays valid until
    // clear_pending_submit() below -- which is why the clear must happen AFTER
    // vkQueueSubmit and not before it.
    VkSubmitInfo submit_info{};
    if (sync_manager != nullptr) {
        submit_info = sync_manager->build_submit_info(command_buffer);
    } else {
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer;
    }

    if (fence != VK_NULL_HANDLE) {
        // Reset immediately before the submit, and only now. Resetting earlier
        // (at begin, say) would leave a fence that nothing signals if the submit
        // never happened, turning the next frame's wait into a hang.
        const VkResult reset_result = vkResetFences(renderer_->device(), 1, &fence);
        if (reset_result != VK_SUCCESS) {
            LOGE("vkResetFences(frame %u) failed: %s", frame_index, result_name(reset_result));
            std::lock_guard<std::mutex> lock(mutex_);
            ++submit_failures_;
            return false;
        }
    }

    const VkResult result =
        vkQueueSubmit(renderer_->graphicsQueue(), 1, &submit_info, fence);
    if (result != VK_SUCCESS) {
        LOGE("vkQueueSubmit(frame %u) failed: %s", frame_index, result_name(result));
        std::lock_guard<std::mutex> lock(mutex_);
        ++submit_failures_;
        return false;
    }

    // The pending waits and signals were consumed by this submit. Leaving them in
    // place would silently double-signal every semaphore on the next one.
    if (sync_manager != nullptr) {
        sync_manager->clear_pending_submit();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    submitted_ = true;
    return true;
}

void VulkanCommandBuffer::onWait() {
    VkFence fence = VK_NULL_HANDLE;
    uint64_t timeout_ns = 0;
    uint32_t frame_index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fence = resolve_fence_locked();
        if (wait_timeout_ns_ != 0) {
            timeout_ns = wait_timeout_ns_;
        } else if (renderer_ != nullptr) {
            timeout_ns = clamp_timeout_ns(0, renderer_->getConfig().frameTimeoutMs);
        } else {
            timeout_ns = clamp_timeout_ns(0, 0);
        }
        frame_index = frame_index_;
    }

    if (fence == VK_NULL_HANDLE) {
        return;
    }
    if (renderer_ == nullptr || renderer_->device() == VK_NULL_HANDLE) {
        return;
    }

    // WHY no reset before waiting: vkResetFences is illegal on a fence that is in
    // use by a pending submission, and "is it in use" is exactly what this call
    // is trying to find out. onSubmit already resets immediately before every
    // submit, so the fence is unsignalled and pending for every wait that follows
    // one. A wait issued before any submit hits an unsignalled fence and times
    // out, which is the honest answer: nothing has been submitted yet.

    // Timeout is bounded (see the header): a fence that a lost or failed
    // submission will never signal must surface as a log line, not as a render
    // thread that never returns.
    const VkResult result = vkWaitForFences(renderer_->device(), 1, &fence, VK_TRUE, timeout_ns);
    if (result == VK_TIMEOUT) {
        LOGW("wait(frame %u) timed out after %.1f ms; the GPU is behind or the submit failed",
             frame_index, static_cast<double>(timeout_ns) / 1'000'000.0);
    } else if (result != VK_SUCCESS) {
        LOGE("vkWaitForFences(frame %u) failed: %s", frame_index, result_name(result));
    }
}

} // namespace copper