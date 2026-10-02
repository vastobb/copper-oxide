#pragma once

// WHY: the platform guard has to be defined before vulkan.h is pulled in,
// otherwise the Android surface/window types stay invisible.
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "sync_manager.h"

namespace copper {

class VulkanRenderer;

// Vulkan backend for SyncManager.
//
// The base class owns handle allocation, the host-visible "signaled" bookkeeping
// and the pool bookkeeping; everything below is real Vulkan work driven from the
// base's on* hooks.
//
// ---------------------------------------------------------------------------
// SEMANTIC MISMATCH: the base's fence/semaphore API is a GL-shaped API
// ---------------------------------------------------------------------------
// The base models GL fences: the host may "signal" a fence itself
// (SyncManager::signalFence) and callers read SyncManager::isFenceSignaled().
// Vulkan has no such thing: a VkFence is signalled *only* by a queue submission,
// and the host can merely reset it or wait on it. This backend therefore:
//
//   * onSignalFence() is a documented no-op. The base has already flipped its
//     host-side flag before the hook runs, so isFenceSignaled() keeps answering
//     what callers expect, but no VkFence is touched.
//   * onResetFence() is the real vkResetFences.
//   * onWaitFence() is the real vkWaitForFences.
//
// DEADLOCK CONTRACT: SyncManager::signalFence() and SyncManager::resetFence()
// call the on* hooks WHILE HOLDING THE BASE'S OWN MUTEX. These hooks must
// therefore never call back into a base public method (SyncManager::createFence,
// destroyFence, getSemaphoreValue, ...): the base mutex is not recursive and the
// second acquisition self-deadlocks. Only this class's own mutex_ may be taken.
//
// SECOND GAP IN THE BASE, verified by reading common/sync_manager.cpp:
// SyncManager::waitFence() never calls onWaitFence() at all. It looks the fence
// up, checks the host flag, and otherwise waits on a condition_variable -- there
// is no call to the backend hook anywhere in that function. So onWaitFence() is
// unreachable through the public API today, exactly like
// Profiler::beginFrame()/endFrame() never calling their hooks. It is implemented
// anyway (a fixed base needs it), and wait_fence_for() below reaches the same
// code directly so a caller can block on a real VkFence today. The diff is in the
// integration report.
//
// THIRD GAP IN THE BASE, same file: isEventSet() reads only the host flag, and
// setEvent() flips it without asking the GPU, so a VkEvent that was never set by
// a submitted vkCmdSetEvent still reports "set". isEventSet() has no on* hook to
// fix, so this backend cannot reconcile it; onSetEvent()/onResetEvent() issue the
// real calls and the base's flag remains the source of truth for callers.
//
// INTEGRATOR NOTE: vulkan_renderer.h still contains an inline stub of this class
// name (together with VulkanCommandBuffer / VulkanResourcePool / VulkanProfiler).
// Those stubs have to be deleted from vulkan_renderer.h and replaced with an
// include of this header, otherwise the two definitions of
// `copper::VulkanSyncManager` collide.
class VulkanSyncManager : public SyncManager {
public:
    explicit VulkanSyncManager(VulkanRenderer* renderer);
    ~VulkanSyncManager() override;

    VulkanSyncManager(const VulkanSyncManager&) = delete;
    VulkanSyncManager& operator=(const VulkanSyncManager&) = delete;

    // Capability probe + lazy handle-table init.
    //
    // WHY explicit AND lazy: SyncManager::initialize() is not virtual, so the
    // backend has no init hook; the integrator can call prepare() from
    // VulkanRenderer::initializeManagers() and every on* hook calls it anyway,
    // so an integrator that forgets still gets a working manager.
    bool prepare();

    // True only when the device really has timeline semaphores enabled, i.e.
    // VK_KHR_timeline_semaphore (or Vulkan 1.2) is present AND
    // VkPhysicalDeviceFeatures::timelineSemaphore was set at vkCreateDevice
    // time. Everything created while this is false is a binary semaphore.
    bool timeline_enabled() const;
    bool timeline_available() const;

    // Handle translation, needed by the submit path (the base hands out opaque
    // uint64_t handles, Vulkan needs VkSemaphore/VkFence).
    VkFence fence(uint64_t handle) const;
    VkSemaphore semaphore(uint64_t handle) const;
    bool is_timeline_semaphore(uint64_t handle) const;
    uint64_t timeline_value(uint64_t handle) const;

    // Direct backend waits. SyncManager::waitFence() never reaches the on* hook
    // (host-side only), so this is the only way today to actually block on a
    // VkFence. timeout_ns == 0 polls, UINT64_MAX waits forever.
    bool wait_fence_for(uint64_t handle, uint64_t timeout_ns);

    // --- submit integration -------------------------------------------------
    // Builds the VkSubmitInfo for the NEXT vkQueueSubmit out of whatever
    // submitWaitSemaphores()/submitSignalSemaphores() recorded.
    //
    // CONTRACT:
    //   * The returned struct borrows this object's internal storage. It stays
    //     valid until the next build_submit_info() / clear_pending_submit() /
    //     onSubmitWaitSemaphores() / onSubmitSignalSemaphores() call. The caller
    //     must therefore call vkQueueSubmit before touching the sync manager
    //     again -- which is exactly what VulkanCommandBuffer::onSubmit does.
    //   * pCommandBuffers points at this object's member, not at the argument,
    //     because the argument is a by-value parameter that dies at return.
    //   * Timeline semaphores are hoisted to the front of both lists: the
    //     Vulkan spec requires the first semaphoreValueCount entries of
    //     pWaitSemaphores / pSignalSemaphores to be the timeline ones when
    //     VkTimelineSemaphoreSubmitInfo is chained in.
    VkSubmitInfo build_submit_info(VkCommandBuffer command_buffer);

    bool has_pending_submit_sync() const;
    void clear_pending_submit();
    uint32_t pending_wait_count() const;
    uint32_t pending_signal_count() const;

protected:
    bool onCreateFence(uint64_t handle, bool signaled) override;
    void onDestroyFence(uint64_t handle) override;
    bool onWaitFence(uint64_t handle, uint64_t timeout_ns) override;
    void onSignalFence(uint64_t handle) override;
    void onResetFence(uint64_t handle) override;

    bool onCreateSemaphore(uint64_t handle, uint64_t initial_value) override;
    void onDestroySemaphore(uint64_t handle) override;
    void onSignalSemaphore(uint64_t handle, uint64_t value) override;

    bool onCreateEvent(uint64_t handle) override;
    void onDestroyEvent(uint64_t handle) override;
    void onSetEvent(uint64_t handle) override;
    void onResetEvent(uint64_t handle) override;

    void onSubmitWaitSemaphores(const std::vector<uint64_t>& semaphores,
                                const std::vector<uint64_t>& values,
                                uint32_t stage_flags) override;
    void onSubmitSignalSemaphores(const std::vector<uint64_t>& semaphores,
                                  const std::vector<uint64_t>& values) override;

private:
    // One entry of the pending submit. `timeline` decides whether the value is
    // meaningful (timeline) or ignored (binary).
    struct PendingSemaphore {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        VkPipelineStageFlags stage_mask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        uint64_t value = 0;
        bool timeline = false;
    };

    // Storage the returned VkSubmitInfo points into. Lives in the object, not on
    // the stack, so the pointers stay valid after build_submit_info returns.
    struct SubmitStorage {
        VkSubmitInfo info{};
        VkTimelineSemaphoreSubmitInfo timeline{};
        std::vector<VkSemaphore> wait_semaphores;
        std::vector<VkPipelineStageFlags> wait_stages;
        std::vector<uint64_t> wait_values;
        std::vector<VkSemaphore> signal_semaphores;
        std::vector<uint64_t> signal_values;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        uint32_t wait_value_count = 0;
        uint32_t signal_value_count = 0;
    };

    // Vulkan's practical limit for one submit is implementation defined; 16 is
    // well inside every mobile driver while still being an honest bound.
    static constexpr uint32_t k_max_pending_semaphores = 16;

    bool prepare_locked();
    VkDevice device() const;
    void load_timeline_entry_points_locked();

    VulkanRenderer* renderer_ = nullptr;

    // The base calls the on* hooks while holding ITS lock, so this mutex only
    // guards this class's own tables. It is never held across vkQueueSubmit or
    // vkDeviceWaitIdle (this class never submits).
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, VkFence> fences_;
    std::unordered_map<uint64_t, VkSemaphore> semaphores_;
    std::unordered_map<uint64_t, uint64_t> timeline_values_;
    std::unordered_map<uint64_t, VkEvent> events_;
    std::vector<PendingSemaphore> pending_waits_;
    std::vector<PendingSemaphore> pending_signals_;
    SubmitStorage submit_;

    bool prepared_ = false;
    bool timeline_enabled_ = false;
    bool timeline_available_ = false;
    bool binary_fallback_logged_ = false;
    bool signal_fence_logged_ = false;
    bool binary_signal_logged_ = false;
    bool infinite_wait_logged_ = false;
    bool submit_order_logged_ = false;
    mutable PFN_vkSignalSemaphoreKHR signal_semaphore_fn_ = nullptr;
};

} // namespace copper