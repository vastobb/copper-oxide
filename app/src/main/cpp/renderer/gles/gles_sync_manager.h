#pragma once

#include "sync_manager.h"

#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace copper {

// GLES backend for SyncManager.
//
// WHAT GLES ACTUALLY HAS: no fences, no semaphores, no events and no
// submission-time synchronization objects. There is no vkQueueSubmit to attach
// a wait/signal to, and the only cross-API synchronization primitives in core
// GLES are glFinish / glFlush, which are host-side commands.
//
// So every object below is HOST-SIDE BOOKKEEPING. SyncManager::Impl already
// keeps a fence flag plus a condition_variable, a semaphore counter and an
// event flag; the base's waitFence() therefore already does the real (host)
// waiting work. This class exists so that:
//
//   * creation always succeeds (there is nothing to create that can fail), and
//   * onWaitFence() can answer the "is this fence signaled?" question without
//     re-entering the base (see the deadlock note below).
//
// HONEST LIMITATION, stated once so nobody has to re-derive it: a GLES fence
// here is NOT GPU completion. If the CPU signals a fence after enqueuing work,
// waitFence() returning true means "the host reached that point", not "the GPU
// finished". A caller that needs real GPU completion on GLES must call
// glFinish()/waitIdle() (RendererBase::waitIdle) or use a PBO fence / a
// GL_EXT_disjoint_timer_query result, none of which this class pretends to do.
//
// DEADLOCK NOTE: the base calls the on* hooks while holding its own mutex
// (SyncManager::Impl::mutex), so no hook here may call back into a public
// SyncManager method -- isFenceSignaled(), getSemaphoreValue() and friends all
// re-take that same non-recursive mutex. That is why the signaled state is
// mirrored in signaled_fences_ below instead of being queried from the base.
class GLESCSyncManager : public SyncManager {
public:
    GLESCSyncManager();
    ~GLESCSyncManager() override;

    // Capability probes, so a caller can branch instead of guessing. All three
    // are false by construction; they exist to make "GLES cannot do this"
    // greppable at the call site.
    static constexpr bool k_has_gpu_fence = false;
    static constexpr bool k_has_gpu_semaphore = false;
    static constexpr bool k_has_submit_sync = false;

    bool hasGpuFence() const noexcept { return k_has_gpu_fence; }
    bool hasGpuSemaphore() const noexcept { return k_has_gpu_semaphore; }

    // Number of submitWaitSemaphores()/submitSignalSemaphores() calls that
    // reached the backend at all. Both are validated no-ops on GLES; the
    // counters make "the caller asked for queue sync it cannot get" visible in
    // a logcat dump instead of invisible.
    uint64_t submitWaitCallCount() const;
    uint64_t submitSignalCallCount() const;

    // Number of submit-wait/signal calls that were dropped because the
    // semaphores/values vectors disagreed in size.
    uint64_t rejectedSubmitCallCount() const;

protected:
    // --- fences: host-side flags only, no VkFence equivalent exists ---------
    bool onCreateFence(uint64_t handle, bool signaled) override;
    void onDestroyFence(uint64_t handle) override;
    bool onWaitFence(uint64_t handle, uint64_t timeout_ns) override;
    void onSignalFence(uint64_t handle) override;
    void onResetFence(uint64_t handle) override;

    // --- semaphores: a plain uint64 counter, no GPU object ------------------
    bool onCreateSemaphore(uint64_t handle, uint64_t initial_value) override;
    void onDestroySemaphore(uint64_t handle) override;
    void onSignalSemaphore(uint64_t handle, uint64_t value) override;

    // --- events: a plain bool, no GPU object -------------------------------
    bool onCreateEvent(uint64_t handle) override;
    void onDestroyEvent(uint64_t handle) override;
    void onSetEvent(uint64_t handle) override;
    void onResetEvent(uint64_t handle) override;

    // --- submission sync: validated no-ops ---------------------------------
    void onSubmitWaitSemaphores(const std::vector<uint64_t>& semaphores,
                                const std::vector<uint64_t>& values,
                                uint32_t stage_flags) override;
    void onSubmitSignalSemaphores(const std::vector<uint64_t>& semaphores,
                                  const std::vector<uint64_t>& values) override;

private:
    // Guards signaled_fences_ and the counters only. Never held while calling
    // into the base or into GL.
    mutable std::mutex mutex_;
    // Mirrors SyncManager::Impl's per-fence `signaled` flag. Seeded by
    // onCreateFence and maintained by onSignalFence/onResetFence, so onWaitFence
    // can answer without touching the base's map.
    std::unordered_set<uint64_t> signaled_fences_;
    uint64_t submit_wait_calls_ = 0;
    uint64_t submit_signal_calls_ = 0;
    uint64_t rejected_submit_calls_ = 0;
};

} // namespace copper
