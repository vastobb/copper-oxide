#include "vulkan_resource_pool.h"

// The VulkanRenderer accessors used below are the ones the integrator must add
// to VulkanRenderer; see the class comment in the header. getBufferManager() /
// getTextureManager() already exist as RendererBase overrides.
#include "vulkan_renderer.h"

#include "buffer_manager.h"
#include "texture_manager.h"

#include <android/log.h>

#define LOG_TAG "CopperOxide-VK"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

VulkanResourcePool::VulkanResourcePool(VulkanRenderer* renderer) : renderer_(renderer) {}

VulkanResourcePool::~VulkanResourcePool() {
    // ResourcePool::shutdown() is not virtual, so the destructor is the only
    // guaranteed teardown. Leaking here is safe -- VulkanBufferManager and
    // VulkanTextureManager destroy every VkBuffer/VmaAllocation in their own
    // shutdown -- but the maps are cleared so a double free cannot happen if
    // releaseAll() already ran.
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_handles_.clear();
    texture_handles_.clear();
}

// ---------------------------------------------------------------------------
// base hooks
// ---------------------------------------------------------------------------

bool VulkanResourcePool::onAllocateBuffer(uint64_t handle, uint64_t size, uint32_t usage,
                                          uint32_t memory_flags) {
    if (renderer_ == nullptr) {
        LOGE("pool allocateBuffer(%llu): no renderer attached", static_cast<unsigned long long>(handle));
        return false;
    }
    // Guard the manager, not the pool: a null manager means initializeManagers()
    // has not run yet, and creating a VkBuffer here anyway is impossible.
    BufferManager* buffer_manager = renderer_->getBufferManager();
    if (buffer_manager == nullptr) {
        LOGE("pool allocateBuffer(%llu): getBufferManager() is null; refusing to invent a"
             " second allocation path",
             static_cast<unsigned long long>(handle));
        return false;
    }
    if (size == 0) {
        LOGW("pool allocateBuffer(%llu): size 0 rejected", static_cast<unsigned long long>(handle));
        return false;
    }

    // WHY the base API and not the Vulkan subclass: createBuffer() is the public
    // entry point that allocates a handle, records the bookkeeping and forwards
    // to onCreateBuffer(), where the actual vkCreateBuffer/VMA allocation lives.
    const uint64_t manager_handle = buffer_manager->createBuffer(size, usage, memory_flags);
    if (manager_handle == 0) {
        LOGE("pool allocateBuffer(%llu): BufferManager::createBuffer(%llu) failed",
             static_cast<unsigned long long>(handle), static_cast<unsigned long long>(size));
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    buffer_handles_[handle] = manager_handle;
    return true;
}

bool VulkanResourcePool::onAllocateTexture(uint64_t handle, uint32_t width, uint32_t height,
                                           uint32_t format, uint32_t usage, uint32_t mip_levels) {
    if (renderer_ == nullptr) {
        LOGE("pool allocateTexture(%llu): no renderer attached",
             static_cast<unsigned long long>(handle));
        return false;
    }
    TextureManager* texture_manager = renderer_->getTextureManager();
    if (texture_manager == nullptr) {
        LOGE("pool allocateTexture(%llu): getTextureManager() is null; refusing to invent a"
             " second allocation path",
             static_cast<unsigned long long>(handle));
        return false;
    }
    if (width == 0 || height == 0) {
        LOGW("pool allocateTexture(%llu): %ux%u rejected", static_cast<unsigned long long>(handle),
             width, height);
        return false;
    }

    const uint64_t manager_handle =
        texture_manager->createTexture2D(width, height, format, usage, mip_levels);
    if (manager_handle == 0) {
        LOGE("pool allocateTexture(%llu): TextureManager::createTexture2D(%ux%u) failed",
             static_cast<unsigned long long>(handle), width, height);
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    texture_handles_[handle] = manager_handle;
    return true;
}

void VulkanResourcePool::onFree(uint64_t handle) {
    if (renderer_ == nullptr) {
        return;
    }

    // Take the manager handle under the lock, then release it before calling
    // into the manager: the manager takes its own lock and, if it ever calls
    // back into this pool, holding ours across that call is a deadlock.
    uint64_t buffer_handle = 0;
    uint64_t texture_handle = 0;
    bool is_buffer = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto buffer_it = buffer_handles_.find(handle);
        if (buffer_it != buffer_handles_.end()) {
            buffer_handle = buffer_it->second;
            is_buffer = true;
            buffer_handles_.erase(buffer_it);
        } else {
            const auto texture_it = texture_handles_.find(handle);
            if (texture_it != texture_handles_.end()) {
                texture_handle = texture_it->second;
                texture_handles_.erase(texture_it);
            }
        }
    }

    if (is_buffer) {
        // Matching destroy for the matching create: onAllocateBuffer went
        // through createBuffer, so onFree goes through destroyBuffer.
        if (BufferManager* buffer_manager = renderer_->getBufferManager()) {
            buffer_manager->destroyBuffer(buffer_handle);
        }
        return;
    }
    if (texture_handle != 0) {
        if (TextureManager* texture_manager = renderer_->getTextureManager()) {
            texture_manager->destroyTexture(texture_handle);
        }
        return;
    }

    // Not an error: the base calls onFree for entries that were pooled without
    // ever reaching onAllocateBuffer (a rejected allocation), and releaseAll()
    // walks the whole table.
    LOGW("pool onFree(%llu): no backend handle recorded; nothing to destroy",
         static_cast<unsigned long long>(handle));
}

uint64_t VulkanResourcePool::buffer_handle(uint64_t pool_handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = buffer_handles_.find(pool_handle);
    return it != buffer_handles_.end() ? it->second : 0;
}

uint64_t VulkanResourcePool::texture_handle(uint64_t pool_handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = texture_handles_.find(pool_handle);
    return it != texture_handles_.end() ? it->second : 0;
}

uint64_t VulkanResourcePool::live_buffer_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffer_handles_.size();
}

uint64_t VulkanResourcePool::live_texture_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return texture_handles_.size();
}

} // namespace copper