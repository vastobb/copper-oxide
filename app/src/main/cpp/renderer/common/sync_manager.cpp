#include "sync_manager.h"
#include "renderer_base.h"

#include <vector>
#include <mutex>
#include <condition_variable>

namespace copper {

class SyncManager::Impl {
public:
    struct Fence {
        uint64_t handle = 0;
        bool signaled = false;
        std::unique_ptr<std::mutex> mutex = std::make_unique<std::mutex>();
        std::unique_ptr<std::condition_variable> cv = std::make_unique<std::condition_variable>();
    };

    struct Semaphore {
        uint64_t handle = 0;
        uint64_t value = 0; // For timeline semaphores
    };

    struct Event {
        uint64_t handle = 0;
        bool set = false;
    };

    std::unordered_map<uint64_t, Fence> fences;
    std::unordered_map<uint64_t, Semaphore> semaphores;
    std::unordered_map<uint64_t, Event> events;
    uint64_t next_fence_handle = 1;
    uint64_t next_semaphore_handle = 1;
    uint64_t next_event_handle = 1;
    std::mutex mutex;
    RendererBase* renderer = nullptr;
    bool supports_timeline_semaphores = false;
};

SyncManager::SyncManager() : pImpl(std::make_unique<Impl>()) {}
SyncManager::~SyncManager() = default;

bool SyncManager::initialize(RendererBase* renderer) {
    pImpl->renderer = renderer;
    if (renderer) {
        pImpl->supports_timeline_semaphores = renderer->supportsFeature(RendererFeature::TimelineSemaphore);
    }
    return true;
}

void SyncManager::shutdown() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    for (auto& [handle, fence] : pImpl->fences) {
        onDestroyFence(handle);
    }
    for (auto& [handle, semaphore] : pImpl->semaphores) {
        onDestroySemaphore(handle);
    }
    for (auto& [handle, event] : pImpl->events) {
        onDestroyEvent(handle);
    }
    pImpl->fences.clear();
    pImpl->semaphores.clear();
    pImpl->events.clear();
}

uint64_t SyncManager::createFence(bool signaled) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    uint64_t handle = pImpl->next_fence_handle++;

    Impl::Fence fence;
    fence.handle = handle;
    fence.signaled = signaled;

    if (!onCreateFence(handle, signaled)) {
        return 0;
    }

    pImpl->fences[handle] = std::move(fence);
    return handle;
}

void SyncManager::destroyFence(uint64_t handle) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->fences.find(handle);
    if (it == pImpl->fences.end()) {
        return;
    }
    onDestroyFence(handle);
    pImpl->fences.erase(it);
}

bool SyncManager::waitFence(uint64_t handle, uint64_t timeout_ns) {
    auto it = pImpl->fences.find(handle);
    if (it == pImpl->fences.end()) {
        return false;
    }

    if (it->second.signaled) {
        return true;
    }

    if (timeout_ns == 0) {
        return false;
    }

    std::unique_lock<std::mutex> lock(*it->second.mutex);
    return it->second.cv->wait_for(lock, std::chrono::nanoseconds(timeout_ns), [&]() {
        return it->second.signaled;
    });
}

void SyncManager::signalFence(uint64_t handle) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->fences.find(handle);
    if (it == pImpl->fences.end()) {
        return;
    }
    std::lock_guard<std::mutex> fence_lock(*it->second.mutex);
    it->second.signaled = true;
    it->second.cv->notify_all();
    onSignalFence(handle);
}

void SyncManager::resetFence(uint64_t handle) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->fences.find(handle);
    if (it == pImpl->fences.end()) {
        return;
    }
    it->second.signaled = false;
    onResetFence(handle);
}

bool SyncManager::isFenceSignaled(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->fences.find(handle);
    if (it == pImpl->fences.end()) {
        return false;
    }
    return it->second.signaled;
}

uint64_t SyncManager::createSemaphore(uint64_t initial_value) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    uint64_t handle = pImpl->next_semaphore_handle++;

    Impl::Semaphore semaphore;
    semaphore.handle = handle;
    semaphore.value = initial_value;

    if (!onCreateSemaphore(handle, initial_value)) {
        return 0;
    }

    pImpl->semaphores[handle] = std::move(semaphore);
    return handle;
}

void SyncManager::destroySemaphore(uint64_t handle) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->semaphores.find(handle);
    if (it == pImpl->semaphores.end()) {
        return;
    }
    onDestroySemaphore(handle);
    pImpl->semaphores.erase(it);
}

void SyncManager::signalSemaphore(uint64_t handle, uint64_t value) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->semaphores.find(handle);
    if (it == pImpl->semaphores.end()) {
        return;
    }
    it->second.value = value;
    onSignalSemaphore(handle, value);
}

uint64_t SyncManager::getSemaphoreValue(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->semaphores.find(handle);
    if (it == pImpl->semaphores.end()) {
        return 0;
    }
    return it->second.value;
}

bool SyncManager::waitSemaphore(uint64_t handle, uint64_t value, uint64_t timeout_ns) {
    // Timeline semaphore wait
    // In a real implementation, this would use vkWaitSemaphores or similar
    return true;
}

uint64_t SyncManager::createEvent() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    uint64_t handle = pImpl->next_event_handle++;

    Impl::Event event;
    event.handle = handle;

    if (!onCreateEvent(handle)) {
        return 0;
    }

    pImpl->events[handle] = std::move(event);
    return handle;
}

void SyncManager::destroyEvent(uint64_t handle) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->events.find(handle);
    if (it == pImpl->events.end()) {
        return;
    }
    onDestroyEvent(handle);
    pImpl->events.erase(it);
}

void SyncManager::setEvent(uint64_t handle) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->events.find(handle);
    if (it == pImpl->events.end()) {
        return;
    }
    it->second.set = true;
    onSetEvent(handle);
}

void SyncManager::resetEvent(uint64_t handle) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->events.find(handle);
    if (it == pImpl->events.end()) {
        return;
    }
    it->second.set = false;
    onResetEvent(handle);
}

bool SyncManager::isEventSet(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->events.find(handle);
    if (it == pImpl->events.end()) {
        return false;
    }
    return it->second.set;
}

void SyncManager::submitWaitSemaphores(const std::vector<uint64_t>& semaphores, const std::vector<uint64_t>& values, uint32_t stage_flags) {
    onSubmitWaitSemaphores(semaphores, values, stage_flags);
}

void SyncManager::submitSignalSemaphores(const std::vector<uint64_t>& semaphores, const std::vector<uint64_t>& values) {
    onSubmitSignalSemaphores(semaphores, values);
}

} // namespace copper