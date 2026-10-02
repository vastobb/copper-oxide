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

#include "resource_pool.h"

namespace copper {

class VulkanRenderer;

// Vulkan backend for ResourcePool.
//
// ---------------------------------------------------------------------------
// DESIGN: the pool owns POLICY, the managers own the GPU objects
// ---------------------------------------------------------------------------
// ResourcePool is a handle allocator plus a free list: it decides WHEN a
// resource is reused and TRACKS bytes (see ResourcePool::Stats). It has no
// idea what a VkBuffer is.
//
// Creating a second, competing allocation path inside this class was the
// obvious-looking mistake to avoid. VulkanBufferManager already owns every
// VkBuffer/VmaAllocation pair and already has its own free list
// (BufferManager::allocateFromPool / returnToPool). Two allocators over one
// device means two handle spaces, two free lists, and a class of bug where a
// VkBuffer is freed by the wrong owner. So:
//
//   * onAllocateBuffer() -> getBufferManager()->createBuffer()
//   * onAllocateTexture() -> getTextureManager()->createTexture2D()
//   * onFree() -> the matching destroy on the same manager
//
// Both managers are reached through RendererBase's public virtual accessors, so
// this backend works with whatever backend manager the integrator installs.
//
// HANDLE SPACES DIFFER -- READ THIS:
// BufferManager::createBuffer() and TextureManager::createTexture2D() return
// handles from their OWN counters (1, 2, 3, ...), which have nothing to do with
// the pool handle the base allocated for this hook. The pool handle is what
// callers of ResourcePool::allocateBuffer() see, so the translation has to live
// somewhere; it lives in the maps below, and buffer_handle() / texture_handle()
// hand it back to the integrator.
//
// INTEGRATOR NOTE: vulkan_renderer.h still contains an inline stub of this
// class name (together with VulkanCommandBuffer / VulkanSyncManager /
// VulkanProfiler). Those stubs have to be deleted from vulkan_renderer.h and
// replaced with an include of this header, otherwise the two definitions of
// `copper::VulkanResourcePool` collide.
class VulkanResourcePool : public ResourcePool {
public:
    explicit VulkanResourcePool(VulkanRenderer* renderer);
    ~VulkanResourcePool() override;

    VulkanResourcePool(const VulkanResourcePool&) = delete;
    VulkanResourcePool& operator=(const VulkanResourcePool&) = delete;

    // Pool handle -> manager handle. Returns 0 when the pool handle is unknown
    // or was never backed by a GPU object. The renderer needs these to build
    // VkBuffer / VkImage handles for a draw call.
    uint64_t buffer_handle(uint64_t pool_handle) const;
    uint64_t texture_handle(uint64_t pool_handle) const;

    // Live GPU objects this pool currently owns (0 == a pooled-but-unbacked
    // entry, which should not happen after a successful allocation).
    uint64_t live_buffer_count() const;
    uint64_t live_texture_count() const;

protected:
    bool onAllocateBuffer(uint64_t handle, uint64_t size, uint32_t usage,
                          uint32_t memory_flags) override;
    bool onAllocateTexture(uint64_t handle, uint32_t width, uint32_t height, uint32_t format,
                           uint32_t usage, uint32_t mip_levels) override;
    void onFree(uint64_t handle) override;

private:
    // The base calls the on* hooks while holding ITS lock, so this mutex only
    // guards the handle translation below. It is never held across a submit or
    // an idle wait.
    //
    // Lock order: ResourcePool's base mutex -> this mutex -> manager's mutex.
    // Nothing in these hooks calls back into a ResourcePool public method: the
    // base's mutex is not recursive, and a second acquisition deadlocks.
    mutable std::mutex mutex_;
    VulkanRenderer* renderer_ = nullptr;
    std::unordered_map<uint64_t, uint64_t> buffer_handles_;
    std::unordered_map<uint64_t, uint64_t> texture_handles_;
};

} // namespace copper