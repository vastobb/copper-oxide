#pragma once

#include <cstdint>
#include <vector>
#include <memory>

namespace copper {

class RendererBase;

class SyncManager {
public:
    SyncManager();
    virtual ~SyncManager();

    SyncManager(const SyncManager&) = delete;
    SyncManager& operator=(const SyncManager&) = delete;
    SyncManager(SyncManager&&) noexcept = default;
    SyncManager& operator=(SyncManager&&) noexcept = default;

    bool initialize(RendererBase* renderer);
    void shutdown();

    // Fences
    virtual uint64_t createFence(bool signaled = false);
    virtual void destroyFence(uint64_t handle);
    virtual bool waitFence(uint64_t handle, uint64_t timeout_ns);
    virtual void signalFence(uint64_t handle);
    virtual void resetFence(uint64_t handle);
    virtual bool isFenceSignaled(uint64_t handle) const;

    // Semaphores (including timeline semaphores)
    virtual uint64_t createSemaphore(uint64_t initial_value = 0);
    virtual void destroySemaphore(uint64_t handle);
    virtual void signalSemaphore(uint64_t handle, uint64_t value = 0);
    virtual uint64_t getSemaphoreValue(uint64_t handle) const;
    virtual bool waitSemaphore(uint64_t handle, uint64_t value, uint64_t timeout_ns);

    // Events
    virtual uint64_t createEvent();
    virtual void destroyEvent(uint64_t handle);
    virtual void setEvent(uint64_t handle);
    virtual void resetEvent(uint64_t handle);
    virtual bool isEventSet(uint64_t handle) const;

    // Submission synchronization
    virtual void submitWaitSemaphores(const std::vector<uint64_t>& semaphores, const std::vector<uint64_t>& values, uint32_t stage_flags);
    virtual void submitSignalSemaphores(const std::vector<uint64_t>& semaphores, const std::vector<uint64_t>& values);

protected:
    virtual bool onCreateFence(uint64_t handle, bool signaled) = 0;
    virtual void onDestroyFence(uint64_t handle) = 0;
    virtual bool onWaitFence(uint64_t handle, uint64_t timeout_ns) = 0;
    virtual void onSignalFence(uint64_t handle) = 0;
    virtual void onResetFence(uint64_t handle) = 0;

    virtual bool onCreateSemaphore(uint64_t handle, uint64_t initial_value) = 0;
    virtual void onDestroySemaphore(uint64_t handle) = 0;
    virtual void onSignalSemaphore(uint64_t handle, uint64_t value) = 0;

    virtual bool onCreateEvent(uint64_t handle) = 0;
    virtual void onDestroyEvent(uint64_t handle) = 0;
    virtual void onSetEvent(uint64_t handle) = 0;
    virtual void onResetEvent(uint64_t handle) = 0;

    virtual void onSubmitWaitSemaphores(const std::vector<uint64_t>& semaphores, const std::vector<uint64_t>& values, uint32_t stage_flags) = 0;
    virtual void onSubmitSignalSemaphores(const std::vector<uint64_t>& semaphores, const std::vector<uint64_t>& values) = 0;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace copper