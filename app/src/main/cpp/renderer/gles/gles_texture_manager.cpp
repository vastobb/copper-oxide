#include "gles_texture_manager.h"

#include "gles_renderer.h"

#include <EGL/egl.h>
#include <android/log.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#define LOG_TAG "CopperOxide-GLES-Texture"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// The ASTC tokens belong to GL_EXT_texture_compression_astc_decode, which is an
// extension in ES 3.0/3.1 and is therefore not guaranteed to be present in every
// <GLES3/gl32.h>. The values are fixed by the extension specification.
#ifndef GL_COMPRESSED_RGBA_ASTC_4x4_KHR
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR 0x93B0
#endif
#ifndef GL_COMPRESSED_RGBA_ASTC_5x5_KHR
#define GL_COMPRESSED_RGBA_ASTC_5x5_KHR 0x93B1
#endif
#ifndef GL_COMPRESSED_RGBA_ASTC_6x6_KHR
#define GL_COMPRESSED_RGBA_ASTC_6x6_KHR 0x93B2
#endif
#ifndef GL_COMPRESSED_RGBA_ASTC_8x8_KHR
#define GL_COMPRESSED_RGBA_ASTC_8x8_KHR 0x93B3
#endif
#ifndef GL_COMPRESSED_RGBA_ASTC_10x10_KHR
#define GL_COMPRESSED_RGBA_ASTC_10x10_KHR 0x93B4
#endif
#ifndef GL_COMPRESSED_RGBA_ASTC_12x12_KHR
#define GL_COMPRESSED_RGBA_ASTC_12x12_KHR 0x93BC
#endif

namespace copper {

namespace {

// GL_KHR_debug is an extension in GLES, so glObjectLabel is not declared by
// <GLES3/gl32.h> and has to be resolved through EGL. The length argument is a
// GLsizei in the extension (core desktop GL uses the wider GLsizeiptr), so the
// typedef must keep the 32-bit width or 64-bit ABIs would misread it.
using PFN_glObjectLabel = void (*)(GLenum, GLuint, GLsizei, const GLchar*);

PFN_glObjectLabel resolveObjectLabel(GLESCRenderer* renderer) {
    if (!renderer || !renderer->isExtensionSupported("GL_KHR_debug")) {
        return nullptr;
    }
    return reinterpret_cast<PFN_glObjectLabel>(eglGetProcAddress("glObjectLabel"));
}

// Cube face / slice bound to a target, saved and restored around uploads so the
// texture unit state the state cache tracks stays intact.
GLenum textureBindingQueryFor(GLenum target) {
    // A cube face target maps onto the cube map binding, not onto the 2D one.
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) {
        return GL_TEXTURE_BINDING_CUBE_MAP;
    }
    switch (target) {
        case GL_TEXTURE_2D_ARRAY:
            return GL_TEXTURE_BINDING_2D_ARRAY;
        case GL_TEXTURE_3D:
            return GL_TEXTURE_BINDING_3D;
        case GL_TEXTURE_CUBE_MAP:
            return GL_TEXTURE_BINDING_CUBE_MAP;
        case GL_TEXTURE_2D:
        default:
            return GL_TEXTURE_BINDING_2D;
    }
}

GLuint boundTexture(GLenum target) {
    GLint binding = 0;
    glGetIntegerv(textureBindingQueryFor(target), &binding);
    return static_cast<GLuint>(binding);
}

bool isDepthFormat(GLenum internal_format) {
    switch (internal_format) {
        case GL_DEPTH_COMPONENT16:
        case GL_DEPTH_COMPONENT24:
        case GL_DEPTH_COMPONENT32F:
        case GL_DEPTH24_STENCIL8:
        case GL_DEPTH32F_STENCIL8:
            return true;
        default:
            return false;
    }
}

bool isFloatFormat(GLenum internal_format) {
    switch (internal_format) {
        case GL_R16F:
        case GL_RG16F:
        case GL_RGBA16F:
        case GL_R32F:
        case GL_RG32F:
        case GL_RGBA32F:
            return true;
        default:
            return false;
    }
}

// Uncompressed texel size; 0 for compressed formats (which use block sizes).
uint32_t bytesPerTexel(GLenum internal_format) {
    switch (internal_format) {
        case GL_R8:
            return 1;
        case GL_RG8:
        case GL_DEPTH_COMPONENT16:
            return 2;
        case GL_RGBA8:
        case GL_SRGB8_ALPHA8:
            return 4;
        case GL_R16F:
        case GL_R32F:
        case GL_DEPTH_COMPONENT24:
        case GL_DEPTH_COMPONENT32F:
            return 4;
        case GL_RG16F:
        case GL_RG32F:
            return 8;
        case GL_RGBA16F:
        case GL_RGBA32F:
        case GL_DEPTH24_STENCIL8:
        case GL_DEPTH32F_STENCIL8:
            return 8;
        default:
            return 0;
    }
}

// Compressed block dimensions of the requested format; (0, 0) when the format is
// not block compressed.
void blockExtent(uint32_t format, uint32_t* out_width, uint32_t* out_height) {
    *out_width = 0;
    *out_height = 0;
    switch (static_cast<TextureFormat>(format)) {
        case TextureFormat::ASTC4x4: *out_width = 4; *out_height = 4; break;
        case TextureFormat::ASTC5x5: *out_width = 5; *out_height = 5; break;
        case TextureFormat::ASTC6x6: *out_width = 6; *out_height = 6; break;
        case TextureFormat::ASTC8x8: *out_width = 8; *out_height = 8; break;
        case TextureFormat::ASTC10x10: *out_width = 10; *out_height = 10; break;
        case TextureFormat::ASTC12x12: *out_width = 12; *out_height = 12; break;
        case TextureFormat::ETC2RGB8:
        case TextureFormat::ETC2RGBA8:
        case TextureFormat::ETC2RGB8PunchthroughAlpha1:
            *out_width = 4;
            *out_height = 4;
            break;
        default:
            break;
    }
}

// Upper bound for the CPU round trip in copyLayerOnCpu(); beyond this a layer copy
// is skipped instead of allocating an arbitrarily large temporary.
constexpr uint64_t kMaxCpuCopyBytes = 64ull * 1024ull * 1024ull;

} // namespace

GLESCTextureManager::GLESCTextureManager(GLESCRenderer* renderer) : renderer_(renderer) {}

GLESCTextureManager::~GLESCTextureManager() {
    // TextureManager::~TextureManager() is defaulted and never runs shutdown(), so
    // the base records would simply be dropped. Running it here routes through the
    // on* hooks (base lock, then ours - never the reverse) and leaves the two
    // sides consistent. destroyAllLocked() is the safety net for anything that
    // reached GL without a base record.
    TextureManager::shutdown();

    std::lock_guard<std::mutex> lock(mutex_);
    destroyAllLocked();
}

bool GLESCTextureManager::initialize(RendererBase* renderer) {
    if (!TextureManager::initialize(renderer)) {
        return false;
    }
    if (!renderer_ && renderer) {
        // A manager built before the renderer existed adopts it here; a foreign
        // RendererBase would leave renderer_ null and every GL call inert.
        renderer_ = dynamic_cast<GLESCRenderer*>(renderer);
        if (!renderer_) {
            LOGW("texture manager cannot use this renderer backend");
        }
    }
    return true;
}

bool GLESCTextureManager::contextAvailable() const {
    if (!renderer_) {
        // Constructed without a renderer (unit tests): no GL call is legal.
        return false;
    }
    // GLESCRenderer::make_current() refuses to bind without a surface, and a GL
    // call on a thread without a current context is undefined, so both are
    // required before touching GL.
    if (renderer_->get_egl_context() == EGL_NO_CONTEXT) {
        return false;
    }
    return renderer_->get_egl_surface() != EGL_NO_SURFACE;
}

GLuint GLESCTextureManager::glObject(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle);
    return it == textures_.end() ? 0u : it->second.texture;
}

GLenum GLESCTextureManager::glTarget(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle);
    return it == textures_.end() ? GL_TEXTURE_2D : it->second.target;
}

GLenum GLESCTextureManager::internalFormat(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle);
    return it == textures_.end() ? GL_RGBA8 : it->second.internal_format;
}

bool GLESCTextureManager::isCubeMap(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle);
    return it != textures_.end() && it->second.is_cube;
}

size_t GLESCTextureManager::liveTextureCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return textures_.size();
}

GLenum GLESCTextureManager::glInternalFormat(uint32_t format) {
    switch (static_cast<TextureFormat>(format)) {
        case TextureFormat::R8: return GL_R8;
        case TextureFormat::RG8: return GL_RG8;
        case TextureFormat::RGB8: return GL_RGBA8;  // ES has no sized RGB8 format
        case TextureFormat::RGBA8: return GL_RGBA8;
        case TextureFormat::SRGB8_ALPHA8: return GL_SRGB8_ALPHA8;
        case TextureFormat::R16F: return GL_R16F;
        case TextureFormat::RG16F: return GL_RG16F;
        case TextureFormat::RGBA16F: return GL_RGBA16F;
        case TextureFormat::R32F: return GL_R32F;
        case TextureFormat::RG32F: return GL_RG32F;
        case TextureFormat::RGBA32F: return GL_RGBA32F;
        // R16 / RG16 / RGBA16 are deliberately absent. The Khronos headers give
        // GL_RGBA16 the same token as GL_RGBA8, and GL_R16 / GL_RG16 collide with
        // other ES 3.1 additions, so they cannot appear as distinct switch labels
        // here. Nothing in Copper Oxide requests 16-bit unsigned-normalized
        // textures; returning 0 makes the format report unsupported instead of
        // silently allocating a different format than the caller asked for. The
        // Vulkan backend maps these formats correctly, so a caller that needs
        // them must go through Vulkan or add the tokens under #ifndef guards.
        case TextureFormat::Depth16: return GL_DEPTH_COMPONENT16;
        case TextureFormat::Depth24: return GL_DEPTH_COMPONENT24;
        case TextureFormat::Depth32F: return GL_DEPTH_COMPONENT32F;
        case TextureFormat::Depth24Stencil8: return GL_DEPTH24_STENCIL8;
        case TextureFormat::Depth32FStencil8: return GL_DEPTH32F_STENCIL8;
        case TextureFormat::ASTC4x4: return GL_COMPRESSED_RGBA_ASTC_4x4_KHR;
        case TextureFormat::ASTC5x5: return GL_COMPRESSED_RGBA_ASTC_5x5_KHR;
        case TextureFormat::ASTC6x6: return GL_COMPRESSED_RGBA_ASTC_6x6_KHR;
        case TextureFormat::ASTC8x8: return GL_COMPRESSED_RGBA_ASTC_8x8_KHR;
        case TextureFormat::ASTC10x10: return GL_COMPRESSED_RGBA_ASTC_10x10_KHR;
        case TextureFormat::ASTC12x12: return GL_COMPRESSED_RGBA_ASTC_12x12_KHR;
        case TextureFormat::ETC2RGB8: return GL_COMPRESSED_RGB8_ETC2;
        case TextureFormat::ETC2RGBA8: return GL_COMPRESSED_RGBA8_ETC2_EAC;
        case TextureFormat::ETC2RGB8PunchthroughAlpha1: return GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2;
        case TextureFormat::Undefined:
        default:
            // Unknown opaque format: RGBA8 is the one internal format that is
            // always colour renderable and texture filterable.
            return GL_RGBA8;
    }
}

bool GLESCTextureManager::isCompressedFormat(uint32_t format) {
    uint32_t block_width = 0;
    uint32_t block_height = 0;
    blockExtent(format, &block_width, &block_height);
    return block_width != 0;
}

uint32_t GLESCTextureManager::compressedBlockSize(uint32_t format) {
    switch (static_cast<TextureFormat>(format)) {
        // Every ASTC block size is a 128 bit block.
        case TextureFormat::ASTC4x4:
        case TextureFormat::ASTC5x5:
        case TextureFormat::ASTC6x6:
        case TextureFormat::ASTC8x8:
        case TextureFormat::ASTC10x10:
        case TextureFormat::ASTC12x12:
            return 16;
        case TextureFormat::ETC2RGB8:
        case TextureFormat::ETC2RGB8PunchthroughAlpha1:
            return 8;
        case TextureFormat::ETC2RGBA8:
            return 16;
        default:
            return 0;
    }
}

GLenum GLESCTextureManager::glDataFormat(GLenum internal_format) {
    switch (internal_format) {
        case GL_R8:
        case GL_R16F:
        case GL_R32F:
            return GL_RED;
        case GL_RG8:
        case GL_RG16F:
        case GL_RG32F:
            return GL_RG;
        case GL_DEPTH_COMPONENT16:
        case GL_DEPTH_COMPONENT24:
        case GL_DEPTH_COMPONENT32F:
            return GL_DEPTH_COMPONENT;
        case GL_DEPTH24_STENCIL8:
        case GL_DEPTH32F_STENCIL8:
            return GL_DEPTH_STENCIL;
        case GL_RGBA8:
        case GL_SRGB8_ALPHA8:
        case GL_RGBA16F:
        case GL_RGBA32F:
        default:
            return GL_RGBA;
    }
}

GLenum GLESCTextureManager::glDataType(GLenum internal_format) {
    switch (internal_format) {
        case GL_RGBA8:
        case GL_SRGB8_ALPHA8:
            return GL_UNSIGNED_BYTE;
        case GL_DEPTH_COMPONENT16:
            return GL_UNSIGNED_SHORT;
        case GL_R16F:
        case GL_RG16F:
        case GL_RGBA16F:
            return GL_HALF_FLOAT;
        case GL_R32F:
        case GL_RG32F:
        case GL_RGBA32F:
        case GL_DEPTH_COMPONENT32F:
            return GL_FLOAT;
        case GL_DEPTH_COMPONENT24:
        case GL_DEPTH24_STENCIL8:
            return GL_UNSIGNED_INT;
        case GL_DEPTH32F_STENCIL8:
            return GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
        default:
            return GL_UNSIGNED_BYTE;
    }
}

uint32_t GLESCTextureManager::mipChainLength(uint32_t width, uint32_t height, uint32_t depth) {
    uint32_t largest = std::max({width, height, depth});
    uint32_t levels = 1;
    while (largest > 1) {
        largest >>= 1;
        levels++;
    }
    return levels;
}

// The version is queried once per context: the surface can be recreated under a
// new context with different capabilities, so the cache is dropped whenever no
// context is available.
bool GLESCTextureManager::glesAtLeast(int major, int minor) {
    if (!contextAvailable()) {
        // The surface can be recreated under a new context with different
        // capabilities, so a cached version must not outlive the context it was
        // queried from.
        gles_version_known_ = false;
        astc_cached_ = false;
        return false;
    }
    if (!gles_version_known_) {
        GLint value = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &value);
        gles_major_ = value;
        glGetIntegerv(GL_MINOR_VERSION, &value);
        gles_minor_ = value;
        gles_version_known_ = true;
        LOGI("GLES %d.%d detected", gles_major_, gles_minor_);
    }
    if (gles_major_ != major) {
        return gles_major_ > major;
    }
    return gles_minor_ >= minor;
}

bool GLESCTextureManager::astcSupported() {
    if (!astc_cached_) {
        // ASTC is core in ES 3.2, an extension before that.
        astc_available_ = glesAtLeast(3, 2) ||
                          (renderer_ != nullptr &&
                           (renderer_->isExtensionSupported("GL_EXT_texture_compression_astc_decode") ||
                            renderer_->isExtensionSupported("GL_KHR_texture_compression_astc_hdr")));
        astc_cached_ = true;
        if (!astc_available_) {
            LOGI("ASTC unavailable: compressed uploads fall back to ETC2");
        }
    }
    return astc_available_;
}

GLenum GLESCTextureManager::resolveInternalFormat(uint32_t format, bool* out_fallback) {
    if (out_fallback != nullptr) {
        *out_fallback = false;
    }
    const TextureFormat requested = static_cast<TextureFormat>(format);
    const bool compressed = isCompressedFormat(format);
    const bool astc = requested >= TextureFormat::ASTC4x4 && requested <= TextureFormat::ASTC12x12;

    if (compressed && !astc) {
        // ETC2 is GLES 3.0 core, so it is decodable everywhere the backend runs.
        return glInternalFormat(format);
    }
    if (compressed && astc && astcSupported()) {
        // Preferred family: the caller's block size is kept as-is.
        return glInternalFormat(format);
    }
    if (compressed) {
        // Tier 2 of the fallback chain: ASTC is unavailable, so the texture is
        // allocated as ETC2, which every ES 3.0 device decodes.
        if (out_fallback != nullptr) {
            *out_fallback = true;
        }
        if (glesAtLeast(3, 0)) {
            LOGW("ASTC format %u unsupported, substituting ETC2", format);
            return GL_COMPRESSED_RGBA8_ETC2_EAC;
        }
        // Tier 3: uncompressed RGBA8. Unreachable for a conformant ES 3.0 device
        // (ETC2 is core) but it keeps the texture usable if the compression query
        // ever disagrees with the specification.
        LOGW("ASTC format %u and ETC2 unsupported, substituting RGBA8", format);
        return GL_RGBA8;
    }

    if (requested == TextureFormat::SRGB8_ALPHA8 && !glesAtLeast(3, 1)) {
        // sRGB sized formats became core in ES 3.1; before that they exist but
        // are not guaranteed to be renderable, so plain RGBA8 keeps the texture
        // usable (linear values).
        if (out_fallback != nullptr) {
            *out_fallback = true;
        }
        return GL_RGBA8;
    }
    if (requested == TextureFormat::Undefined) {
        if (out_fallback != nullptr) {
            *out_fallback = true;
        }
        return GL_RGBA8;
    }
    return glInternalFormat(format);
}

void GLESCTextureManager::levelExtent(const TextureObject& object, uint32_t mip_level, uint32_t* out_width,
                                      uint32_t* out_height, uint32_t* out_depth) const {
    if (mip_level >= object.levels) {
        *out_width = 0;
        *out_height = 0;
        *out_depth = 0;
        return;
    }
    *out_width = std::max<uint32_t>(1, object.width >> mip_level);
    *out_height = std::max<uint32_t>(1, object.height >> mip_level);
    *out_depth = object.target == GL_TEXTURE_3D ? std::max<uint32_t>(1, object.depth >> mip_level) : 1;
}

uint64_t GLESCTextureManager::regionBytes(uint32_t format, GLenum internal_format, uint32_t width, uint32_t height,
                                          uint32_t depth) const {
    if (width == 0 || height == 0 || depth == 0) {
        return 0;
    }
    if (isCompressedFormat(format)) {
        uint32_t block_width = 0;
        uint32_t block_height = 0;
        blockExtent(format, &block_width, &block_height);
        const uint32_t block_bytes = compressedBlockSize(format);
        if (block_bytes == 0) {
            return 0;
        }
        const uint64_t blocks_x = (static_cast<uint64_t>(width) + block_width - 1) / block_width;
        const uint64_t blocks_y = (static_cast<uint64_t>(height) + block_height - 1) / block_height;
        return blocks_x * blocks_y * block_bytes * depth;
    }
    const uint32_t texel = bytesPerTexel(internal_format);
    if (texel == 0) {
        return 0;
    }
    return static_cast<uint64_t>(width) * height * depth * texel;
}

bool GLESCTextureManager::allocateStorage(GLenum target, uint32_t levels, GLenum internal_format, uint32_t width,
                                          uint32_t height, uint32_t depth_or_layers) {
    if (levels == 0 || width == 0 || height == 0 || depth_or_layers == 0) {
        return false;
    }
    switch (target) {
        case GL_TEXTURE_2D:
            glTexStorage2D(target, static_cast<GLsizei>(levels), internal_format, static_cast<GLsizei>(width),
                           static_cast<GLsizei>(height));
            break;
        case GL_TEXTURE_3D:
            glTexStorage3D(target, static_cast<GLsizei>(levels), internal_format, static_cast<GLsizei>(width),
                           static_cast<GLsizei>(height), static_cast<GLsizei>(depth_or_layers));
            break;
        case GL_TEXTURE_2D_ARRAY:
            glTexStorage3D(target, static_cast<GLsizei>(levels), internal_format, static_cast<GLsizei>(width),
                           static_cast<GLsizei>(height), static_cast<GLsizei>(depth_or_layers));
            break;
        case GL_TEXTURE_CUBE_MAP:
            // ES restricts glTexStorage2D on a cube target to a single level, so
            // the chain is allocated face by face with glTexImage2D. Mutable
            // storage is what later allows glTexSubImage2D on the face targets.
            for (uint32_t level = 0; level < levels; ++level) {
                const uint32_t level_width = std::max<uint32_t>(1, width >> level);
                const uint32_t level_height = std::max<uint32_t>(1, height >> level);
                for (uint32_t face = 0; face < kTextureCubeFaceCount; ++face) {
                    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, static_cast<GLint>(level), internal_format,
                                 static_cast<GLsizei>(level_width), static_cast<GLsizei>(level_height), 0,
                                 glDataFormat(internal_format), glDataType(internal_format), nullptr);
                }
            }
            break;
        default:
            return false;
    }
    return glGetError() == GL_NO_ERROR;
}

void GLESCTextureManager::configureParameters(GLenum target, uint32_t levels, bool is_cube, bool filterable) {
    // A texture with a mip filter but only one level is incomplete and samples
    // as black, so the min filter always follows the allocated level count.
    if (filterable) {
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    } else {
        // Depth formats and non linear float formats are not texture filterable.
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    // Cube sampling is defined for seamless filtering inside a face, never across
    // faces, so cube faces clamp and everything else repeats.
    const GLint wrap = is_cube ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    glTexParameteri(target, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, wrap);
    if (target == GL_TEXTURE_3D) {
        glTexParameteri(target, GL_TEXTURE_WRAP_R, wrap);
    }
    // GL_TEXTURE_MAX_LEVEL is left alone: immutable storage already fixes the
    // level count, and cube faces allocated with glTexImage2D report it implicitly.
}

bool GLESCTextureManager::createTextureObject(uint64_t handle, GLenum target, uint32_t width, uint32_t height,
                                              uint32_t depth, uint32_t layers, uint32_t format, uint32_t usage,
                                              uint32_t mip_levels) {
    if (!contextAvailable() || width == 0 || height == 0 || depth == 0 || layers == 0) {
        return false;
    }
    // ES derives renderability from the internal format itself; what a texture is
    // bound to is the command manager's business, so usage does not change the
    // allocation here.
    (void)usage;

    const uint32_t chain = mipChainLength(width, height, depth);
    uint32_t levels = (mip_levels == 0 || mip_levels > chain) ? chain : mip_levels;

    bool fallback = false;
    const GLenum internal_format = resolveInternalFormat(format, &fallback);
    if (isDepthFormat(internal_format)) {
        // Depth formats cannot be mipmapped in ES; a depth attachment is always
        // sampled at one level anyway.
        levels = 1;
    }

    const uint32_t depth_or_layers = (target == GL_TEXTURE_3D) ? depth : layers;

    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture == 0) {
        LOGE("glGenTextures failed for texture %llu", (unsigned long long)handle);
        return false;
    }

    const GLuint previous = boundTexture(target);
    glBindTexture(target, texture);
    const bool allocated = allocateStorage(target, levels, internal_format, width, height, depth_or_layers);
    if (allocated) {
        // Linear filtering of float formats needs OES_texture_float_linear;
        // everything else here (RGBA8, ETC2, ASTC) is filterable in ES 3.0.
        const bool filterable = !isDepthFormat(internal_format) &&
                                (!isFloatFormat(internal_format) ||
                                 (renderer_ != nullptr && renderer_->isExtensionSupported("GL_OES_texture_float_linear")));
        configureParameters(target, levels, target == GL_TEXTURE_CUBE_MAP, filterable);
    }
    glBindTexture(target, previous);

    if (!allocated || glGetError() != GL_NO_ERROR) {
        LOGE("storage allocation failed for texture %llu (%ux%u)", (unsigned long long)handle, width, height);
        glDeleteTextures(1, &texture);
        return false;
    }

    TextureObject object;
    object.texture = texture;
    object.target = target;
    object.internal_format = internal_format;
    object.width = width;
    object.height = height;
    object.depth = depth;
    object.layers = layers;
    object.levels = levels;
    object.format = format;
    object.is_cube = target == GL_TEXTURE_CUBE_MAP;
    object.format_fallback = fallback;
    textures_[handle] = std::move(object);
    return true;
}

void GLESCTextureManager::uploadRegion(const TextureObject& object, uint32_t mip_level, uint32_t array_layer, uint32_t x,
                                       uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth,
                                       const void* data, uint64_t data_size) {
    const bool compressed = isCompressedFormat(object.format);
    const bool is_3d = object.target == GL_TEXTURE_3D;
    // Cube faces are addressed as array layers on the cube target; GL uses the
    // array layer as the depth slice for arrays and as the face offset for cubes.
    const GLenum target = object.is_cube ? static_cast<GLenum>(GL_TEXTURE_CUBE_MAP_POSITIVE_X + array_layer)
                                         : object.target;
    const GLint slice = is_3d ? static_cast<GLint>(z) : static_cast<GLint>(array_layer);
    const GLsizei slice_depth = is_3d ? static_cast<GLsizei>(depth) : 1;

    GLint previous_alignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_alignment);
    // Rows are tightly packed here: with the default alignment of 4 GL pads (and
    // rejects) rows whose size is not a multiple of 4, which is the normal case
    // for R8, RGB8 and for compressed block rows. The previous value is restored
    // so unrelated uploads keep their own packing.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const GLuint previous = boundTexture(target);
    glBindTexture(target, object.texture);
    if (compressed) {
        // GL takes the compressed payload size as a 32 bit GLsizei; onUpdateTexture
        // has already verified that the caller supplies at least a full block set.
        const GLsizei image_size = static_cast<GLsizei>(std::min<uint64_t>(data_size, 0x7FFFFFFFull));
        const GLenum format = object.internal_format;
        if (is_3d || object.target == GL_TEXTURE_2D_ARRAY) {
            glCompressedTexSubImage3D(target, static_cast<GLint>(mip_level), static_cast<GLint>(x),
                                      static_cast<GLint>(y), slice, static_cast<GLsizei>(width),
                                      static_cast<GLsizei>(height), slice_depth, format, image_size, data);
        } else {
            glCompressedTexSubImage2D(target, static_cast<GLint>(mip_level), static_cast<GLint>(x),
                                      static_cast<GLint>(y), static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                                      format, image_size, data);
        }
    } else {
        const GLenum format = glDataFormat(object.internal_format);
        const GLenum type = glDataType(object.internal_format);
        if (is_3d || object.target == GL_TEXTURE_2D_ARRAY) {
            glTexSubImage3D(target, static_cast<GLint>(mip_level), static_cast<GLint>(x), static_cast<GLint>(y), slice,
                            static_cast<GLsizei>(width), static_cast<GLsizei>(height), slice_depth, format, type,
                            data);
        } else {
            glTexSubImage2D(target, static_cast<GLint>(mip_level), static_cast<GLint>(x), static_cast<GLint>(y),
                            static_cast<GLsizei>(width), static_cast<GLsizei>(height), format, type, data);
        }
    }
    glBindTexture(target, previous);

    glPixelStorei(GL_UNPACK_ALIGNMENT, previous_alignment);
}

bool GLESCTextureManager::onCreateTexture2D(uint64_t handle, uint32_t width, uint32_t height, uint32_t format,
                                            uint32_t usage, uint32_t mip_levels) {
    std::lock_guard<std::mutex> lock(mutex_);
    return createTextureObject(handle, GL_TEXTURE_2D, width, height, 1, 1, format, usage, mip_levels);
}

bool GLESCTextureManager::onCreateTexture3D(uint64_t handle, uint32_t width, uint32_t height, uint32_t depth,
                                            uint32_t format, uint32_t usage, uint32_t mip_levels) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (depth == 0) {
        return false;
    }
    return createTextureObject(handle, GL_TEXTURE_3D, width, height, depth, 1, format, usage, mip_levels);
}

bool GLESCTextureManager::onCreateTextureArray(uint64_t handle, uint32_t width, uint32_t height, uint32_t array_layers,
                                               uint32_t format, uint32_t usage, uint32_t mip_levels) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (array_layers == 0) {
        return false;
    }
    // TextureManager has no cube hook: createTextureCube() arrives here with six
    // layers, and a six layer array must be bound as a cube map so face selection
    // and clamp-to-edge sampling work.
    if (array_layers == kTextureCubeFaceCount) {
        return onCreateTextureCube(handle, width, height, format, usage, mip_levels);
    }
    return createTextureObject(handle, GL_TEXTURE_2D_ARRAY, width, height, 1, array_layers, format, usage, mip_levels);
}

bool GLESCTextureManager::onCreateTextureCube(uint64_t handle, uint32_t width, uint32_t height, uint32_t format,
                                              uint32_t usage, uint32_t mip_levels) {
    std::lock_guard<std::mutex> lock(mutex_);
    return createTextureObject(handle, GL_TEXTURE_CUBE_MAP, width, height, 1, kTextureCubeFaceCount, format, usage,
                               mip_levels);
}

void GLESCTextureManager::onDestroyTexture(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle);
    if (it == textures_.end()) {
        return;
    }
    if (it->second.texture != 0 && contextAvailable()) {
        glDeleteTextures(1, &it->second.texture);
    }
    textures_.erase(it);
}

void GLESCTextureManager::onUpdateTexture(uint64_t handle, uint32_t mip_level, uint32_t array_layer, uint32_t x,
                                          uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth,
                                          const void* data, uint64_t data_size) {
    if (data == nullptr || width == 0 || height == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle);
    if (it == textures_.end()) {
        return;
    }
    const TextureObject& object = it->second;
    if (object.texture == 0 || !contextAvailable()) {
        return;
    }

    uint32_t level_width = 0;
    uint32_t level_height = 0;
    uint32_t level_depth = 0;
    levelExtent(object, mip_level, &level_width, &level_height, &level_depth);
    if (level_width == 0) {
        LOGW("update of texture %llu targets mip %u of %u", (unsigned long long)handle, mip_level, object.levels);
        return;
    }
    if (static_cast<uint64_t>(x) + width > level_width || static_cast<uint64_t>(y) + height > level_height ||
        static_cast<uint64_t>(z) + depth > level_depth) {
        LOGW("update of texture %llu outside mip %u", (unsigned long long)handle, mip_level);
        return;
    }
    if (object.is_cube && array_layer >= kTextureCubeFaceCount) {
        LOGW("update of cube texture %llu targets layer %u", (unsigned long long)handle, array_layer);
        return;
    }
    if (object.target == GL_TEXTURE_2D_ARRAY && array_layer >= object.layers) {
        LOGW("update of texture %llu targets layer %u of %u", (unsigned long long)handle, array_layer, object.layers);
        return;
    }
    if (object.format_fallback && isCompressedFormat(object.format)) {
        // The device cannot decode the requested block format, so the payload's
        // block layout no longer matches the allocation: pushing it would hand GL
        // compressed data it must reinterpret. The texture stays usable, e.g. for
        // a later update in RGBA8 or for rendering into.
        LOGW("compressed update of texture %llu skipped: format was substituted", (unsigned long long)handle);
        return;
    }

    const uint32_t slice_depth = (object.target == GL_TEXTURE_3D) ? depth : 1;
    const uint64_t required = regionBytes(object.format, object.internal_format, width, height, slice_depth);
    if (required == 0 || data_size < required) {
        LOGW("update of texture %llu carries %llu bytes, needs %llu", (unsigned long long)handle,
             (unsigned long long)data_size, (unsigned long long)required);
        return;
    }

    uploadRegion(object, mip_level, array_layer, x, y, z, width, height, depth, data, data_size);
}

void GLESCTextureManager::copyLayerOnCpu(const TextureObject& src, const TextureObject& dst, uint32_t src_mip,
                                         uint32_t dst_mip, uint32_t src_layer, uint32_t dst_layer, uint32_t width,
                                         uint32_t height, uint32_t depth) {
    // Cube faces and array layers index a target range, so an out of range value
    // has to be rejected before it reaches glBindTexture.
    if ((src.is_cube && src_layer >= kTextureCubeFaceCount) ||
        (dst.is_cube && dst_layer >= kTextureCubeFaceCount) ||
        (src.target == GL_TEXTURE_2D_ARRAY && src_layer >= src.layers) ||
        (dst.target == GL_TEXTURE_2D_ARRAY && dst_layer >= dst.layers)) {
        LOGW("cpu layer copy rejected: layer out of range");
        return;
    }

    const uint64_t bytes = regionBytes(src.format, src.internal_format, width, height, depth);
    if (bytes == 0 || bytes > kMaxCpuCopyBytes) {
        LOGW("cpu layer copy skipped: %llu bytes", (unsigned long long)bytes);
        return;
    }

    std::vector<uint8_t> scratch(static_cast<size_t>(bytes));

    const GLenum read_target = src.is_cube ? static_cast<GLenum>(GL_TEXTURE_CUBE_MAP_POSITIVE_X + src_layer)
                                           : src.target;
    GLint previous_pack = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack);
    // Same reasoning as on upload: rows of an uncompressed level are tightly
    // packed, so the default 4 byte alignment would skew the read back.
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    const GLuint previous = boundTexture(read_target);
    glBindTexture(read_target, src.texture);
    // glGetTexImage needs an explicit extent and border; the scratch buffer was
    // sized for exactly this level, and a non-zero border would read past it.
    glGetTexImage(read_target, static_cast<GLint>(src_mip), glDataFormat(src.internal_format),
                  glDataType(src.internal_format), static_cast<GLsizei>(width),
                  static_cast<GLsizei>(height), 0, scratch.data());
    glBindTexture(read_target, previous);
    glPixelStorei(GL_PACK_ALIGNMENT, previous_pack);

    uploadRegion(dst, dst_mip, dst_layer, 0, 0, 0, width, height, depth, scratch.data(), bytes);
}

void GLESCTextureManager::onCopyTexture(uint64_t src, uint64_t dst, uint32_t src_mip, uint32_t dst_mip,
                                        uint32_t src_layer, uint32_t dst_layer) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto src_it = textures_.find(src);
    auto dst_it = textures_.find(dst);
    if (src_it == textures_.end() || dst_it == textures_.end()) {
        return;
    }
    const TextureObject& source = src_it->second;
    const TextureObject& destination = dst_it->second;
    if (source.texture == 0 || destination.texture == 0 || !contextAvailable()) {
        return;
    }
    if (source.internal_format != destination.internal_format) {
        // glCopyImageSubData requires compatible formats and re-encoding would
        // need a shader, which is out of scope for the manager.
        LOGW("copy between differently formatted textures is not supported");
        return;
    }

    uint32_t src_width = 0;
    uint32_t src_height = 0;
    uint32_t src_depth = 0;
    levelExtent(source, src_mip, &src_width, &src_height, &src_depth);
    uint32_t dst_width = 0;
    uint32_t dst_height = 0;
    uint32_t dst_depth = 0;
    levelExtent(destination, dst_mip, &dst_width, &dst_height, &dst_depth);
    if (src_width == 0 || dst_width == 0) {
        return;
    }

    const uint32_t width = std::min(src_width, dst_width);
    const uint32_t height = std::min(src_height, dst_height);
    const bool both_3d = source.target == GL_TEXTURE_3D && destination.target == GL_TEXTURE_3D;
    const uint32_t depth = both_3d ? std::min(src_depth, dst_depth) : 1;

    // glCopyImageSubData (GLES 3.1 core) copies a whole level at the origin. A
    // plain 2D or 3D level is unambiguous; an array layer or cube face would have
    // to be addressed through the z coordinate, which ES only defines for 3D
    // images, so those copies go through the CPU round trip below where the layer
    // is explicit.
    const bool plain_level = source.target == destination.target &&
                             (source.target == GL_TEXTURE_2D || both_3d);
    // The overload the NDK header declares takes 15 arguments: it carries the
    // source and destination formats and types explicitly, because a copy can
    // reinterpret the data (for example UNPACK_ROW_LENGTH-style layout). The
    // formats must match, which the identical-format check above already
    // established, so both sides are given the same values.
    if (plain_level && src_layer == dst_layer && glesAtLeast(3, 1) &&
        source.internal_format == destination.internal_format) {
        glCopyImageSubData(source.texture, static_cast<GLenum>(src_mip), 0, 0, 0,
                           static_cast<GLenum>(source.internal_format),
                           static_cast<GLenum>(glDataType(source.internal_format)),
                           destination.texture, static_cast<GLenum>(dst_mip), 0, 0, 0,
                           static_cast<GLenum>(destination.internal_format),
                           static_cast<GLenum>(glDataType(destination.internal_format)),
                           static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                           static_cast<GLsizei>(depth));
        return;
    }

    if (isCompressedFormat(source.format)) {
        // Reading a block compressed level back is not portable in ES, so a layer
        // copy of a compressed texture is skipped rather than feeding GL a
        // payload it cannot re-interpret.
        LOGW("compressed layer copy is not supported");
        return;
    }
    copyLayerOnCpu(source, destination, src_mip, dst_mip, src_layer, dst_layer, width, height, depth);
}

void GLESCTextureManager::onGenerateMipmaps(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle);
    if (it == textures_.end()) {
        return;
    }
    const TextureObject& object = it->second;
    if (object.texture == 0 || object.levels <= 1 || !contextAvailable()) {
        return;
    }
    if (isDepthFormat(object.internal_format)) {
        // Depth formats are not colour renderable, which glGenerateMipmap needs
        // to render the upper levels.
        LOGW("mipmap generation skipped for a depth texture");
        return;
    }
    if (isFloatFormat(object.internal_format) &&
        (renderer_ == nullptr || !renderer_->isExtensionSupported("GL_EXT_color_buffer_float"))) {
        // Generating mips renders into the top level, so float formats need a
        // float colour buffer.
        LOGW("mipmap generation skipped for a float texture without EXT_color_buffer_float");
        return;
    }

    const GLuint previous = boundTexture(object.target);
    glBindTexture(object.target, object.texture);
    glGenerateMipmap(object.target);
    glBindTexture(object.target, previous);
}

void GLESCTextureManager::setPendingUploadExtent(uint32_t width, uint32_t height) {
    pending_width_ = width;
    pending_height_ = height;
}

uint64_t GLESCTextureManager::createTextureFromMemory(uint32_t width, uint32_t height, uint32_t format,
                                                      const void* pixels, uint64_t size, bool generate_mipmaps) {
    if (width == 0 || height == 0 || pixels == nullptr) {
        return 0;
    }
    // loadTextureFromMemory() has no extent parameter, so the extent is declared
    // first and the same upload path is used for both entry points.
    setPendingUploadExtent(width, height);
    return loadTextureFromMemory(pixels, size, format, generate_mipmaps);
}

bool GLESCTextureManager::onLoadTextureFromMemory(uint64_t handle, const void* data, uint64_t size, uint32_t format,
                                                  bool generate_mipmaps) {
    std::lock_guard<std::mutex> lock(mutex_);

    const uint32_t width = pending_width_;
    const uint32_t height = pending_height_;
    // Consumed eagerly so a stale extent can never be applied to a later upload.
    pending_width_ = 0;
    pending_height_ = 0;

    if (data == nullptr || size == 0 || !contextAvailable()) {
        return false;
    }
    if (width == 0 || height == 0) {
        // The base API carries no extent and no image decoder is linked into the
        // native library, so this cannot be recovered from the payload.
        LOGW("loadTextureFromMemory() needs an extent: call setPendingUploadExtent() first");
        return false;
    }

    // The payload is consumed as a single top level. Validating the size before
    // creating anything keeps a short payload from being read past its end.
    const uint64_t required = regionBytes(format, glInternalFormat(format), width, height, 1);
    if (required == 0 || size < required) {
        LOGW("memory upload carries %llu bytes, needs %llu", (unsigned long long)size,
             (unsigned long long)required);
        return false;
    }

    // Levels are either filled by the caller or by glGenerateMipmap below.
    const uint32_t levels = generate_mipmaps ? mipChainLength(width, height, 1) : 1;
    if (!createTextureObject(handle, GL_TEXTURE_2D, width, height, 1, 1, format, static_cast<uint32_t>(TextureUsage::Sampler),
                             levels)) {
        return false;
    }

    auto it = textures_.find(handle);
    if (it == textures_.end()) {
        return false;
    }
    uploadRegion(it->second, 0, 0, 0, 0, 0, width, height, 1, data, size);

    if (generate_mipmaps && levels > 1) {
        // Generated here instead of through the base generateMipmaps(): the base
        // record for a memory load only knows one level.
        const TextureObject& object = it->second;
        if (!isDepthFormat(object.internal_format)) {
            const GLuint previous = boundTexture(object.target);
            glBindTexture(object.target, object.texture);
            glGenerateMipmap(object.target);
            glBindTexture(object.target, previous);
        }
    }
    return true;
}

void GLESCTextureManager::onSetTextureDebugName(uint64_t handle, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle);
    if (it == textures_.end()) {
        return;
    }
    it->second.debug_name = name;

    // GLES has no core object naming. GL_KHR_debug adds one, and since it is an
    // extension the entry point has to be resolved through EGL. Resolved per call
    // on purpose: naming is a rare debug-only path and the extension set can
    // change when the surface (and with it the context) is recreated.
    PFN_glObjectLabel label = resolveObjectLabel(renderer_);
    if (label != nullptr && it->second.texture != 0) {
        label(GL_TEXTURE, it->second.texture, static_cast<GLsizei>(name.size()), name.c_str());
    }
}

void GLESCTextureManager::destroyAllLocked() {
    if (textures_.empty()) {
        return;
    }
    if (contextAvailable()) {
        for (auto& entry : textures_) {
            TextureObject& object = entry.second;
            if (object.texture != 0) {
                glDeleteTextures(1, &object.texture);
                object.texture = 0;
            }
        }
    } else {
        // Without a context the names cannot be deleted; they belong to the
        // context, which frees them when it is destroyed.
        LOGW("%zu texture(s) left for context teardown", textures_.size());
    }
    textures_.clear();
}

} // namespace copper