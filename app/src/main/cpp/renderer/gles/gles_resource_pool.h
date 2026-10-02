#pragma once

#include "resource_pool.h"

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace copper {

class RendererBase;

// GLES backend for ResourcePool.
//
// WHO OWNS THE MEMORY: not this class. ResourcePool::Impl is a LIFETIME AND
// POOLING POLICY layer -- it decides when a resource may be recycled, tracks
// byte budgets and hit/miss statistics. The actual GL objects (GLuint buffers,
// GLuint textures) belong to BufferManager / TextureManager, which is also
// where the rest of the engine looks them up (getBufferManager(),
// getTextureManager()). This class therefore only forwards, and keeps a map from
// the pool's handle space to the backend manager's handle space.
//
// Keeping the allocation in the backend managers is a requirement, not an
// optimisation: two competing allocation systems (one here, one in
// BufferManager::allocateFromPool) would hand out handles that the other system
// does not know about, and a texture destroyed through the pool would leak the
// GL name that the texture manager still tracks.
//
// HANDLE SPACES: allocateBuffer()/allocateTexture() return the POOL handle, not
// the backend handle, so a caller that wants to bind/draw has to translate once.
// Use backendHandle() / resourceTypeOf() (or poolHandleOf() for the reverse
// direction). A pool hit does not re-run onAllocateBuffer, so the mapping stays
// valid for the whole life of the resource.
//
// DEADLOCK NOTE: the base calls the on* hooks while holding its own mutex, so
// no hook here may call back into a public ResourcePool method. The lock order
// is always base pool lock -> this class's mutex_ -> backend manager lock,
// never the reverse.
class GLESCResourcePool : public ResourcePool {
public:
    GLESCResourcePool();
    ~GLESCResourcePool() override;

    // ResourcePool::initialize() is NOT virtual, so a call through a
    // ResourcePool* would silently skip setRenderer() and every allocation
    // would then fail the null-manager check. Declare it anyway and forward, so
    // that a call through a GLESCResourcePool* is complete.
    bool initialize(RendererBase* renderer);

    // Explicit renderer injection for callers that only hold a ResourcePool*.
    void setRenderer(RendererBase* renderer);
    RendererBase* renderer() const;

    // Translates a pool handle into the BufferManager / TextureManager handle
    // that can actually be bound. Returns false for handles this pool does not
    // own (including 0 and handles already freed).
    bool backendHandle(uint64_t pool_handle, uint64_t& out_backend_handle) const;

    // Same lookup, 0 when unknown. Convenience wrapper for call sites that
    // treat 0 as "nothing bound".
    uint64_t backendHandle(uint64_t pool_handle) const;

    // Which manager a pool handle belongs to. Needed because onFree() is given
    // only the handle and still has to pick destroyBuffer() vs destroyTexture().
    bool resourceTypeOf(uint64_t pool_handle, ResourceType& out_type) const;

    // Reverse lookup: which pool handle owns this backend handle? 0 when the
    // backend handle did not come from this pool.
    uint64_t poolHandleOf(uint64_t backend_handle) const;

    // Live resources this pool is keeping alive. Pooled-but-free resources are
    // included, because the GL object stays alive until onFree() runs.
    uint32_t liveBufferCount() const;
    uint32_t liveTextureCount() const;

    // Number of allocations that could not be satisfied, because the renderer
    // or the relevant backend manager was missing. A non-zero value means every
    // failure since the last reset hit the same environment problem.
    uint64_t allocationFailureCount() const;

protected:
    bool onAllocateBuffer(uint64_t handle, uint64_t size, uint32_t usage,
                          uint32_t memory_flags) override;
    bool onAllocateTexture(uint64_t handle, uint32_t width, uint32_t height,
                           uint32_t format, uint32_t usage,
                           uint32_t mip_levels) override;
    void onFree(uint64_t handle) override;

private:
    struct Mapping {
        ResourceType type = ResourceType::Buffer;
        uint64_t backend_handle = 0;
    };

    // Backend handle -> pool handle, so the reverse lookup is a single hash
    // instead of a scan.
    void remember(uint64_t pool_handle, ResourceType type, uint64_t backend_handle);
    void forget(uint64_t pool_handle);

    // Called from the failure paths of the allocation hooks.
    void noteAllocationFailure();

    // Guards every member below. Held across the calls into BufferManager /
    // TextureManager on purpose: that is what stops another thread from freeing
    // the mapping between "handle resolved" and "handle used".
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, Mapping> mappings_;
    std::unordered_map<uint64_t, uint64_t> backend_to_pool_;
    RendererBase* renderer_ = nullptr;
    uint64_t allocation_failures_ = 0;
};

} // namespace copper
