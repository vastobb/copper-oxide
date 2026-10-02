#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <memory>

#include "renderer_config.h"

namespace copper {

class RendererBase;

class TextureManager {
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

    TextureManager();
    virtual ~TextureManager();

    TextureManager(const TextureManager&) = delete;
    TextureManager& operator=(const TextureManager&) = delete;
    TextureManager(TextureManager&&) noexcept = default;
    TextureManager& operator=(TextureManager&&) noexcept = default;

    bool initialize(RendererBase* renderer);
    void shutdown();

    virtual uint64_t createTexture2D(uint32_t width, uint32_t height, uint32_t format, uint32_t usage, uint32_t mip_levels = 1);
    virtual uint64_t createTexture3D(uint32_t width, uint32_t height, uint32_t depth, uint32_t format, uint32_t usage, uint32_t mip_levels = 1);
    virtual uint64_t createTextureArray(uint32_t width, uint32_t height, uint32_t array_layers, uint32_t format, uint32_t usage, uint32_t mip_levels = 1);
    virtual uint64_t createTextureCube(uint32_t width, uint32_t height, uint32_t format, uint32_t usage, uint32_t mip_levels = 1);
    virtual void destroyTexture(uint64_t handle);

    /**
     * Uploads one region of one mip level.
     *
     * Returns false when the handle is unknown, the level or layer is out of
     * range, or the region leaves the level. Like updateBuffer, this used to
     * return void, so a rejected upload was indistinguishable from a successful
     * one at the JNI boundary.
     *
     * A zero width, height or depth means "the whole level", which the backend
     * resolves against the stored extent.
     */
    virtual bool updateTexture(uint64_t handle, uint32_t mip_level, uint32_t array_layer, uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth, const void* data, uint64_t data_size);
    virtual void copyTexture(uint64_t src, uint64_t dst, uint32_t src_mip, uint32_t dst_mip, uint32_t src_layer, uint32_t dst_layer);
    virtual void generateMipmaps(uint64_t handle);

    virtual uint64_t loadTextureFromMemory(const void* data, uint64_t size, uint32_t format, bool generate_mipmaps);
    virtual uint64_t getOrCreateTexture(const std::string& key, std::function<uint64_t()> creator);

    virtual void setTextureDebugName(uint64_t handle, const std::string& name);
    virtual void trimCache(int level);
    virtual size_t getCacheSizeMb() const;
    virtual void setMaxCacheSizeMb(size_t size_mb);

protected:
    virtual bool onCreateTexture2D(uint64_t handle, uint32_t width, uint32_t height, uint32_t format, uint32_t usage, uint32_t mip_levels) = 0;
    virtual bool onCreateTexture3D(uint64_t handle, uint32_t width, uint32_t height, uint32_t depth, uint32_t format, uint32_t usage, uint32_t mip_levels) = 0;
    virtual bool onCreateTextureArray(uint64_t handle, uint32_t width, uint32_t height, uint32_t array_layers, uint32_t format, uint32_t usage, uint32_t mip_levels) = 0;
    virtual void onDestroyTexture(uint64_t handle) = 0;
    virtual void onUpdateTexture(uint64_t handle, uint32_t mip_level, uint32_t array_layer, uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth, const void* data, uint64_t data_size) = 0;
    virtual void onCopyTexture(uint64_t src, uint64_t dst, uint32_t src_mip, uint32_t dst_mip, uint32_t src_layer, uint32_t dst_layer) = 0;
    virtual void onGenerateMipmaps(uint64_t handle) = 0;
    virtual bool onLoadTextureFromMemory(uint64_t handle, const void* data, uint64_t size, uint32_t format, bool generate_mipmaps) = 0;
    virtual void onSetTextureDebugName(uint64_t handle, const std::string& name) = 0;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace copper