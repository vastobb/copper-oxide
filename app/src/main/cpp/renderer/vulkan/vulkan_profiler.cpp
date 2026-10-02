#include "vulkan_profiler.h"

// The VulkanRenderer accessors used below (device(), physicalDevice(),
// graphicsQueueFamily()) are the ones the integrator must add to VulkanRenderer;
// see the class comment in the header.
#include "vulkan_renderer.h"

#include <android/log.h>

#include <algorithm>
#include <vector>

#define LOG_TAG "CopperOxide-VK"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

namespace {

const char* mode_name(VulkanProfiler::Mode mode) {
    switch (mode) {
        case VulkanProfiler::Mode::Uninitialized: return "uninitialized";
        case VulkanProfiler::Mode::Unsupported: return "unsupported";
        case VulkanProfiler::Mode::CpuOnly: return "cpu-only";
        case VulkanProfiler::Mode::TimestampQueries: return "timestamp-queries";
        default: return "unknown";
    }
}

// vkGetQueryPoolResults returns a raw result code; the interesting ones are
// VK_SUCCESS (available) and VK_NOT_READY (still in flight). Everything else is a
// real error and gets logged with its number, because there is no portable
// name for the rest of the enum.
const char* query_result_name(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        default: return "unexpected VkResult";
    }
}

} // namespace

VulkanProfiler::VulkanProfiler(VulkanRenderer* renderer) : renderer_(renderer) {
    history_.reserve(k_ring_frames);
    slot_state_.assign(k_ring_frames, 0);
    slot_frame_number_.assign(k_ring_frames, 0);
}

VulkanProfiler::~VulkanProfiler() {
    if (renderer_ == nullptr || renderer_->device() == VK_NULL_HANDLE ||
        query_pool_ == VK_NULL_HANDLE) {
        // Device already gone, so the pool died with it. Destroying it now would
        // be a use-after-free on a dead VkDevice.
        return;
    }
    // WHY the idle wait here: a query pool cannot be destroyed while a submitted
    // batch still references its queries. This runs in the destructor at
    // shutdown, outside every hook, and outside mutex_ -- the documented deadlock
    // class is holding the lock across a blocking call, not blocking during
    // teardown when no other thread can be inside these hooks.
    vkDeviceWaitIdle(renderer_->device());
    vkDestroyQueryPool(renderer_->device(), query_pool_, nullptr);
    query_pool_ = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// capability probe
// ---------------------------------------------------------------------------

bool VulkanProfiler::probe_support() {
    std::lock_guard<std::mutex> lock(mutex_);
    return probe_support_locked();
}

bool VulkanProfiler::probe_support_locked() {
    if (support_probed_) {
        return mode_ == Mode::TimestampQueries && query_pool_ != VK_NULL_HANDLE;
    }
    support_probed_ = true;

    const VkDevice device = (renderer_ != nullptr) ? renderer_->device() : VK_NULL_HANDLE;
    const VkPhysicalDevice physical_device =
        (renderer_ != nullptr) ? renderer_->physicalDevice() : VK_NULL_HANDLE;
    if (device == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE) {
        mode_ = Mode::Unsupported;
        LOGW("profiler: no VkDevice/physical device; GPU timing stays off. Mode = %s",
             mode_name(mode_));
        return false;
    }

    // --- axis 1: is there a clock at all? ---------------------------------
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    timestamp_period_ns_ = properties.limits.timestampPeriod;
    if (timestamp_period_ns_ == 0.0f) {
        mode_ = Mode::Unsupported;
        LOGI("profiler: timestampPeriod is 0; this device exposes no usable GPU clock."
             " Mode = %s", mode_name(mode_));
        return false;
    }

    // --- axis 2: does the queue family we submit to carry timestamps? ------
    const uint32_t graphics_family = renderer_->graphicsQueueFamily();
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
    if (family_count > 0) {
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
        if (graphics_family < family_count) {
            timestamp_valid_bits_ = families[graphics_family].timestampValidBits;
        }
    }
    if (timestamp_valid_bits_ == 0) {
        mode_ = Mode::Unsupported;
        LOGI("profiler: graphics queue family %u reports 0 valid timestamp bits; GPU timestamps"
             " are unavailable. Mode = %s",
             graphics_family, mode_name(mode_));
        return false;
    }

    // --- axis 3: is the feature actually ENABLED on this VkDevice? --------
    // This is the decisive check. vkCreateQueryPool succeeds without
    // timestampQuery and the queries then return undefined values, so a pool
    // existing proves nothing. The property query only reports what the PHYSICAL
    // device could do; the enabled-features struct is what the logical device
    // will honour.
    //
    // The renderer creates its device with vkGetPhysicalDeviceFeatures, so
    // checking the legacy struct is correct here. An integrator who moves to
    // VkPhysicalDeviceFeatures2 must extend this with vkGetPhysicalDeviceFeatures2
    // and walk pNext for VkPhysicalDeviceVulkan12Features::
    // timestampComputeAndGraphics.
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(physical_device, &features);
    if (features.timestampQuery != VK_TRUE) {
        mode_ = Mode::CpuOnly;
        LOGW("profiler: VkPhysicalDeviceFeatures::timestampQuery is not enabled on this VkDevice,"
             " so GPU timing falls back to CPU timing. Mode = %s", mode_name(mode_));
        return false;
    }

    mode_ = Mode::TimestampQueries;
    if (!create_query_pool_locked()) {
        // A pool that will not create means the mode cannot work. Stay usable as
        // a CPU timer instead of pretending timestamps exist.
        mode_ = Mode::CpuOnly;
        return false;
    }
    LOGI("profiler: timestamp queries enabled (period=%.3f ns, valid bits=%u, pool queries=%u,"
         " ring frames=%u). Mode = %s",
         static_cast<double>(timestamp_period_ns_), timestamp_valid_bits_, pool_query_count_,
         k_ring_frames, mode_name(mode_));
    return true;
}

bool VulkanProfiler::create_query_pool_locked() {
    if (query_pool_ != VK_NULL_HANDLE) {
        return true;
    }
    if (pool_create_failed_) {
        // One failure is enough; retrying every frame would only spam the log and
        // cannot succeed without the device changing underneath us.
        return false;
    }
    pool_query_count_ = std::min(k_ring_frames * k_queries_per_frame, k_max_queries_per_pool);

    VkQueryPoolCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    create_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    create_info.queryCount = pool_query_count_;

    const VkResult result =
        vkCreateQueryPool(renderer_->device(), &create_info, nullptr, &query_pool_);
    if (result != VK_SUCCESS) {
        pool_create_failed_ = true;
        LOGE("vkCreateQueryPool(%u queries) failed: %s; GPU timing disabled", pool_query_count_,
             query_result_name(result));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// readback
// ---------------------------------------------------------------------------

void VulkanProfiler::collect_gpu_times() {
    std::lock_guard<std::mutex> lock(mutex_);
    collect_gpu_times_locked();
}

void VulkanProfiler::collect_gpu_times_locked() {
    if (renderer_ == nullptr || renderer_->device() == VK_NULL_HANDLE ||
        query_pool_ == VK_NULL_HANDLE) {
        return;
    }

    for (uint32_t slot = 0; slot < k_ring_frames; ++slot) {
        if (slot_state_[slot] != 1) {
            continue;  // idle, or currently being written by this frame
        }
        GpuFrameTiming timing;
        if (!read_slot_locked(slot, timing)) {
            // Not ready. Leave it pending and retry next frame: a GPU a few
            // frames behind is normal. Blocking here would be the wrong cure (see
            // the header), so an indefinitely stuck slot is simply overwritten
            // when the ring wraps and counted as dropped.
            continue;
        }

        slot_state_[slot] = 0;
        if (history_.size() < k_ring_frames) {
            history_.push_back(timing);
        } else {
            history_[history_head_] = timing;
            history_head_ = (history_head_ + 1) % k_ring_frames;
        }
        if (timing.valid) {
            ++samples_recorded_;
        } else {
            ++samples_dropped_;
        }
    }
}

bool VulkanProfiler::read_slot_locked(uint32_t slot, GpuFrameTiming& out) {
    const uint32_t first_query = slot * k_queries_per_frame;
    if (first_query + k_queries_per_frame > pool_query_count_) {
        return true;  // nothing there; treat as a completed empty sample
    }

    uint64_t ticks[k_queries_per_frame] = {0, 0};
    // VK_QUERY_RESULT_64_BIT: timestamps are 64-bit, and reading them as 32-bit
    // wraps silently on a long frame. No WAIT bit: this must never block.
    const VkResult result = vkGetQueryPoolResults(
        renderer_->device(), query_pool_, first_query, k_queries_per_frame, sizeof(ticks), ticks,
        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);

    out.frame_number = slot_frame_number_[slot];
    out.gpu_time_ms = 0.0;
    out.valid = false;

    if (result == VK_NOT_READY) {
        return false;
    }
    if (result != VK_SUCCESS) {
        LOGW("vkGetQueryPoolResults(slot=%u) failed: %s", slot, query_result_name(result));
        return true;  // stop retrying this slot; it is reported as invalid
    }
    if (ticks[1] < ticks[0]) {
        // End before start. Either the batch has not run (both are still 0) or the
        // slot was reused before the old batch landed. Not an error, just no
        // sample -- counted as dropped so the ratio stays honest.
        return true;
    }

    const uint64_t delta = ticks[1] - ticks[0];
    out.gpu_time_ms =
        (static_cast<double>(delta) * static_cast<double>(timestamp_period_ns_)) / 1'000'000.0;
    out.valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// accessors
// ---------------------------------------------------------------------------

void VulkanProfiler::set_command_buffer(VkCommandBuffer command_buffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    command_buffer_ = command_buffer;
}

VkCommandBuffer VulkanProfiler::command_buffer() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return command_buffer_;
}

VulkanProfiler::Mode VulkanProfiler::mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mode_;
}

bool VulkanProfiler::timestamps_supported() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mode_ == Mode::TimestampQueries && query_pool_ != VK_NULL_HANDLE;
}

float VulkanProfiler::timestamp_period_ns() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return timestamp_period_ns_;
}

double VulkanProfiler::gpu_time_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (history_.empty()) {
        return 0.0;
    }
    // Oldest-first ring: the newest entry is the last pushed slot while filling,
    // and history_head_ once wrapped.
    const size_t newest = (history_.size() < k_ring_frames) ? history_.size() - 1 : history_head_;
    const GpuFrameTiming& latest = history_[newest];
    return latest.valid ? latest.gpu_time_ms : 0.0;
}

std::vector<VulkanProfiler::GpuFrameTiming> VulkanProfiler::gpu_time_history(uint32_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<GpuFrameTiming> out;
    const size_t total = history_.size();
    if (total == 0) {
        return out;
    }
    const size_t wanted = std::min<size_t>(count == 0 ? total : count, total);
    out.reserve(wanted);

    // Index forward from the oldest slot so the caller gets chronological order
    // without needing to know how the ring is arranged.
    const size_t oldest = (total < k_ring_frames) ? 0 : history_head_;
    for (size_t i = 0; i < wanted; ++i) {
        const size_t index = (oldest + total - wanted + i) % total;
        out.push_back(history_[index]);
    }
    return out;
}

uint64_t VulkanProfiler::gpu_samples_recorded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_recorded_;
}

uint64_t VulkanProfiler::gpu_samples_dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_dropped_;
}

// ---------------------------------------------------------------------------
// base hooks
// ---------------------------------------------------------------------------

void VulkanProfiler::begin_gpu_frame(uint64_t frame_number) {
    onBeginFrame(frame_number);
}

void VulkanProfiler::end_gpu_frame(uint64_t frame_number) {
    // The frame-timing arguments are the base's own bookkeeping and are already
    // in its ring by the time any hook could run; passing zeros here is correct
    // and is ignored either way.
    onEndFrame(frame_number, 0.0, 0.0, 0.0, 0);
}

void VulkanProfiler::onBeginFrame(uint64_t frame_number) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!support_probed_) {
        probe_support_locked();
    }
    if (mode_ != Mode::TimestampQueries || query_pool_ == VK_NULL_HANDLE) {
        return;
    }

    // Reap whatever finished while the previous frame was in flight, before a
    // slot gets reused.
    collect_gpu_times_locked();

    if (command_buffer_ == VK_NULL_HANDLE) {
        if (!waiting_for_command_buffer_logged_) {
            waiting_for_command_buffer_logged_ = true;
            LOGI("profiler: no VkCommandBuffer bound; call set_command_buffer(cb) between"
                 " vkBeginCommandBuffer and vkEndCommandBuffer to enable GPU timing");
        }
        return;
    }

    write_slot_ = static_cast<uint32_t>(frame_number % k_ring_frames);
    if (slot_state_[write_slot_] == 1) {
        // The previous occupant never came back. Overwriting it is correct (the
        // query must be reset anyway) but it is worth counting so a persistently
        // stuck GPU shows up as a dropped-sample ratio rather than silently
        // reporting stale numbers.
        ++samples_dropped_;
    }
    writing_ = true;
    slot_state_[write_slot_] = 2;
    slot_frame_number_[write_slot_] = frame_number;

    // Reset before writing: a timestamp query must be reset before reuse unless
    // hostQueryReset is enabled. Doing it inside the command buffer keeps the
    // ordering correct without a host round trip.
    vkCmdResetQueryPool(command_buffer_, query_pool_, write_slot_ * k_queries_per_frame,
                        k_queries_per_frame);
    vkCmdWriteTimestamp(command_buffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query_pool_,
                        write_slot_ * k_queries_per_frame);
}

void VulkanProfiler::onEndFrame(uint64_t frame_number, double frame_time_ms, double cpu_time_ms,
                                double gpu_time_ms, uint32_t draw_calls) {
    // WHY these are ignored: the base already recorded them into its own history
    // before this hook ran. Feeding them back would double-count. The GPU number
    // this class produces is deliberately kept out of Profiler::Stats because
    // the base's endFrame() ran before the GPU had finished the batch.
    (void)frame_time_ms;
    (void)cpu_time_ms;
    (void)gpu_time_ms;
    (void)draw_calls;
    (void)frame_number;

    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ != Mode::TimestampQueries || query_pool_ == VK_NULL_HANDLE || !writing_ ||
        command_buffer_ == VK_NULL_HANDLE) {
        return;
    }
    writing_ = false;

    vkCmdWriteTimestamp(command_buffer_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool_,
                        write_slot_ * k_queries_per_frame + 1);
    // State 1 = written, awaiting readback. Not read here: the GPU has not run
    // this batch yet, so any read would be VK_NOT_READY.
    slot_state_[write_slot_] = 1;
}

} // namespace copper