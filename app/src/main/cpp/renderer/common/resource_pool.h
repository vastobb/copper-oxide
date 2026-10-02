#pragma once

#include <cstdint>
#include <memory>

#include "renderer_config.h"

namespace copper {

class RendererBase;

enum class ResourceType : uint8_t {
    Buffer = 0,
    Texture = 1,
    Shader = 2,
    Pipeline = 3,
    DescriptorSet = 4,
    Framebuffer = 5,
    Sampler = 6,
    AccelerationStructure = 7,
};

class ResourcePool {
public:
    struct Stats {
        uint64_t total_allocated = 0;
        uint64_t total_freed = 0;
        uint64_t current_allocated = 0;
        uint64_t peak_allocated = 0;
        uint32_t allocation_count = 0;
        uint32_t free_count = 0;
        uint32_t pool_hits = 0;
        uint32_t pool_misses = 0;
    };

    ResourcePool();
    virtual ~ResourcePool();

    ResourcePool(const ResourcePool&) = delete;
    ResourcePool& operator=(const ResourcePool&) = delete;
    ResourcePool(ResourcePool&&) noexcept = default;
    ResourcePool& operator=(ResourcePool&&) noexcept = default;

    bool initialize(RendererBase* renderer);
    void shutdown();

    virtual uint64_t allocateBuffer(uint64_t size, uint32_t usage, uint32_t memory_flags = 0);
    virtual uint64_t allocateTexture(uint32_t width, uint32_t height, uint32_t format, uint32_t usage, uint32_t mip_levels = 1);
    virtual void free(uint64_t handle);
    virtual void releaseAll();
    virtual void trim(int level);

    virtual void setMaxPoolSizeMb(size_t size_mb);
    virtual size_t getMaxPoolSizeMb() const;
    virtual uint64_t getCurrentUsageMb() const;
    virtual uint64_t getPeakUsageMb() const;
    virtual Stats getStats() const;

protected:
    virtual bool onAllocateBuffer(uint64_t handle, uint64_t size, uint32_t usage, uint32_t memory_flags) = 0;
    virtual bool onAllocateTexture(uint64_t handle, uint32_t width, uint32_t height, uint32_t format, uint32_t usage, uint32_t mip_levels) = 0;
    virtual void onFree(uint64_t handle) = 0;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace copper