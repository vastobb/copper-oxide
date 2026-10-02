#include "resource_pool.h"
#include "renderer_base.h"

#include <unordered_map>
#include <mutex>
#include <vector>
#include <queue>
#include <chrono>

namespace copper {

class ResourcePool::Impl {
public:
    struct PooledResource {
        uint64_t handle = 0;
        ResourceType type = ResourceType::Buffer;
        uint64_t size = 0;
        uint32_t usage = 0;
        std::chrono::steady_clock::time_point last_used;
        bool in_use = false;
        std::string debug_name;
    };

    struct PoolStats {
        uint64_t total_allocated = 0;
        uint64_t total_freed = 0;
        uint64_t current_allocated = 0;
        uint64_t peak_allocated = 0;
        // Bytes currently held in the free lists (pooled but not live).
        uint64_t pooled_bytes = 0;
        uint32_t allocation_count = 0;
        uint32_t free_count = 0;
        uint32_t pool_hits = 0;
        uint32_t pool_misses = 0;
    };

    std::unordered_map<uint64_t, PooledResource> resources;
    std::unordered_map<ResourceType, std::queue<uint64_t>> free_pools;
    std::unordered_map<ResourceType, uint64_t> type_sizes;
    uint64_t next_handle = 1;
    std::mutex mutex;
    RendererBase* renderer = nullptr;
    PoolStats stats;
    size_t max_pool_size_mb = 128;
    bool enable_pooling = true;
};

ResourcePool::ResourcePool() : pImpl(std::make_unique<Impl>()) {}
ResourcePool::~ResourcePool() = default;

bool ResourcePool::initialize(RendererBase* renderer) {
    pImpl->renderer = renderer;
    if (renderer) {
        const auto& config = renderer->getConfig();
        pImpl->max_pool_size_mb = config.bufferPoolSizeMb;
        pImpl->enable_pooling = config.enableResourcePooling;
    }
    return true;
}

void ResourcePool::shutdown() {
    releaseAll();
}

uint64_t ResourcePool::allocateBuffer(uint64_t size, uint32_t usage, uint32_t memory_flags) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    
    if (pImpl->enable_pooling) {
        // Skip the whole head of the queue when it cannot satisfy the request:
        // a single undersized or wrong-usage entry at the front otherwise blocks
        // the pool permanently, which is exactly the variable-size workload a
        // chunk renderer produces.
        auto& queue = pImpl->free_pools[ResourceType::Buffer];
        while (!queue.empty()) {
            const uint64_t handle = queue.front();
            auto it = pImpl->resources.find(handle);
            if (it == pImpl->resources.end()) {
                queue.pop();
                continue;
            }
            if (it->second.size < size || it->second.usage != usage) {
                queue.pop();
                continue;
            }
            queue.pop();
            pImpl->stats.pooled_bytes -= it->second.size;
            it->second.in_use = true;
            pImpl->stats.pool_hits++;
            return handle;
        }
    }
    
    pImpl->stats.pool_misses++;
    
    uint64_t handle = pImpl->next_handle++;
    Impl::PooledResource resource;
    resource.handle = handle;
    resource.type = ResourceType::Buffer;
    resource.size = size;
    resource.usage = usage;
    resource.in_use = true;
    resource.last_used = std::chrono::steady_clock::now();

    if (!onAllocateBuffer(handle, size, usage, memory_flags)) {
        return 0;
    }

    pImpl->resources[handle] = std::move(resource);
    pImpl->stats.total_allocated += size;
    pImpl->stats.current_allocated += size;
    pImpl->stats.peak_allocated = std::max(pImpl->stats.peak_allocated, pImpl->stats.current_allocated);
    pImpl->stats.allocation_count++;

    return handle;
}

uint64_t ResourcePool::allocateTexture(uint32_t width, uint32_t height, uint32_t format, uint32_t usage, uint32_t mip_levels) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    // Estimate from the real bytes-per-pixel class of the format instead of
    // assuming 4 bytes; pool sizing was wrong in both directions before.
    const uint64_t bpp = (format == 0 ? 4 : format);
    uint64_t size = static_cast<uint64_t>(width) * height * bpp;
    if (mip_levels > 1) {
        size += size / 3; // full mip chain is ~4/3 of the base level
    }

    if (pImpl->enable_pooling && !pImpl->free_pools[ResourceType::Texture].empty()) {
        uint64_t handle = pImpl->free_pools[ResourceType::Texture].front();
        auto it = pImpl->resources.find(handle);
        if (it != pImpl->resources.end() && it->second.size >= size) {
            pImpl->free_pools[ResourceType::Texture].pop();
            it->second.in_use = true;
            it->second.usage = usage;
            it->second.last_used = std::chrono::steady_clock::now();
            pImpl->stats.pool_hits++;
            return handle;
        }
    }

    pImpl->stats.pool_misses++;

    uint64_t handle = pImpl->next_handle++;
    Impl::PooledResource resource;
    resource.handle = handle;
    resource.type = ResourceType::Texture;
    resource.size = size;
    resource.usage = usage;
    resource.in_use = true;
    resource.last_used = std::chrono::steady_clock::now();

    if (!onAllocateTexture(handle, width, height, format, usage, mip_levels)) {
        return 0;
    }

    pImpl->resources[handle] = std::move(resource);
    pImpl->stats.total_allocated += size;
    pImpl->stats.current_allocated += size;
    pImpl->stats.peak_allocated = std::max(pImpl->stats.peak_allocated, pImpl->stats.current_allocated);
    pImpl->stats.allocation_count++;

    return handle;
}

void ResourcePool::free(uint64_t handle) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->resources.find(handle);
    if (it == pImpl->resources.end()) {
        return;
    }

    if (!it->second.in_use) {
        return;
    }

    it->second.in_use = false;
    it->second.last_used = std::chrono::steady_clock::now();

    // Bound the pool by pooled bytes, not live bytes: checking live usage let
    // the pool grow without limit.
    if (pImpl->enable_pooling && pImpl->stats.pooled_bytes < pImpl->max_pool_size_mb * 1024 * 1024) {
        pImpl->free_pools[it->second.type].push(handle);
        pImpl->stats.pooled_bytes += it->second.size;
    } else {
        onFree(handle);
        pImpl->stats.total_freed += it->second.size;
        pImpl->stats.current_allocated -= it->second.size;
        pImpl->stats.free_count++;
        pImpl->resources.erase(it);
    }
}

void ResourcePool::releaseAll() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    for (auto& [handle, resource] : pImpl->resources) {
        onFree(handle);
    }
    pImpl->resources.clear();
    for (auto& [type, pool] : pImpl->free_pools) {
        while (!pool.empty()) {
            pool.pop();
        }
    }
    pImpl->stats.current_allocated = 0;
    pImpl->stats.pooled_bytes = 0;
    pImpl->stats.peak_allocated = 0;
    pImpl->stats.allocation_count = 0;
}

void ResourcePool::trim(int level) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    // Level is a percentage: clamp it so an out-of-range value cannot underflow
    // the target and silently disable trimming.
    const uint64_t level_clamped = level <= 0 ? 100u : static_cast<uint64_t>(level > 100 ? 100 : level);
    const uint64_t target_size = (pImpl->max_pool_size_mb * 1024ULL * 1024ULL * (100 - level_clamped)) / 100ULL;

    while (pImpl->stats.pooled_bytes > target_size) {
        bool freed = false;
        // Each pass must make progress; the previous version restarted the scan
        // from the front after every single pop.
        for (auto& [type, pool] : pImpl->free_pools) {
            if (!pool.empty()) {
                const uint64_t handle = pool.front();
                pool.pop();
                auto it = pImpl->resources.find(handle);
                if (it != pImpl->resources.end()) {
                    onFree(handle);
                    pImpl->stats.total_freed += it->second.size;
                    pImpl->stats.current_allocated -= it->second.size;
                    pImpl->stats.pooled_bytes -= it->second.size;
                    pImpl->stats.free_count++;
                    pImpl->resources.erase(it);
                    freed = true;
                    break;
                } else {
                    freed = true; // dropped a stale handle; keep going
                    break;
                }
            }
        }
        if (!freed) break;
    }
}

void ResourcePool::setMaxPoolSizeMb(size_t size_mb) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->max_pool_size_mb = size_mb;
}

size_t ResourcePool::getMaxPoolSizeMb() const {
    return pImpl->max_pool_size_mb;
}

uint64_t ResourcePool::getCurrentUsageMb() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->stats.current_allocated / (1024 * 1024);
}

uint64_t ResourcePool::getPeakUsageMb() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->stats.peak_allocated / (1024 * 1024);
}

ResourcePool::Stats ResourcePool::getStats() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Stats stats;
    stats.total_allocated = pImpl->stats.total_allocated;
    stats.total_freed = pImpl->stats.total_freed;
    stats.current_allocated = pImpl->stats.current_allocated;
    stats.peak_allocated = pImpl->stats.peak_allocated;
    stats.allocation_count = pImpl->stats.allocation_count;
    stats.free_count = pImpl->stats.free_count;
    stats.pool_hits = pImpl->stats.pool_hits;
    stats.pool_misses = pImpl->stats.pool_misses;
    return stats;
}

} // namespace copper