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

#include "profiler.h"

namespace copper {

class VulkanRenderer;

// Vulkan backend for Profiler.
//
// ---------------------------------------------------------------------------
// THE BASE NEVER CALLS onBeginFrame / onEndFrame -- verified, not assumed
// ---------------------------------------------------------------------------
// Profiler::beginFrame() and Profiler::endFrame() in common/profiler.cpp push the
// frame data into the base's own ring and update the base's stats. Neither one
// calls its on* hook. The two pure virtuals declared in common/profiler.h are
// therefore unreachable through the public API today, for every backend.
//
// So: the hooks ARE implemented (they are what a fixed base needs), and
// begin_gpu_frame() / end_gpu_frame() below are the same code reached directly,
// so the integrator can get real GPU timings today without patching common/.
// The diff that makes Profiler::beginFrame/endFrame call the hooks is in the
// integration report.
//
// ---------------------------------------------------------------------------
// REAL GPU TIMING VIA TIMESTAMP QUERIES
// ---------------------------------------------------------------------------
// The base Profiler measures wall-clock CPU time on the host; it has no way to
// learn how long the GPU was actually busy, which is the number that decides
// whether a frame is GPU bound. This backend adds VkQueryPool timestamp queries
// when the device supports them.
//
// Support is required on THREE axes, all checked in probe_support():
//   1. VkPhysicalDeviceProperties::limits.timestampPeriod != 0. A zero period
//      means the implementation has no usable clock (allowed by the spec) and
//      every timestamp would be meaningless.
//   2. The graphics queue family reporting timestampValidBits > 0.
//   3. The enabled device feature: VkPhysicalDeviceFeatures::timestampQuery.
//
// All three must hold. All three failing is not an error: Mode::CpuOnly is a
// legitimate answer, and the active mode is logged once because "the profiler
// shows 0 ms GPU time" is only explicable by knowing which mode is live.
//
// ---------------------------------------------------------------------------
// WHY THE QUERY POOL IS NOT REBUILT EVERY FRAME
// ---------------------------------------------------------------------------
// vkCreateQueryPool/vkDestroyQueryPool per frame is a driver allocation per
// frame, and destroying a pool while its timestamps are still in flight is a
// use-after-free. The pool here is created ONCE, on first use, sized for a
// bounded ring of frames, and each frame's slot is reused round-robin. A slot is
// only rewritten k_ring_frames frames later, by which time the earlier batch has
// normally completed; if it has not, the sample is skipped rather than waited on.
//
// The timestamp writes are recorded INTO a command buffer, so they need a real
// VkCommandBuffer. That is what set_command_buffer() is for; the profiler does
// not open one itself. See INTEGRATION CONTRACT below.
//
// INTEGRATION CONTRACT:
//   cb->begin()   -> profiler->set_command_buffer(cb, frame_index)
//                    profiler->onBeginFrame()  writes the TOP_OF_PIPE timestamp
//   ...record the frame...
//   cb->end()     -> profiler->onEndFrame()    writes the BOTTOM_OF_PIPE timestamp
//   cb->submit()  -> vulkan_queue_submit()
//   next frame    -> collect_gpu_times() reaps whatever finished
//   read back     -> gpu_time_ms() / gpu_time_history()
//
// WHAT THIS CLASS DELIBERATELY DOES NOT DO:
//   * It does not open or close its own command buffer per frame; the renderer
//     already owns one per frame in flight and a second would need its own pool
//     allocation, reset and fence.
//   * It does not submit anything.
//   * It does not wait on a fence to read timestamps. A per-frame
//     vkWaitForFences here would serialise the GPU against the CPU and the queue
//     against itself -- turning a measurement into the bottleneck. Reading N
//     frames late is the standard trade and costs one ring slot of precision.
//
// INTEGRATOR NOTE: vulkan_renderer.h still contains an inline stub of this class
// name (together with VulkanCommandBuffer / VulkanSyncManager /
// VulkanResourcePool). Those stubs have to be deleted from vulkan_renderer.h and
// replaced with an include of this header, otherwise the two definitions of
// `copper::VulkanProfiler` collide.
class VulkanProfiler : public Profiler {
public:
    // One GPU frame measurement. `valid` is false when the device has no usable
    // clock or the query was not ready in time; gpu_time_ms is then meaningless
    // (0.0) and must not be averaged in.
    struct GpuFrameTiming {
        uint64_t frame_number = 0;
        double gpu_time_ms = 0.0;
        bool valid = false;
    };

    // Which implementation is live. Reported by mode() and logged once, because
    // a 0 ms GPU time is only explicable by knowing which mode is active.
    enum class Mode {
        Uninitialized,   // probe_support() has not run yet
        Unsupported,     // device exposes no usable timestamp clock
        CpuOnly,         // frame timing only, no GPU queries
        TimestampQueries // real VkQueryPool timestamps
    };

    explicit VulkanProfiler(VulkanRenderer* renderer);
    ~VulkanProfiler() override;

    VulkanProfiler(const VulkanProfiler&) = delete;
    VulkanProfiler& operator=(const VulkanProfiler&) = delete;

    // Capability probe + lazy query-pool creation. Idempotent and cached, so the
    // integrator may call it from initializeManagers() to get the log line up
    // front rather than at frame 1. Returns true iff mode is TimestampQueries.
    bool probe_support();

    Mode mode() const;
    bool timestamps_supported() const;

    // Nanoseconds per timestamp tick. 0.0 when unsupported. A tick delta times
    // this is the GPU duration in nanoseconds.
    float timestamp_period_ns() const;

    // Binds the command buffer the per-frame timestamps are recorded into. Call
    // it after vkBeginCommandBuffer and clear it after vkEndCommandBuffer.
    void set_command_buffer(VkCommandBuffer command_buffer);
    VkCommandBuffer command_buffer() const;

    double gpu_time_ms() const;
    std::vector<GpuFrameTiming> gpu_time_history(uint32_t count) const;
    uint64_t gpu_samples_recorded() const;
    uint64_t gpu_samples_dropped() const;

    // Reads back whatever timestamps have finished. Non-blocking (no
    // VK_QUERY_RESULT_WAIT_BIT), so it never stalls the render thread on a busy
    // queue. Called automatically at the top of each frame; exposed for the
    // renderer to call right after its frame-fence wait, which is the cheapest
    // point to know the GPU is idle.
    void collect_gpu_times();

    // Direct entry points, identical to onBeginFrame / onEndFrame.
    //
    // WHY they exist: Profiler::beginFrame()/endFrame() never call the on* hooks
    // (see the class comment), so until common/profiler.cpp is fixed these are
    // the only way to reach the timestamp recording. The renderer calls them
    // around its own recording work:
    //
    //   cb->begin();  profiler->set_command_buffer(cb, frame);
    //   profiler->begin_gpu_frame(frame_number);   // TOP_OF_PIPE
    //   ...record...
    //   profiler->end_gpu_frame(frame_number);     // BOTTOM_OF_PIPE
    //   cb->end();
    //
    // Once the base diff is applied these become redundant wrappers over the
    // hooks and can be dropped.
    void begin_gpu_frame(uint64_t frame_number);
    void end_gpu_frame(uint64_t frame_number);

protected:
    void onBeginFrame(uint64_t frame_number) override;
    void onEndFrame(uint64_t frame_number, double frame_time_ms, double cpu_time_ms,
                    double gpu_time_ms, uint32_t draw_calls) override;

private:
    static constexpr uint32_t k_queries_per_frame = 2;   // TOP_OF_PIPE + BOTTOM_OF_PIPE
    static constexpr uint32_t k_ring_frames = 8;         // WHY 8: covers the renderer's 1..3
                                                        // frames in flight with room to spare,
                                                        // without holding a large pool.
    static constexpr uint32_t k_max_queries_per_pool = 2048;  // under the guaranteed 4096

    // Unlocked variants; every caller already holds mutex_. Splitting them out is
    // what keeps onBeginFrame from self-deadlocking on a non-recursive mutex.
    bool probe_support_locked();
    void collect_gpu_times_locked();
    bool create_query_pool_locked();

    // Reads one slot. Returns false when the results are not ready yet.
    bool read_slot_locked(uint32_t slot, GpuFrameTiming& out);

    VulkanRenderer* renderer_ = nullptr;

    // The base calls the on* hooks while holding ITS lock, so this mutex only
    // guards this class's own state. Never held across vkQueueSubmit or
    // vkDeviceWaitIdle.
    mutable std::mutex mutex_;

    VkQueryPool query_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;

    Mode mode_ = Mode::Uninitialized;
    bool support_probed_ = false;
    bool pool_create_failed_ = false;
    bool waiting_for_command_buffer_logged_ = false;

    float timestamp_period_ns_ = 0.0f;
    uint32_t timestamp_valid_bits_ = 0;
    uint32_t pool_query_count_ = 0;

    std::vector<GpuFrameTiming> history_;
    uint32_t history_head_ = 0;
    uint64_t samples_recorded_ = 0;
    uint64_t samples_dropped_ = 0;

    // Slot being written between onBeginFrame and onEndFrame.
    uint32_t write_slot_ = 0;
    bool writing_ = false;

    // Per-slot state: 0 = idle, 1 = written and awaiting readback, 2 = being
    // written this frame. The frame number is stashed per slot so a late read
    // still reports the frame it actually measured.
    std::vector<uint8_t> slot_state_;
    std::vector<uint64_t> slot_frame_number_;
};

} // namespace copper