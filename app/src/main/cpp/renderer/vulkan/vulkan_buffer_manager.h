#pragma once

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>

#include "buffer_manager.h"

namespace copper {

class VulkanRenderer;

// Backend-agnostic buffer usage bits. The common layer passes these through
// verbatim (BufferManager::createBuffer stores them as given), so the Vulkan
// backend owns the translation into VkBufferUsageFlags.
namespace BufferUsage {
inline constexpr uint32_t NONE         = 0u;
inline constexpr uint32_t VERTEX       = 1u << 0;
inline constexpr uint32_t INDEX        = 1u << 1;
inline constexpr uint32_t UNIFORM      = 1u << 2;
inline constexpr uint32_t STORAGE      = 1u << 3;
inline constexpr uint32_t TRANSFER_SRC = 1u << 4;
inline constexpr uint32_t TRANSFER_DST = 1u << 5;
inline constexpr uint32_t INDIRECT     = 1u << 6;
}

// Memory property bits accepted by createBuffer()/allocateFromPool(). Passing
// BufferMemory::NONE (0) selects the backend default, see defaultMemoryFlags().
namespace BufferMemory {
inline constexpr uint32_t NONE                 = 0u;
inline constexpr uint32_t HOST_VISIBLE         = 1u << 0;
inline constexpr uint32_t HOST_COHERENT        = 1u << 1;
inline constexpr uint32_t DEVICE_LOCAL         = 1u << 2;
inline constexpr uint32_t STAGING              = 1u << 3;
inline constexpr uint32_t PREFER_DEVICE_LOCAL  = 1u << 4;
}

// Vulkan implementation of the backend-agnostic buffer manager.
//
// Lifetime: VulkanRenderer owns one instance and keeps it alive for as long as
// the device is alive. All creation/destruction happens inside the on* hooks,
// which BufferManager calls while holding *its* lock; those hooks take this
// manager's own mutex (see Impl::mutex) and never call back into a public
// BufferManager method, which would invert the lock order and self-deadlock on
// the non-recursive base mutex.
//
// Memory: VkDeviceMemory is allocated by Impl::allocate_and_bind(), which
// prefers VMA when VulkanRenderer::create_allocator() has produced an allocator
// and otherwise picks a memory type by hand. That single helper is the only
// place that has to change when VMA lands.
class VulkanBufferManager : public BufferManager {
public:
    explicit VulkanBufferManager(VulkanRenderer* renderer);
    ~VulkanBufferManager() override;

    // Usage / memory-flag translation, exposed so the rest of the Vulkan backend
    // (state manager, upload paths) can ask for the same flags.
    static VkBufferUsageFlags translateUsage(uint32_t usage);
    static VkMemoryPropertyFlags translateMemoryFlags(uint32_t memory_flags);

    // Resolves the VkMemoryPropertyFlags actually requested for a buffer.
    // memory_flags == BufferMemory::NONE means "backend default": uniform
    // buffers get HOST_VISIBLE|HOST_COHERENT (the CPU rewrites them every
    // frame), everything else gets DEVICE_LOCAL and is written through a
    // staging copy.
    static VkMemoryPropertyFlags defaultMemoryFlags(uint32_t usage, uint32_t memory_flags);

    // Backend-specific queries used by the rest of the Vulkan backend.
    VkBuffer vkBuffer(uint64_t handle) const;
    bool isMapped(uint64_t handle) const;
    // True while the buffer's current contents may still be read by a frame
    // that has not finished on the GPU.
    bool isInUseByGpu(uint64_t handle) const;
    // Blocks until every staging upload submitted by this manager has retired.
    bool waitForUploads(uint64_t timeout_ns = UINT64_MAX);
    uint64_t getUploadedBytes() const;
    // Frees the staging buffer / command buffer / fence. VulkanRenderer::shutdown()
    // must call this before vkDestroyDevice(); ~VulkanBufferManager() calls it
    // as well, but by then the device may already be gone.
    void releaseDeviceResources();

    // Overridden because BufferManager::getBuffer() reinterpret_casts a
    // different struct (see buffer_manager.cpp): these answer from the Vulkan
    // record, which is the one that actually owns the VkBuffer.
    const Buffer* getBuffer(uint64_t handle) const override;
    uint64_t getBufferSize(uint64_t handle) const override;

protected:
    bool onCreateBuffer(uint64_t handle, uint64_t size, uint32_t usage, uint32_t memory_flags) override;
    void onDestroyBuffer(uint64_t handle) override;
    void* onMapBuffer(uint64_t handle, uint64_t offset, uint64_t size) override;
    void onUnmapBuffer(uint64_t handle) override;
    void onFlushBuffer(uint64_t handle, uint64_t offset, uint64_t size) override;
    void onInvalidateBuffer(uint64_t handle, uint64_t offset, uint64_t size) override;
    void onUpdateBuffer(uint64_t handle, uint64_t offset, const void* data, uint64_t size) override;
    void onCopyBuffer(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset, uint64_t dst_offset) override;
    void onSetBufferDebugName(uint64_t handle, const std::string& name) override;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
    VulkanRenderer* renderer_ = nullptr;
};

} // namespace copper
