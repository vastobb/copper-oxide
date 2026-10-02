#pragma once

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>

#include "texture_manager.h"

namespace copper {

class VulkanRenderer;

// Backend-agnostic image usage bits; TextureManager passes them through
// verbatim, so the Vulkan backend owns the translation.
namespace TextureUsage {
inline constexpr uint32_t NONE                   = 0u;
inline constexpr uint32_t SAMPLED                = 1u << 0;
inline constexpr uint32_t STORAGE                = 1u << 1;
inline constexpr uint32_t COLOR_ATTACHMENT       = 1u << 2;
inline constexpr uint32_t DEPTH_STENCIL_ATTACHMENT = 1u << 3;
inline constexpr uint32_t TRANSFER_SRC           = 1u << 4;
inline constexpr uint32_t TRANSFER_DST           = 1u << 5;
inline constexpr uint32_t INPUT_ATTACHMENT       = 1u << 6;
}

// Backend-agnostic pixel formats. The values are deliberately NOT VkFormat
// values: the common layer stores them as plain uint32_t, and keeping our own
// numbering means a driver-specific fallback (see resolveFormat()) stays visible
// to the caller instead of silently looking like the request.
namespace TextureFormat {
inline constexpr uint32_t NONE = 0u;

// Uncompressed
inline constexpr uint32_t R8_UNORM     = 1u;
inline constexpr uint32_t R8G8_UNORM   = 2u;
inline constexpr uint32_t R8G8B8_UNORM = 3u;
inline constexpr uint32_t R8G8B8A8_UNORM = 4u;
inline constexpr uint32_t R8G8B8A8_SRGB  = 5u;
inline constexpr uint32_t B8G8R8A8_UNORM = 6u;
inline constexpr uint32_t B8G8R8A8_SRGB  = 7u;
inline constexpr uint32_t R16_UNORM      = 8u;
inline constexpr uint32_t R16G16B16_UNORM = 9u;
inline constexpr uint32_t R16G16B16A16_UNORM = 10u;
inline constexpr uint32_t R16_SFLOAT     = 11u;
inline constexpr uint32_t R32_SFLOAT     = 12u;
inline constexpr uint32_t R16G16_SFLOAT  = 13u;
inline constexpr uint32_t R32G32_SFLOAT  = 14u;
inline constexpr uint32_t R16G16B16A16_SFLOAT = 15u;
inline constexpr uint32_t R32G32B32A32_SFLOAT = 16u;
inline constexpr uint32_t A2B10G10R10_UNORM_PACK32 = 17u;

// Depth / stencil
inline constexpr uint32_t D16_UNORM        = 20u;
inline constexpr uint32_t D24_UNORM_S8_UINT = 21u;
inline constexpr uint32_t D32_SFLOAT      = 22u;

// Block compressed (BC). Desktop GPUs; rarely exposed on Android.
inline constexpr uint32_t BC1_RGB_UNORM_BLOCK  = 30u;
inline constexpr uint32_t BC1_RGBA_UNORM_BLOCK = 31u;  // BC1 with alpha
inline constexpr uint32_t BC2_UNORM_BLOCK      = 32u;
inline constexpr uint32_t BC3_UNORM_BLOCK      = 33u;
inline constexpr uint32_t BC4_UNORM_BLOCK      = 34u;
inline constexpr uint32_t BC5_UNORM_BLOCK      = 35u;
inline constexpr uint32_t BC6H_UFLOAT_BLOCK    = 36u;
inline constexpr uint32_t BC7_UNORM_BLOCK      = 37u;

// Block compressed (ETC2 / EAC). Universally available on Android: guaranteed
// by the Vulkan spec for every implementation.
inline constexpr uint32_t ETC2_R8G8B8_UNORM_BLOCK   = 40u;
inline constexpr uint32_t ETC2_R8G8B8A1_UNORM_BLOCK = 41u;
inline constexpr uint32_t ETC2_R8G8B8A8_UNORM_BLOCK  = 42u;
inline constexpr uint32_t EAC_R11_UNORM_BLOCK       = 43u;
inline constexpr uint32_t EAC_R11G11_UNORM_BLOCK    = 44u;

// Block compressed (ASTC). The practical high-rate choice on Android hardware.
inline constexpr uint32_t ASTC_4x4_UNORM_BLOCK   = 50u;
inline constexpr uint32_t ASTC_5x4_UNORM_BLOCK   = 51u;
inline constexpr uint32_t ASTC_5x5_UNORM_BLOCK   = 52u;
inline constexpr uint32_t ASTC_6x6_UNORM_BLOCK   = 53u;
inline constexpr uint32_t ASTC_8x8_UNORM_BLOCK   = 54u;
inline constexpr uint32_t ASTC_10x5_UNORM_BLOCK  = 55u;
inline constexpr uint32_t ASTC_10x8_UNORM_BLOCK  = 56u;
inline constexpr uint32_t ASTC_12x12_UNORM_BLOCK = 57u;
} // namespace TextureFormat

// Vulkan implementation of the backend-agnostic texture manager.
//
// Lifetime: VulkanRenderer owns one instance, created in initializeManagers() and
// destroyed in shutdown() before vkDestroyDevice(). As with the buffer manager,
// the on* hooks take this manager's mutex and never call back into a public
// TextureManager method (TextureManager holds its own non-recursive mutex while
// calling the hooks, so that would self-deadlock).
//
// Images are created with VK_IMAGE_TILING_OPTIMAL and are never CPU-mapped:
// uploads go through a host-visible staging buffer and vkCmdCopyBufferToImage.
class VulkanTextureManager : public TextureManager {
public:
    explicit VulkanTextureManager(VulkanRenderer* renderer);
    ~VulkanTextureManager() override;

    // --- format handling -------------------------------------------------
    // Maps a TextureFormat constant to a VkFormat without querying the device.
    static VkFormat toVkFormat(uint32_t format);
    // Maps and then checks capability against
    // vkGetPhysicalDeviceFormatProperties(), falling back to a supported format
    // (an ASTC request on an ETC2-only device becomes ETC2) and logging.
    VkFormat resolveFormat(uint32_t format) const;
    // True when the format has a non 1x1x1 texel block (BC/ETC2/EAC/ASTC).
    static bool isBlockCompressed(uint32_t format);
    // Bytes one mip level of `width` x `height` x `depth` texels occupies with
    // tightly packed, unfiltered data (compressed formats use block math).
    static uint64_t mipByteSize(uint32_t format, uint32_t width, uint32_t height, uint32_t depth);

    // --- backend queries -------------------------------------------------
    VkImage image(uint64_t handle) const;
    VkImageView imageView(uint64_t handle) const;
    VkFormat imageFormat(uint64_t handle) const;
    // Extent of the image, needed to build a VkFramebuffer whose attachments
    // must all agree. Returns false for an unknown handle.
    bool imageExtent(uint64_t handle, uint32_t* width, uint32_t* height,
                     uint32_t* array_layers) const;
    // True when the image's contents may still be touched by a frame that has
    // not finished on the GPU.
    bool isInUseByGpu(uint64_t handle) const;
    // Blocks until every upload / mipmap job submitted here has retired.
    bool waitForUploads(uint64_t timeout_ns = UINT64_MAX);
    // Frees the staging buffer, transient command buffer and fence. Must run
    // before vkDestroyDevice(); the destructor calls it as well.
    void releaseDeviceResources();

    // Overridden because TextureManager::getCacheSizeMb() returns a counter the
    // common layer never increments; this answers from the real VkImage memory.
    size_t getCacheSizeMb() const override;

    // The single layout transition entry point. Every transition this manager
    // performs internally (upload, image copy, mip chain) goes through the same
    // helper inside Impl::transition_image_locked(); this public form exists so
    // the framebuffer/state backends can move an image to
    // COLOR_ATTACHMENT_OPTIMAL or GENERAL and get the matching stages and access
    // masks for free. mip_count / layer_count of 0 mean "all levels/layers".
    //
    // The transition is recorded on this manager's transient command buffer and
    // submitted on the graphics queue, not inside VulkanRenderer::onEndFrame():
    // the frame command buffer is reset and re-recorded from scratch every frame
    // and is only open for recording inside that function. Queue submission
    // order still orders the transition correctly against the frame's submit.
    bool transitionImage(uint64_t handle, VkImageLayout new_layout, uint32_t base_mip = 0,
                         uint32_t mip_count = 0, uint32_t base_layer = 0, uint32_t layer_count = 0);

    // Uploads tightly packed, already-decoded pixels and creates the texture in
    // one step. This is the supported entry point for CPU-side image data:
    // loadTextureFromMemory() receives no dimensions, so it cannot size an
    // image, and container formats (PNG/JPEG/KTX2) need a decoder that lives
    // in the loading thread (stb_image, basisu) rather than here.
    uint64_t uploadRawPixels(const void* pixels, uint64_t size, uint32_t width, uint32_t height,
                             uint32_t format, bool generate_mipmaps,
                             uint32_t array_layers = 1);

    // Overridden because TextureManager::createTextureCube() forwards to
    // createTextureArray(..., 6, ...) and the hook it ends up in cannot tell a
    // cube map from an ordinary 6-layer array, which would leave the image without
    // VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT and the view without
    // VK_IMAGE_VIEW_TYPE_CUBE. The cube bit travels to the hook through a
    // thread_local one-shot flag; the base class still owns the handle.
    uint64_t createTextureCube(uint32_t width, uint32_t height, uint32_t format, uint32_t usage,
                               uint32_t mip_levels = 1) override;

protected:
    bool onCreateTexture2D(uint64_t handle, uint32_t width, uint32_t height, uint32_t format,
                           uint32_t usage, uint32_t mip_levels) override;
    bool onCreateTexture3D(uint64_t handle, uint32_t width, uint32_t height, uint32_t depth,
                           uint32_t format, uint32_t usage, uint32_t mip_levels) override;
    bool onCreateTextureArray(uint64_t handle, uint32_t width, uint32_t height, uint32_t array_layers,
                              uint32_t format, uint32_t usage, uint32_t mip_levels) override;
    void onDestroyTexture(uint64_t handle) override;
    void onUpdateTexture(uint64_t handle, uint32_t mip_level, uint32_t array_layer, uint32_t x,
                         uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth,
                         const void* data, uint64_t data_size) override;
    void onCopyTexture(uint64_t src, uint64_t dst, uint32_t src_mip, uint32_t dst_mip,
                       uint32_t src_layer, uint32_t dst_layer) override;
    void onGenerateMipmaps(uint64_t handle) override;
    bool onLoadTextureFromMemory(uint64_t handle, const void* data, uint64_t size, uint32_t format,
                                 bool generate_mipmaps) override;
    void onSetTextureDebugName(uint64_t handle, const std::string& name) override;

private:
    class Impl;

    // Validates `region` against the image, stages the bytes and records
    // UNDEFINED/old -> TRANSFER_DST_OPTIMAL -> shader-read on the transient
    // command buffer. pImpl->upload_mutex must already be held, which is why the
    // multi-layer and mip-chain paths can keep several regions in one lock hold.
    bool uploadRegion(const Impl::Record& record, uint32_t mip_level, uint32_t array_layer,
                      uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height,
                      uint32_t depth, const void* data, uint64_t data_size);

    std::unique_ptr<Impl> pImpl;
    VulkanRenderer* renderer_ = nullptr;
};

} // namespace copper
