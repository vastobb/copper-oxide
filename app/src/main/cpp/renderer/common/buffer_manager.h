#pragma once

#include <cstdint>
#include <string>
#include <memory>

namespace copper {

class RendererBase;

class BufferManager {
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

    BufferManager();
    virtual ~BufferManager();

    // Non-copyable, movable
    BufferManager(const BufferManager&) = delete;
    BufferManager& operator=(const BufferManager&) = delete;
    BufferManager(BufferManager&&) noexcept = default;
    BufferManager& operator=(BufferManager&&) noexcept = default;

    bool initialize(RendererBase* renderer);
    void shutdown();

    // Buffer creation/destruction
    virtual uint64_t createBuffer(uint64_t size, uint32_t usage, uint32_t memory_flags);
    virtual void destroyBuffer(uint64_t handle);

    // Buffer mapping
    virtual void* mapBuffer(uint64_t handle, uint64_t offset = 0, uint64_t size = 0);
    virtual void unmapBuffer(uint64_t handle);
    virtual void flushBuffer(uint64_t handle, uint64_t offset, uint64_t size);
    virtual void invalidateBuffer(uint64_t handle, uint64_t offset, uint64_t size);

    // Buffer updates
    virtual void updateBuffer(uint64_t handle, uint64_t offset, const void* data, uint64_t size);
    virtual void copyBuffer(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset = 0, uint64_t dst_offset = 0);

    // Buffer queries
    virtual const Buffer* getBuffer(uint64_t handle) const;
    virtual uint64_t getBufferSize(uint64_t handle) const;
    virtual void setBufferDebugName(uint64_t handle, const std::string& name);

    // Pool management
    virtual uint64_t allocateFromPool(uint64_t size, uint32_t usage);
    virtual void returnToPool(uint64_t handle);
    virtual void trimPool(int level);
    virtual uint64_t getPoolSize() const;
    virtual uint64_t getTotalAllocated() const;

protected:
    // Backend-specific implementations
    virtual bool onCreateBuffer(uint64_t handle, uint64_t size, uint32_t usage, uint32_t memory_flags) = 0;
    virtual void onDestroyBuffer(uint64_t handle) = 0;
    virtual void* onMapBuffer(uint64_t handle, uint64_t offset, uint64_t size) = 0;
    virtual void onUnmapBuffer(uint64_t handle) = 0;
    virtual void onFlushBuffer(uint64_t handle, uint64_t offset, uint64_t size) = 0;
    virtual void onInvalidateBuffer(uint64_t handle, uint64_t offset, uint64_t size) = 0;
    virtual void onUpdateBuffer(uint64_t handle, uint64_t offset, const void* data, uint64_t size) = 0;
    virtual void onCopyBuffer(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset, uint64_t dst_offset) = 0;
    virtual void onSetBufferDebugName(uint64_t handle, const std::string& name) = 0;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace copper