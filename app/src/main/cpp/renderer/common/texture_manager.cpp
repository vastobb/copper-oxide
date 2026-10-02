#include "texture_manager.h"
#include "renderer_base.h"

#include <unordered_map>
#include <mutex>
#include <string>

namespace copper {

class TextureManager::Impl {
public:
    struct Texture {
        uint64_t handle = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 1;
        uint32_t format = 0;
        uint32_t usage = 0;
        uint32_t mip_levels = 1;
        uint32_t array_layers = 1;
        std::string debug_name;
    };

    std::unordered_map<uint64_t, Texture> textures;
    std::unordered_map<std::string, uint64_t> texture_cache;
    std::vector<uint64_t> pool_handles;
    uint64_t next_handle = 1;
    std::mutex mutex;
    RendererBase* renderer = nullptr;
    size_t max_cache_size_mb = 256;
    size_t current_cache_size_mb = 0;
};

TextureManager::TextureManager() : pImpl(std::make_unique<Impl>()) {}
TextureManager::~TextureManager() = default;

bool TextureManager::initialize(RendererBase* renderer) {
    pImpl->renderer = renderer;
    if (renderer) {
        const auto& config = renderer->getConfig();
        pImpl->max_cache_size_mb = config.textureCacheSizeMb;
    }
    return true;
}

void TextureManager::shutdown() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    for (auto& [handle, texture] : pImpl->textures) {
        onDestroyTexture(handle);
    }
    pImpl->textures.clear();
    pImpl->texture_cache.clear();
    pImpl->pool_handles.clear();
    pImpl->current_cache_size_mb = 0;
}

uint64_t TextureManager::createTexture2D(uint32_t width, uint32_t height, uint32_t format, uint32_t usage, uint32_t mip_levels) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    uint64_t handle = pImpl->next_handle++;

    Impl::Texture texture;
    texture.handle = handle;
    texture.width = width;
    texture.height = height;
    texture.format = format;
    texture.usage = usage;
    texture.mip_levels = mip_levels;

    if (!onCreateTexture2D(handle, width, height, format, usage, mip_levels)) {
        return 0;
    }

    pImpl->textures[handle] = std::move(texture);
    return handle;
}

uint64_t TextureManager::createTexture3D(uint32_t width, uint32_t height, uint32_t depth, uint32_t format, uint32_t usage, uint32_t mip_levels) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    uint64_t handle = pImpl->next_handle++;

    Impl::Texture texture;
    texture.handle = handle;
    texture.width = width;
    texture.height = height;
    texture.depth = depth;
    texture.format = format;
    texture.usage = usage;
    texture.mip_levels = mip_levels;

    if (!onCreateTexture3D(handle, width, height, depth, format, usage, mip_levels)) {
        return 0;
    }

    pImpl->textures[handle] = std::move(texture);
    return handle;
}

uint64_t TextureManager::createTextureArray(uint32_t width, uint32_t height, uint32_t array_layers, uint32_t format, uint32_t usage, uint32_t mip_levels) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    uint64_t handle = pImpl->next_handle++;

    Impl::Texture texture;
    texture.handle = handle;
    texture.width = width;
    texture.height = height;
    texture.array_layers = array_layers;
    texture.format = format;
    texture.usage = usage;
    texture.mip_levels = mip_levels;

    if (!onCreateTextureArray(handle, width, height, array_layers, format, usage, mip_levels)) {
        return 0;
    }

    pImpl->textures[handle] = std::move(texture);
    return handle;
}

uint64_t TextureManager::createTextureCube(uint32_t width, uint32_t height, uint32_t format, uint32_t usage, uint32_t mip_levels) {
    return createTextureArray(width, height, 6, format, usage, mip_levels);
}

void TextureManager::destroyTexture(uint64_t handle) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->textures.find(handle);
    if (it == pImpl->textures.end()) {
        return;
    }

    onDestroyTexture(handle);

    for (auto cache_it = pImpl->texture_cache.begin(); cache_it != pImpl->texture_cache.end(); ++cache_it) {
        if (cache_it->second == handle) {
            pImpl->texture_cache.erase(cache_it);
            break;
        }
    }

    auto pool_it = std::find(pImpl->pool_handles.begin(), pImpl->pool_handles.end(), handle);
    if (pool_it != pImpl->pool_handles.end()) {
        pImpl->pool_handles.erase(pool_it);
    }

    pImpl->textures.erase(it);
}

void TextureManager::updateTexture(uint64_t handle, uint32_t mip_level, uint32_t array_layer, uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth, const void* data, uint64_t data_size) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->textures.find(handle);
    if (it == pImpl->textures.end()) {
        return;
    }
    onUpdateTexture(handle, mip_level, array_layer, x, y, z, width, height, depth, data, data_size);
}

void TextureManager::copyTexture(uint64_t src, uint64_t dst, uint32_t src_mip, uint32_t dst_mip, uint32_t src_layer, uint32_t dst_layer) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (pImpl->textures.find(src) == pImpl->textures.end() || pImpl->textures.find(dst) == pImpl->textures.end()) {
        return;
    }
    onCopyTexture(src, dst, src_mip, dst_mip, src_layer, dst_layer);
}

void TextureManager::generateMipmaps(uint64_t handle) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->textures.find(handle);
    if (it == pImpl->textures.end() || it->second.mip_levels <= 1) {
        return;
    }
    onGenerateMipmaps(handle);
}

uint64_t TextureManager::loadTextureFromMemory(const void* data, uint64_t size, uint32_t format, bool generate_mipmaps) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    // In a real implementation, this would decode image data
    // For now, return a placeholder handle
    uint64_t handle = pImpl->next_handle++;
    
    Impl::Texture texture;
    texture.handle = handle;
    texture.format = format;
    texture.mip_levels = generate_mipmaps ? 0 : 1; // 0 = auto

    if (!onLoadTextureFromMemory(handle, data, size, format, generate_mipmaps)) {
        return 0;
    }

    pImpl->textures[handle] = std::move(texture);
    return handle;
}

uint64_t TextureManager::getOrCreateTexture(const std::string& key, std::function<uint64_t()> creator) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->texture_cache.find(key);
    if (it != pImpl->texture_cache.end()) {
        return it->second;
    }
    uint64_t handle = creator();
    if (handle != 0) {
        pImpl->texture_cache[key] = handle;
    }
    return handle;
}

void TextureManager::setTextureDebugName(uint64_t handle, const std::string& name) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->textures.find(handle);
    if (it != pImpl->textures.end()) {
        it->second.debug_name = name;
        onSetTextureDebugName(handle, name);
    }
}

void TextureManager::trimCache(int level) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    size_t target_size = pImpl->max_cache_size_mb * (100 - level) / 100;
    while (pImpl->current_cache_size_mb > target_size && !pImpl->pool_handles.empty()) {
        uint64_t handle = pImpl->pool_handles.back();
        pImpl->pool_handles.pop_back();
        auto it = pImpl->textures.find(handle);
        if (it != pImpl->textures.end()) {
            onDestroyTexture(handle);
            pImpl->current_cache_size_mb -= (it->second.width * it->second.height * 4) / (1024 * 1024);
            pImpl->textures.erase(it);
        }
    }
}

size_t TextureManager::getCacheSizeMb() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->current_cache_size_mb;
}

void TextureManager::setMaxCacheSizeMb(size_t size_mb) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->max_cache_size_mb = size_mb;
}

} // namespace copper