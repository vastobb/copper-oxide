#pragma once

#include <GLES3/gl32.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "texture_manager.h"

namespace copper {

class GLESCRenderer;

// A cube map is a six layer array that is sampled as a cube, so the layer count
// is what separates the two in this backend.
constexpr uint32_t kTextureCubeFaceCount = 6;

// TextureManager passes the pixel format down as an opaque uint32_t. These are
// the formats the GLES backend understands; the values are an internal contract
// and callers only ever see them through TextureFormat.
enum class TextureFormat : uint32_t {
    Undefined = 0,
    // 8 bit per channel
    R8 = 1,
    RG8 = 2,
    // ES has no sized RGB8 format; RGB8 is promoted to RGBA8, which costs one
    // byte per pixel and keeps every consumer on a single code path.
    RGB8 = 3,
    RGBA8 = 4,
    SRGB8_ALPHA8 = 5,
    // half float
    R16F = 16,
    RG16F = 17,
    RGBA16F = 18,
    // float
    R32F = 19,
    RG32F = 20,
    RGBA32F = 21,
    // unorm integers
    R16 = 32,
    RG16 = 33,
    RGBA16 = 34,
    // depth / stencil, always single level
    Depth16 = 48,
    Depth24 = 49,
    Depth32F = 50,
    Depth24Stencil8 = 51,
    Depth32FStencil8 = 52,
    // ASTC: the preferred mobile block format, block encoded 128 bit blocks
    ASTC4x4 = 64,
    ASTC5x5 = 65,
    ASTC6x6 = 66,
    ASTC8x8 = 67,
    ASTC10x10 = 68,
    ASTC12x12 = 69,
    // ETC2: GLES 3.0 core, so always decodable
    ETC2RGB8 = 80,
    ETC2RGBA8 = 81,
    ETC2RGB8PunchthroughAlpha1 = 82,
};

// Purpose of a texture. GLES derives renderability from the internal format
// itself, so these bits only steer creation (level count, filtering, cube
// sampling rules).
enum class TextureUsage : uint32_t {
    None = 0,
    Sampler = 1u << 0,
    ColorAttachment = 1u << 1,
    DepthAttachment = 1u << 2,
    Storage = 1u << 3,
};

inline uint32_t operator|(TextureUsage lhs, TextureUsage rhs) {
    return static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs);
}

// Note: TextureUsage::None always matches because it is zero.
inline bool hasFlag(uint32_t bits, TextureUsage flag) {
    const uint32_t mask = static_cast<uint32_t>(flag);
    return (bits & mask) == mask;
}

// OpenGL ES backend for TextureManager.
//
// Storage is immutable (glTexStorage2D/3D) so a mip chain is allocated once and
// uploads stay glTexSubImage*, which is cheaper than re-specifying the format on
// every update. Cube maps are the exception: ES restricts glTexStorage2D on
// GL_TEXTURE_CUBE_MAP, so they are allocated level by level with glTexImage2D.
//
// Capability fallback for compressed formats: the requested block format is kept
// when the device can decode it, otherwise ETC2 (ES 3.0 core) and finally plain
// RGBA8 are substituted and the texture records that its payload can no longer be
// uploaded as-is.
//
// Locking: the base class calls the on* hooks while holding its own mutex, so the
// hooks must never call back into a public base method (that would re-lock a
// non-recursive mutex). All state a hook needs lives in textures_, guarded by
// mutex_, so the lock order is always base -> backend and never the reverse.
class GLESCTextureManager : public TextureManager {
public:
    // renderer may be null: every GL call is then skipped.
    explicit GLESCTextureManager(GLESCRenderer* renderer);
    ~GLESCTextureManager() override;

    GLESCTextureManager(const GLESCTextureManager&) = delete;
    GLESCTextureManager& operator=(const GLESCTextureManager&) = delete;
    GLESCTextureManager(GLESCTextureManager&&) = delete;
    GLESCTextureManager& operator=(GLESCTextureManager&&) = delete;

    // Adopts the renderer if the instance was built without one, so a manager
    // created by the backend factory before GLESCRenderer exists still works.
    // Not marked override: the base's initialize() is deliberately non-virtual,
    // because the factory constructs the concrete type already.
    bool initialize(RendererBase* renderer);

    // GL name / sampling target of a handle, used by the state and command
    // managers to bind the texture.
    GLuint glObject(uint64_t handle) const;
    GLenum glTarget(uint64_t handle) const;
    // Internal format actually allocated (may differ from the requested format
    // after the compressed fallback).
    GLenum internalFormat(uint64_t handle) const;
    bool isCubeMap(uint64_t handle) const;
    size_t liveTextureCount() const;

    // True when this thread could issue GL calls: the renderer must own a
    // context *and* a surface, because GLESCRenderer::make_current() refuses to
    // bind anything without both and a GL call without a current context is
    // undefined behaviour.
    bool contextAvailable() const;

    // TextureManager::loadTextureFromMemory() carries a byte count but no
    // extent, and no image decoder is linked into the native library, so the
    // caller declares the extent of the payload before loading. Thread-local so
    // concurrent uploads on different threads cannot pick each other's extent.
    void setPendingUploadExtent(uint32_t width, uint32_t height);

    // Explicit-extent variant of loadTextureFromMemory(); uses the same upload path
    // with the extent supplied up front instead of through the pending extent.
    uint64_t createTextureFromMemory(uint32_t width, uint32_t height, uint32_t format, const void* pixels,
                                     uint64_t size, bool generate_mipmaps);

    // Format mapping, exposed so loaders can stage pixels without duplicating it.
    // glInternalFormat() is the nominal mapping; the capability fallback is
    // applied at creation time and is visible through internalFormat(handle).
    static GLenum glInternalFormat(uint32_t format);
    static bool isCompressedFormat(uint32_t format);
    // Compressed block size in bytes, 0 for uncompressed formats.
    static uint32_t compressedBlockSize(uint32_t format);
    // Upload / read-back format and type for an allocated internal format.
    static GLenum glDataFormat(GLenum internal_format);
    static GLenum glDataType(GLenum internal_format);
    static uint32_t mipChainLength(uint32_t width, uint32_t height, uint32_t depth);

protected:
    bool onCreateTexture2D(uint64_t handle, uint32_t width, uint32_t height, uint32_t format, uint32_t usage,
                           uint32_t mip_levels) override;
    bool onCreateTexture3D(uint64_t handle, uint32_t width, uint32_t height, uint32_t depth, uint32_t format,
                           uint32_t usage, uint32_t mip_levels) override;
    bool onCreateTextureArray(uint64_t handle, uint32_t width, uint32_t height, uint32_t array_layers, uint32_t format,
                              uint32_t usage, uint32_t mip_levels) override;
    void onDestroyTexture(uint64_t handle) override;
    void onUpdateTexture(uint64_t handle, uint32_t mip_level, uint32_t array_layer, uint32_t x, uint32_t y, uint32_t z,
                         uint32_t width, uint32_t height, uint32_t depth, const void* data, uint64_t data_size) override;
    void onCopyTexture(uint64_t src, uint64_t dst, uint32_t src_mip, uint32_t dst_mip, uint32_t src_layer,
                       uint32_t dst_layer) override;
    void onGenerateMipmaps(uint64_t handle) override;
    bool onLoadTextureFromMemory(uint64_t handle, const void* data, uint64_t size, uint32_t format,
                                 bool generate_mipmaps) override;
    void onSetTextureDebugName(uint64_t handle, const std::string& name) override;

    // TextureManager has no cube hook: createTextureCube() forwards to
    // createTextureArray() with six layers, so onCreateTextureArray() delegates to
    // this for that case and the sampling target stays GL_TEXTURE_CUBE_MAP.
    bool onCreateTextureCube(uint64_t handle, uint32_t width, uint32_t height, uint32_t format, uint32_t usage,
                             uint32_t mip_levels);

private:
    struct TextureObject {
        GLuint texture = 0;
        GLenum target = GL_TEXTURE_2D;
        GLenum internal_format = GL_RGBA8;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 1;
        uint32_t layers = 1;
        uint32_t levels = 1;
        // Format the caller asked for, which differs from internal_format after a
        // capability fallback and drives the compressed payload layout.
        uint32_t format = 0;
        bool is_cube = false;
        // Set when the requested format had to be substituted; a compressed
        // payload can then no longer be uploaded because its block layout does
        // not match the allocated format.
        bool format_fallback = false;
        std::string debug_name;
    };

    // Caller must hold mutex_.
    bool glesAtLeast(int major, int minor);
    bool astcSupported();
    GLenum resolveInternalFormat(uint32_t format, bool* out_fallback);
    bool allocateStorage(GLenum target, uint32_t levels, GLenum internal_format, uint32_t width, uint32_t height,
                         uint32_t depth_or_layers);
    void configureParameters(GLenum target, uint32_t levels, bool is_cube, bool filterable);
    // Mip level extent of a level, 0 for levels past the chain.
    void levelExtent(const TextureObject& object, uint32_t mip_level, uint32_t* out_width, uint32_t* out_height,
                     uint32_t* out_depth) const;
    // Bytes the region [width, height, depth] of a mip level needs; 0 when the
    // extent is empty.
    uint64_t regionBytes(uint32_t format, GLenum internal_format, uint32_t width, uint32_t height, uint32_t depth) const;
    void uploadRegion(const TextureObject& object, uint32_t mip_level, uint32_t array_layer, uint32_t x, uint32_t y,
                      uint32_t z, uint32_t width, uint32_t height, uint32_t depth, const void* data, uint64_t data_size);
    void copyLayerOnCpu(const TextureObject& src, const TextureObject& dst, uint32_t src_mip, uint32_t dst_mip,
                        uint32_t src_layer, uint32_t dst_layer, uint32_t width, uint32_t height, uint32_t depth);
    bool createTextureObject(uint64_t handle, GLenum target, uint32_t width, uint32_t height, uint32_t depth,
                             uint32_t layers, uint32_t format, uint32_t usage, uint32_t mip_levels);
    void destroyAllLocked();

    GLESCRenderer* renderer_ = nullptr;
    std::unordered_map<uint64_t, TextureObject> textures_;
    mutable std::mutex mutex_;
    // GL_MAJOR_VERSION / GL_MINOR_VERSION queried once per context; the context
    // can be recreated on surface loss, so the cache is filled lazily and dropped
    // whenever no context is available.
    bool gles_version_known_ = false;
    int gles_major_ = 0;
    int gles_minor_ = 0;
    bool astc_cached_ = false;
    bool astc_available_ = false;
    // Extent of the next loadTextureFromMemory() payload, see
    // setPendingUploadExtent().
    static inline thread_local uint32_t pending_width_ = 0;
    static inline thread_local uint32_t pending_height_ = 0;
};

} // namespace copper