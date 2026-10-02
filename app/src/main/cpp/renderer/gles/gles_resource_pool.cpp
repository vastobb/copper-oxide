#include "gles_resource_pool.h"

#include "buffer_manager.h"
#include "renderer_base.h"
#include "texture_manager.h"

#include <android/log.h>

#define LOG_TAG "CopperOxide-GLESCResourcePool"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

GLESCResourcePool::GLESCResourcePool() = default;
GLESCResourcePool::~GLESCResourcePool() = default;

bool GLESCResourcePool::initialize(RendererBase* renderer_base) {
    setRenderer(renderer_base);
    // The base picks up bufferPoolSizeMb / enableResourcePooling from the
    // renderer config; those come from the base implementation, not from here.
    return ResourcePool::initialize(renderer_base);
}

void GLESCResourcePool::setRenderer(RendererBase* renderer_base) {
    std::lock_guard<std::mutex> lock(mutex_);
    renderer_ = renderer_base;
}

RendererBase* GLESCResourcePool::renderer() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return renderer_;
}

void GLESCResourcePool::remember(uint64_t pool_handle, ResourceType type,
                                 uint64_t backend_handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    Mapping mapping;
    mapping.type = type;
    mapping.backend_handle = backend_handle;
    mappings_[pool_handle] = mapping;
    backend_to_pool_[backend_handle] = pool_handle;
}

void GLESCResourcePool::forget(uint64_t pool_handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = mappings_.find(pool_handle);
    if (it == mappings_.end()) {
        return;
    }
    backend_to_pool_.erase(it->second.backend_handle);
    mappings_.erase(it);
}

bool GLESCResourcePool::backendHandle(uint64_t pool_handle,
                                      uint64_t& out_backend_handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = mappings_.find(pool_handle);
    if (it == mappings_.end() || it->second.backend_handle == 0) {
        return false;
    }
    out_backend_handle = it->second.backend_handle;
    return true;
}

uint64_t GLESCResourcePool::backendHandle(uint64_t pool_handle) const {
    uint64_t backend = 0;
    backendHandle(pool_handle, backend);
    return backend;
}

bool GLESCResourcePool::resourceTypeOf(uint64_t pool_handle,
                                       ResourceType& out_type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = mappings_.find(pool_handle);
    if (it == mappings_.end()) {
        return false;
    }
    out_type = it->second.type;
    return true;
}

uint64_t GLESCResourcePool::poolHandleOf(uint64_t backend_handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = backend_to_pool_.find(backend_handle);
    return it == backend_to_pool_.end() ? 0 : it->second;
}

uint32_t GLESCResourcePool::liveBufferCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t count = 0;
    for (const auto& [pool_handle, mapping] : mappings_) {
        (void)pool_handle;
        if (mapping.type == ResourceType::Buffer) {
            ++count;
        }
    }
    return count;
}

uint32_t GLESCResourcePool::liveTextureCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t count = 0;
    for (const auto& [pool_handle, mapping] : mappings_) {
        (void)pool_handle;
        if (mapping.type == ResourceType::Texture) {
            ++count;
        }
    }
    return count;
}

uint64_t GLESCResourcePool::allocationFailureCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return allocation_failures_;
}

void GLESCResourcePool::noteAllocationFailure() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++allocation_failures_;
}

// --- buffers ---------------------------------------------------------------

bool GLESCResourcePool::onAllocateBuffer(uint64_t handle, uint64_t size,
                                         uint32_t usage, uint32_t memory_flags) {
    // `handle` is the pool handle the base is about to publish; the backend
    // handle comes back from BufferManager and is a different number.
    // Named renderer_base: a local called `renderer` would shadow the
    // renderer() accessor on the right-hand side of its own initialiser.
    RendererBase* renderer_base = renderer();
    if (renderer_base == nullptr) {
        noteAllocationFailure();
        LOGE("buffer allocation refused: pool handle %llu has no renderer "
             "(call setRenderer()/initialize() before allocating)",
             static_cast<unsigned long long>(handle));
        return false;
    }
    BufferManager* buffers = renderer_base->getBufferManager();
    if (buffers == nullptr) {
        noteAllocationFailure();
        LOGE("buffer allocation refused: the renderer exposes no BufferManager");
        return false;
    }

    // GL objects are created here and only here. BufferManager owns the GLuint
    // and its own handle, so nothing in the pool can leak or double free it.
    const uint64_t backend_handle =
            buffers->createBuffer(size, usage, memory_flags);
    if (backend_handle == 0) {
        // Honest failure: the base turns a false into "handle 0", which callers
        // already have to treat as "no buffer".
        noteAllocationFailure();
        LOGW("createBuffer(%llu, usage 0x%x) failed for pool handle %llu",
             static_cast<unsigned long long>(size), usage,
             static_cast<unsigned long long>(handle));
        return false;
    }

    remember(handle, ResourceType::Buffer, backend_handle);
    return true;
}

// --- textures --------------------------------------------------------------

bool GLESCResourcePool::onAllocateTexture(uint64_t handle, uint32_t width,
                                          uint32_t height, uint32_t format,
                                          uint32_t usage, uint32_t mip_levels) {
    // Named renderer_base: a local called `renderer` would shadow the
    // renderer() accessor on the right-hand side of its own initialiser.
    RendererBase* renderer_base = renderer();
    if (renderer_base == nullptr) {
        noteAllocationFailure();
        LOGE("texture allocation refused: pool handle %llu has no renderer "
             "(call setRenderer()/initialize() before allocating)",
             static_cast<unsigned long long>(handle));
        return false;
    }
    TextureManager* textures = renderer_base->getTextureManager();
    if (textures == nullptr) {
        noteAllocationFailure();
        LOGE("texture allocation refused: the renderer exposes no TextureManager");
        return false;
    }

    // 2D is the only shape the pool's handle model describes (one GL object per
    // handle, no array/cube layer data), so a pooled allocation is always 2D.
    const uint64_t backend_handle =
            textures->createTexture2D(width, height, format, usage, mip_levels);
    if (backend_handle == 0) {
        noteAllocationFailure();
        LOGW("createTexture2D(%ux%u, format %u, usage 0x%x) failed for pool "
             "handle %llu",
             width, height, format, usage,
             static_cast<unsigned long long>(handle));
        return false;
    }

    remember(handle, ResourceType::Texture, backend_handle);
    return true;
}

// --- free ------------------------------------------------------------------

void GLESCResourcePool::onFree(uint64_t handle) {
    // The base hands over only the handle, so the type has to come from the
    // mapping recorded at allocation time.
    Mapping mapping;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mappings_.find(handle);
        if (it == mappings_.end()) {
            // Not fatal: a stale handle means the pool is being torn down after
            // the manager already went away, or free() raced with releaseAll().
            LOGD("onFree(%llu): not a live pool resource, nothing to destroy",
                 static_cast<unsigned long long>(handle));
            return;
        }
        mapping = it->second;
    }

    // Named renderer_base: a local called `renderer` would shadow the
    // renderer() accessor on the right-hand side of its own initialiser.
    RendererBase* renderer_base = renderer();
    if (renderer_base == nullptr) {
        LOGW("onFree(%llu): no renderer_base, leaking backend handle %llu",
             static_cast<unsigned long long>(handle),
             static_cast<unsigned long long>(mapping.backend_handle));
        return;
    }

    if (mapping.type == ResourceType::Texture) {
        TextureManager* textures = renderer_base->getTextureManager();
        if (textures == nullptr) {
            LOGW("onFree(%llu): no TextureManager, leaking texture %llu",
                 static_cast<unsigned long long>(handle),
                 static_cast<unsigned long long>(mapping.backend_handle));
            return;
        }
        textures->destroyTexture(mapping.backend_handle);
    } else {
        BufferManager* buffers = renderer_base->getBufferManager();
        if (buffers == nullptr) {
            LOGW("onFree(%llu): no BufferManager, leaking buffer %llu",
                 static_cast<unsigned long long>(handle),
                 static_cast<unsigned long long>(mapping.backend_handle));
            return;
        }
        buffers->destroyBuffer(mapping.backend_handle);
    }

    forget(handle);
}

} // namespace copper
