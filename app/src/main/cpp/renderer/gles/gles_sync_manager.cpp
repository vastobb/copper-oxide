#include "gles_sync_manager.h"

#include <android/log.h>

#define LOG_TAG "CopperOxide-GLESCSyncManager"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace copper {

GLESCSyncManager::GLESCSyncManager() = default;
GLESCSyncManager::~GLESCSyncManager() = default;

uint64_t GLESCSyncManager::submitWaitCallCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return submit_wait_calls_;
}

uint64_t GLESCSyncManager::submitSignalCallCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return submit_signal_calls_;
}

uint64_t GLESCSyncManager::rejectedSubmitCallCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rejected_submit_calls_;
}

// --- fences ---------------------------------------------------------------
//
// GLES has no fence object. createFence() still succeeds: there is nothing to
// allocate, so there is nothing that can fail, and returning 0 would make the
// base treat a perfectly legal GLES synchronization request as an error. The
// returned handle is a bookkeeping key only.
bool GLESCSyncManager::onCreateFence(uint64_t handle, bool signaled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (signaled) {
        signaled_fences_.insert(handle);
    } else {
        signaled_fences_.erase(handle);
    }
    return true;
}

void GLESCSyncManager::onDestroyFence(uint64_t handle) {
    // No GPU object to release; the handle only exists in the base's map, which
    // it erases itself once this returns.
    std::lock_guard<std::mutex> lock(mutex_);
    signaled_fences_.erase(handle);
}

// Returns true only when the host-side flag is already set, false otherwise.
//
// Two things are deliberately NOT done here:
//   * it does not block. SyncManager::waitFence() is the method that actually
//     waits on the base's condition_variable, and it is the only place where
//     blocking is allowed.
//   * it does not call isFenceSignaled(). The base holds Impl::mutex when it
//     invokes hooks, so that would deadlock on a non-recursive mutex.
//
// THE LIMIT, spelled out: a true result means "the host signalled this", never
// "the GPU finished the work submitted before the signal". On GLES the CPU
// already knows the submit call returned, so the host flag is the strongest
// claim available here; real completion needs glFinish() or a PBO/buffer-age
// based fence.
bool GLESCSyncManager::onWaitFence(uint64_t handle, uint64_t /*timeout_ns*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    return signaled_fences_.count(handle) != 0;
}

void GLESCSyncManager::onSignalFence(uint64_t handle) {
    // The base has already flipped its own flag and notified its condition
    // variable before calling this; the mirror is kept so onWaitFence() agrees
    // with the base.
    std::lock_guard<std::mutex> lock(mutex_);
    signaled_fences_.insert(handle);
}

void GLESCSyncManager::onResetFence(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    signaled_fences_.erase(handle);
}

// --- semaphores -----------------------------------------------------------

bool GLESCSyncManager::onCreateSemaphore(uint64_t handle, uint64_t /*initial_value*/) {
    // Pure host-side counter owned by SyncManager::Impl. No GL object exists
    // and none can be created, so this cannot fail.
    (void)handle;
    return true;
}

void GLESCSyncManager::onDestroySemaphore(uint64_t /*handle*/) {
    // Nothing to release on the GPU side.
}

void GLESCSyncManager::onSignalSemaphore(uint64_t /*handle*/, uint64_t /*value*/) {
    // SyncManager::signalSemaphore() has already added `value` to the base's
    // counter by the time it gets here. There is no GPU-side timeline to
    // advance, so there is nothing for this backend to do.
    //
    // Note for whoever reads this next: the base drops any signal with
    // value > 1 unless supportsFeature(TimelineSemaphore) is set, and that
    // happens *before* this hook is reached, so GLES never sees one.
}

// --- events ---------------------------------------------------------------

bool GLESCSyncManager::onCreateEvent(uint64_t /*handle*/) {
    // Host-side bool owned by the base. No GL object, no failure mode.
    return true;
}

void GLESCSyncManager::onDestroyEvent(uint64_t /*handle*/) {
    // Nothing to release.
}

void GLESCSyncManager::onSetEvent(uint64_t /*handle*/) {
    // The base has already set its own flag.
}

void GLESCSyncManager::onResetEvent(uint64_t /*handle*/) {
    // The base has already cleared its own flag.
}

// --- submission synchronization -------------------------------------------

// GL has no equivalent of vkQueueSubmit2's pWaitDstStageMask semaphores: there
// is no submission to hang them off. The calls are validated (a mismatched
// semaphores/values pair is a caller bug and is reported) and then dropped.
//
// The wait semantics are NOT silently lost: the honest GLES way to get a
// cross-API wait is to have the producer signal the semaphore on the host
// before the consumer records work, which the host-side counters already
// honour through SyncManager::waitSemaphore().
void GLESCSyncManager::onSubmitWaitSemaphores(const std::vector<uint64_t>& semaphores,
                                              const std::vector<uint64_t>& values,
                                              uint32_t stage_flags) {
    if (semaphores.size() != values.size()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++rejected_submit_calls_;
        LOGW("submitWaitSemaphores rejected: %zu semaphores vs %zu values "
             "(no GPU-side wait exists on GLES; use a host-side wait)",
             semaphores.size(), values.size());
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++submit_wait_calls_;
    }
    LOGD("submitWaitSemaphores ignored: %zu semaphore(s), stage_flags 0x%x "
         "(GLES has no submission-time wait)",
         semaphores.size(), stage_flags);
}

void GLESCSyncManager::onSubmitSignalSemaphores(const std::vector<uint64_t>& semaphores,
                                                const std::vector<uint64_t>& values) {
    if (semaphores.size() != values.size()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++rejected_submit_calls_;
        LOGW("submitSignalSemaphores rejected: %zu semaphores vs %zu values "
             "(no GPU-side signal exists on GLES)",
             semaphores.size(), values.size());
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++submit_signal_calls_;
    }
    LOGD("submitSignalSemaphores ignored: %zu semaphore(s) (GLES has no "
         "submission-time signal)",
         semaphores.size());
}

} // namespace copper
