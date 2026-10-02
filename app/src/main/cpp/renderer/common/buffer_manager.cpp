#include "buffer_manager.h"
#include "renderer_base.h"

#include <unordered_map>
#include <mutex>

namespace copper {

class BufferManager::Impl {
public:
    struct Buffer {
        uint64_t handle = 0;
        uint64_t size = 0;
        uint32_t usage = 0;
        uint32_t memory_flags = 0;
        void* mapped_ptr = nullptr;
        bool is_mapped = false;
        std::string debug_name;
    };

    std::unordered_map<uint64_t, Buffer> buffers;
    std::vector<uint64_t> pool_handles;
    uint64_t next_handle = 1;
    std::mutex mutex;
    RendererBase* renderer = nullptr;
};

BufferManager::BufferManager() : pImpl(std::make_unique<Impl>()) {}
BufferManager::~BufferManager() = default;

bool BufferManager::initialize(RendererBase* renderer) {
    pImpl->renderer = renderer;
    return true;
}

void BufferManager::shutdown() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    for (auto& [handle, buffer] : pImpl->buffers) {
        if (buffer.is_mapped) {
            onUnmapBuffer(handle);
        }
        onDestroyBuffer(handle);
    }
    pImpl->buffers.clear();
    pImpl->pool_handles.clear();
}

uint64_t BufferManager::createBuffer(uint64_t size, uint32_t usage, uint32_t memory_flags) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    uint64_t handle = pImpl->next_handle++;

    Impl::Buffer buffer;
    buffer.handle = handle;
    buffer.size = size;
    buffer.usage = usage;
    buffer.memory_flags = memory_flags;

    if (!onCreateBuffer(handle, size, usage, memory_flags)) {
        return 0;
    }

    pImpl->buffers[handle] = std::move(buffer);
    return handle;
}

void BufferManager::destroyBuffer(uint64_t handle) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->buffers.find(handle);
    if (it == pImpl->buffers.end()) {
        return;
    }

    if (it->second.is_mapped) {
        onUnmapBuffer(handle);
    }

    onDestroyBuffer(handle);
    pImpl->buffers.erase(it);

    auto pool_it = std::find(pImpl->pool_handles.begin(), pImpl->pool_handles.end(), handle);
    if (pool_it != pImpl->pool_handles.end()) {
        pImpl->pool_handles.erase(pool_it);
    }
}

void* BufferManager::mapBuffer(uint64_t handle, uint64_t offset, uint64_t size) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->buffers.find(handle);
    if (it == pImpl->buffers.end() || it->second.is_mapped) {
        return nullptr;
    }

    void* ptr = onMapBuffer(handle, offset, size);
    if (ptr) {
        it->second.mapped_ptr = ptr;
        it->second.is_mapped = true;
    }
    return ptr;
}

void BufferManager::unmapBuffer(uint64_t handle) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->buffers.find(handle);
    if (it == pImpl->buffers.end() || !it->second.is_mapped) {
        return;
    }

    onUnmapBuffer(handle);
    it->second.mapped_ptr = nullptr;
    it->second.is_mapped = false;
}

void BufferManager::flushBuffer(uint64_t handle, uint64_t offset, uint64_t size) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->buffers.find(handle);
    if (it == pImpl->buffers.end() || !it->second.is_mapped) {
        return;
    }
    onFlushBuffer(handle, offset, size);
}

void BufferManager::invalidateBuffer(uint64_t handle, uint64_t offset, uint64_t size) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->buffers.find(handle);
    if (it == pImpl->buffers.end() || !it->second.is_mapped) {
        return;
    }
    onInvalidateBuffer(handle, offset, size);
}

void BufferManager::updateBuffer(uint64_t handle, uint64_t offset, const void* data, uint64_t size) {
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        auto it = pImpl->buffers.find(handle);
        if (it == pImpl->buffers.end()) {
            return;
        }
        // Out-of-range writes become out-of-bounds memcpys in the backend.
        if (offset > it->second.size || size > it->second.size - offset) {
            return;
        }
    }
    onUpdateBuffer(handle, offset, data, size);
}

void BufferManager::copyBuffer(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset, uint64_t dst_offset) {
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        const auto src_it = pImpl->buffers.find(src);
        const auto dst_it = pImpl->buffers.find(dst);
        if (src_it == pImpl->buffers.end() || dst_it == pImpl->buffers.end()) {
            return;
        }
        if (src_offset > src_it->second.size || size > src_it->second.size - src_offset ||
            dst_offset > dst_it->second.size || size > dst_it->second.size - dst_offset) {
            return;
        }
    }
    onCopyBuffer(src, dst, size, src_offset, dst_offset);
}

const BufferManager::Buffer* BufferManager::getBuffer(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->buffers.find(handle);
    if (it == pImpl->buffers.end()) {
        return nullptr;
    }
    // Note: This is unsafe as Impl::Buffer is different from BufferManager::Buffer
    // For now, we'll cast. In a real implementation, these should be unified.
    return reinterpret_cast<const BufferManager::Buffer*>(&it->second);
}

uint64_t BufferManager::getBufferSize(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->buffers.find(handle);
    if (it == pImpl->buffers.end()) {
        return 0;
    }
    return it->second.size;
}

void BufferManager::setBufferDebugName(uint64_t handle, const std::string& name) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->buffers.find(handle);
    if (it != pImpl->buffers.end()) {
        it->second.debug_name = name;
        onSetBufferDebugName(handle, name);
    }
}

uint64_t BufferManager::allocateFromPool(uint64_t size, uint32_t usage) {
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        for (auto it = pImpl->pool_handles.begin(); it != pImpl->pool_handles.end(); ++it) {
            auto buffer_it = pImpl->buffers.find(*it);
            if (buffer_it != pImpl->buffers.end() && buffer_it->second.size >= size) {
                uint64_t handle = *it;
                pImpl->pool_handles.erase(it);
                buffer_it->second.usage = usage;
                return handle;
            }
        }
    }
    // createBuffer() takes the mutex itself: calling it under the lock would
    // self-deadlock on a non-recursive mutex.
    return createBuffer(size, usage, 0);
}

void BufferManager::returnToPool(uint64_t handle) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (pImpl->buffers.find(handle) != pImpl->buffers.end()) {
        pImpl->pool_handles.push_back(handle);
    }
}

void BufferManager::trimPool(int level) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    size_t keep = pImpl->pool_handles.size() * (100 - level) / 100;
    while (pImpl->pool_handles.size() > keep) {
        uint64_t handle = pImpl->pool_handles.back();
        pImpl->pool_handles.pop_back();
        auto it = pImpl->buffers.find(handle);
        if (it != pImpl->buffers.end()) {
            onDestroyBuffer(handle);
            pImpl->buffers.erase(it);
        }
    }
}

uint64_t BufferManager::getPoolSize() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->pool_handles.size();
}

uint64_t BufferManager::getTotalAllocated() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    uint64_t total = 0;
    for (const auto& [handle, buffer] : pImpl->buffers) {
        total += buffer.size;
    }
    return total;
}

} // namespace copper