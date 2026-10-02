#include "vulkan_texture_manager.h"
#include "vulkan_renderer.h"

// VMA is compiled into vma_impl.cpp; only its declarations are needed here. The
// allocator does not exist yet, so Impl::allocate_and_bind() is the single seam
// where the manual memory path is swapped for vmaAllocateMemory()/
// vmaBindImageMemory(). It is deliberately the same shape as the buffer manager's
// helper (duplicated rather than shared: this change adds four files, not a
// common header).
#include <vk_mem_alloc.h>

#include <android/log.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <type_traits>
#include <unordered_map>
#include <utility>

#define LOG_TAG "CopperOxide-VKTexture"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

namespace {

// "This image was never handed to the GPU": no in-flight state to track.
constexpr uint64_t kNoFrame = UINT64_MAX;

constexpr VkDeviceSize kMinStagingBytes = 64 * 1024;

// A wedged GPU must not turn the serialized upload fence into an unbounded
// hang; five seconds is far beyond a healthy frame.
constexpr uint64_t kUploadWaitTimeoutNs = 5ull * 1000ull * 1000ull * 1000ull;

constexpr uint64_t kMiB = 1024ull * 1024ull;

// Static description of one TextureFormat constant: the VkFormat it maps to, its
// texel block size (1x1x1 when uncompressed) and the bytes per block.
struct FormatInfo {
    uint32_t constant;
    VkFormat vk_format;
    uint32_t block_w;
    uint32_t block_h;
    uint32_t block_d;
    uint32_t bytes_per_block;
    bool compressed;
    bool depth_stencil;
};

const FormatInfo kFormats[] = {
    // constant,                            vk format,                              bw bh bd, bytes, cmp,  depth
    {TextureFormat::R8_UNORM,               VK_FORMAT_R8_UNORM,                     1,  1,  1,  1, false, false},
    {TextureFormat::R8G8_UNORM,             VK_FORMAT_R8G8_UNORM,                   1,  1,  1,  2, false, false},
    {TextureFormat::R8G8B8_UNORM,           VK_FORMAT_R8G8B8_UNORM,                 1,  1,  1,  3, false, false},
    {TextureFormat::R8G8B8A8_UNORM,         VK_FORMAT_R8G8B8A8_UNORM,               1,  1,  1,  4, false, false},
    {TextureFormat::R8G8B8A8_SRGB,          VK_FORMAT_R8G8B8A8_SRGB,                1,  1,  1,  4, false, false},
    {TextureFormat::B8G8R8A8_UNORM,         VK_FORMAT_B8G8R8A8_UNORM,               1,  1,  1,  4, false, false},
    {TextureFormat::B8G8R8A8_SRGB,          VK_FORMAT_B8G8R8A8_SRGB,                1,  1,  1,  4, false, false},
    {TextureFormat::R16_UNORM,              VK_FORMAT_R16_UNORM,                    1,  1,  1,  2, false, false},
    {TextureFormat::R16G16B16_UNORM,        VK_FORMAT_R16G16B16_UNORM,              1,  1,  1,  6, false, false},
    {TextureFormat::R16G16B16A16_UNORM,     VK_FORMAT_R16G16B16A16_UNORM,           1,  1,  1,  8, false, false},
    {TextureFormat::R16_SFLOAT,             VK_FORMAT_R16_SFLOAT,                   1,  1,  1,  2, false, false},
    {TextureFormat::R32_SFLOAT,             VK_FORMAT_R32_SFLOAT,                   1,  1,  1,  4, false, false},
    {TextureFormat::R16G16_SFLOAT,          VK_FORMAT_R16G16_SFLOAT,                1,  1,  1,  4, false, false},
    {TextureFormat::R32G32_SFLOAT,          VK_FORMAT_R32G32_SFLOAT,                1,  1,  1,  8, false, false},
    {TextureFormat::R16G16B16A16_SFLOAT,    VK_FORMAT_R16G16B16A16_SFLOAT,          1,  1,  1,  8, false, false},
    {TextureFormat::R32G32B32A32_SFLOAT,    VK_FORMAT_R32G32B32A32_SFLOAT,          1,  1,  1, 16, false, false},
    {TextureFormat::A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32,  1,  1,  1,  4, false, false},

    {TextureFormat::D16_UNORM,              VK_FORMAT_D16_UNORM,                   1,  1,  1,  2, false, true},
    {TextureFormat::D24_UNORM_S8_UINT,      VK_FORMAT_D24_UNORM_S8_UINT,           1,  1,  1,  4, false, true},
    {TextureFormat::D32_SFLOAT,             VK_FORMAT_D32_SFLOAT,                   1,  1,  1,  4, false, true},

    // BC: desktop GPUs, rarely exposed on Android.
    {TextureFormat::BC1_RGB_UNORM_BLOCK,    VK_FORMAT_BC1_RGB_UNORM_BLOCK,          4,  4,  1,  8, true, false},
    {TextureFormat::BC1_RGBA_UNORM_BLOCK,   VK_FORMAT_BC1_RGBA_UNORM_BLOCK,         4,  4,  1,  8, true, false},
    {TextureFormat::BC2_UNORM_BLOCK,        VK_FORMAT_BC2_UNORM_BLOCK,              4,  4,  1, 16, true, false},
    {TextureFormat::BC3_UNORM_BLOCK,        VK_FORMAT_BC3_UNORM_BLOCK,              4,  4,  1, 16, true, false},
    {TextureFormat::BC4_UNORM_BLOCK,        VK_FORMAT_BC4_UNORM_BLOCK,              4,  4,  1,  8, true, false},
    {TextureFormat::BC5_UNORM_BLOCK,        VK_FORMAT_BC5_UNORM_BLOCK,              4,  4,  1, 16, true, false},
    {TextureFormat::BC6H_UFLOAT_BLOCK,      VK_FORMAT_BC6H_UFLOAT_BLOCK,            4,  4,  1, 16, true, false},
    {TextureFormat::BC7_UNORM_BLOCK,        VK_FORMAT_BC7_UNORM_BLOCK,              4,  4,  1, 16, true, false},

    // ETC2 / EAC: mandatory in the Vulkan spec, so this is the universal
    // compressed fallback on Android.
    {TextureFormat::ETC2_R8G8B8_UNORM_BLOCK,   VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,   4,  4,  1,  8, true, false},
    {TextureFormat::ETC2_R8G8B8A1_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK, 4,  4,  1,  8, true, false},
    {TextureFormat::ETC2_R8G8B8A8_UNORM_BLOCK,  VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, 4,  4,  1, 16, true, false},
    {TextureFormat::EAC_R11_UNORM_BLOCK,       VK_FORMAT_EAC_R11_UNORM_BLOCK,        4,  4,  1,  8, true, false},
    {TextureFormat::EAC_R11G11_UNORM_BLOCK,    VK_FORMAT_EAC_R11G11_UNORM_BLOCK,     4,  4,  1, 16, true, false},

    // ASTC: the practical high-rate choice on Android hardware.
    {TextureFormat::ASTC_4x4_UNORM_BLOCK,   VK_FORMAT_ASTC_4x4_UNORM_BLOCK,         4,  4,  1, 16, true, false},
    {TextureFormat::ASTC_5x4_UNORM_BLOCK,   VK_FORMAT_ASTC_5x4_UNORM_BLOCK,         5,  4,  1, 16, true, false},
    {TextureFormat::ASTC_5x5_UNORM_BLOCK,   VK_FORMAT_ASTC_5x5_UNORM_BLOCK,         5,  5,  1, 16, true, false},
    {TextureFormat::ASTC_6x6_UNORM_BLOCK,   VK_FORMAT_ASTC_6x6_UNORM_BLOCK,         6,  6,  1, 16, true, false},
    {TextureFormat::ASTC_10x5_UNORM_BLOCK,  VK_FORMAT_ASTC_10x5_UNORM_BLOCK,       10,  5,  1, 16, true, false},
    {TextureFormat::ASTC_10x8_UNORM_BLOCK,  VK_FORMAT_ASTC_10x8_UNORM_BLOCK,       10,  8,  1, 16, true, false},
    {TextureFormat::ASTC_8x8_UNORM_BLOCK,   VK_FORMAT_ASTC_8x8_UNORM_BLOCK,         8,  8,  1, 16, true, false},
    {TextureFormat::ASTC_12x12_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_UNORM_BLOCK,      12, 12,  1, 16, true, false},
};

const FormatInfo& unknown_format() {
    static const FormatInfo unknown{TextureFormat::NONE, VK_FORMAT_UNDEFINED, 1, 1, 1, 4, false, false};
    return unknown;
}

const FormatInfo* find_format(uint32_t constant) {
    for (const FormatInfo& info : kFormats) {
        if (info.constant == constant) {
            return &info;
        }
    }
    return nullptr;
}

// Reverse lookup: a resolved VkFormat may have come from the fallback chain, so
// every later size/aspect decision has to be based on the format actually in use
// rather than on what the caller asked for.
const FormatInfo& format_info_for(VkFormat format) {
    for (const FormatInfo& info : kFormats) {
        if (info.vk_format == format) {
            return info;
        }
    }
    return unknown_format();
}

// Features the resolved format must offer: this manager only ever creates
// samplable, uploadable images.
VkFormatFeatureFlags required_features(const FormatInfo& info) {
    if (info.compressed) {
        return VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
               VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_OPTIMAL_TILING_BIT;
    }
    return VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
}

// Ordered safety nets: a texture that can actually be uploaded beats the caller's
// preference. ETC2 is in the compressed list because the spec guarantees it
// everywhere, ASTC because it is what Android hardware actually prefers.
const VkFormat kColorFallbacks[] = {
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM,
    VK_FORMAT_R8G8B8_UNORM,   VK_FORMAT_R8G8_UNORM,    VK_FORMAT_R8_UNORM,
};
const VkFormat kDepthFallbacks[] = {
    VK_FORMAT_D32_SFLOAT,
    VK_FORMAT_D24_UNORM_S8_UINT,
    VK_FORMAT_D16_UNORM,
};
const VkFormat kCompressedFallbacks[] = {
    VK_FORMAT_ASTC_6x6_UNORM_BLOCK,
    VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,
    VK_FORMAT_BC7_UNORM_BLOCK,
};

const VkFormat* fallback_list(const FormatInfo& info, size_t& count) {
    if (info.compressed) {
        count = sizeof(kCompressedFallbacks) / sizeof(VkFormat);
        return kCompressedFallbacks;
    }
    if (info.depth_stencil) {
        count = sizeof(kDepthFallbacks) / sizeof(VkFormat);
        return kDepthFallbacks;
    }
    count = sizeof(kColorFallbacks) / sizeof(VkFormat);
    return kColorFallbacks;
}

// Capability-checked format mapping. Shared by the public resolveFormat() and by
// image creation, which needs the same answer the caller would have got.
VkFormat resolve_format_for(VulkanRenderer* renderer, uint32_t constant) {
    const FormatInfo* info = find_format(constant);
    if (info == nullptr) {
        LOGE("unknown TextureFormat constant %u", constant);
        return VK_FORMAT_UNDEFINED;
    }
    const VkPhysicalDevice physical =
        renderer != nullptr ? renderer->physicalDevice() : VK_NULL_HANDLE;
    if (physical == VK_NULL_HANDLE) {
        // Nothing to interrogate (early init or teardown): answer the mapping.
        return info->vk_format;
    }
    const VkFormatFeatureFlags required = required_features(*info);
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physical, info->vk_format, &props);
    if ((props.optimalTilingFeatures & required) == required) {
        return info->vk_format;
    }

    size_t count = 0;
    const VkFormat* fallbacks = fallback_list(*info, count);
    for (size_t i = 0; i < count; ++i) {
        vkGetPhysicalDeviceFormatProperties(physical, fallbacks[i], &props);
        if ((props.optimalTilingFeatures & required) == required) {
            LOGW("format %u is not usable on this GPU; falling back to VkFormat %d", constant,
                 static_cast<int>(fallbacks[i]));
            return fallbacks[i];
        }
    }
    LOGE("no usable format for constant %u; using the first candidate %d", constant,
         static_cast<int>(fallbacks[0]));
    return fallbacks[0];
}

VkImageAspectFlags aspect_for(const FormatInfo& info) {
    return info.depth_stencil ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

// Layout an image is left in after an upload, so the next frame can sample it.
VkImageLayout shader_read_layout(const FormatInfo& info) {
    return info.depth_stencil ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                              : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// 1 + floor(log2(size)), i.e. the length of a full mip chain.
uint32_t full_mip_levels(uint32_t size) {
    uint32_t levels = 1;
    size = size > 0 ? size : 1;
    while (size > 1) {
        size >>= 1;
        ++levels;
    }
    return levels;
}

// Mip extent: each level halves until 1 and never below one texel block. This is
// what is valid as a vkCmdCopyBufferToImage imageExtent, i.e. already block
// aligned for compressed formats.
VkExtent3D compute_mip_extent(uint32_t width, uint32_t height, uint32_t depth, uint32_t mip_level,
                               const FormatInfo& info) {
    return VkExtent3D{std::max(width >> mip_level, info.block_w),
                      std::max(height >> mip_level, info.block_h),
                      std::max(depth >> mip_level, info.block_d)};
}

// Bytes a tightly packed region of this format occupies. Block formats ceil the
// block count, because a sub-region only has to be block aligned.
uint64_t byte_size_for(const FormatInfo& info, uint32_t width, uint32_t height, uint32_t depth) {
    if (width == 0 || height == 0) {
        return 0;
    }
    const uint32_t d = depth == 0 ? 1 : depth;
    if (!info.compressed) {
        return static_cast<uint64_t>(width) * height * d * info.bytes_per_block;
    }
    const uint64_t blocks_x = (width + info.block_w - 1) / info.block_w;
    const uint64_t blocks_y = (height + info.block_h - 1) / info.block_h;
    const uint64_t blocks_z = (d + info.block_d - 1) / info.block_d;
    return blocks_x * blocks_y * blocks_z * info.bytes_per_block;
}

VkImageViewType view_type_for(VkImageType type, bool cube) {
    if (type == VK_IMAGE_TYPE_3D) {
        return VK_IMAGE_VIEW_TYPE_3D;
    }
    if (cube) {
        return VK_IMAGE_VIEW_TYPE_CUBE;
    }
    return (type == VK_IMAGE_TYPE_2D) ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
}

// Transfer bits are always added: TextureManager::updateTexture() is legal on
// every texture, and generateMipmaps() blits level to level. Without them an
// upload or a mip request would fail on an image the caller created as
// sample-only.
VkImageUsageFlags translate_image_usage(uint32_t usage) {
    VkImageUsageFlags flags = 0;
    if (usage & TextureUsage::SAMPLED)                 flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (usage & TextureUsage::STORAGE)                 flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (usage & TextureUsage::COLOR_ATTACHMENT)        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (usage & TextureUsage::DEPTH_STENCIL_ATTACHMENT) flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (usage & TextureUsage::TRANSFER_SRC)            flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (usage & TextureUsage::TRANSFER_DST)            flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (usage & TextureUsage::INPUT_ATTACHMENT)        flags |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    if (flags == 0) {
        // A texture nobody can use is a caller bug; sampling is the least
        // surprising recovery.
        flags = VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    return flags | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
}

VkAccessFlags access_for_layout(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED: return 0;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        default: return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }
}

VkPipelineStageFlags stage_for_layout(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED: return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_PIPELINE_STAGE_TRANSFER_BIT;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        default: return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
}

// One VkDeviceMemory block plus whether VMA or we own it. Same shape as the
// buffer manager's block, kept in sync by hand rather than shared.
struct MemoryBlock {
    VmaAllocation vma_allocation = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool vma_owned = false;
    bool host_visible = false;
    bool host_coherent = false;
    VkDeviceSize size = 0;
    void* mapped = nullptr;

    bool map(VkDevice device, VmaAllocator allocator) {
        if (mapped != nullptr) {
            return true;
        }
        if (!host_visible) {
            return false;
        }
        if (vma_owned) {
            if (allocator == VK_NULL_HANDLE) {
                return false;
            }
            return vmaMapMemory(allocator, vma_allocation, &mapped) == VK_SUCCESS;
        }
        return vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS;
    }

    void unmap(VkDevice device, VmaAllocator allocator) {
        if (mapped == nullptr) {
            return;
        }
        if (vma_owned) {
            vmaUnmapMemory(allocator, vma_allocation);
        } else {
            vkUnmapMemory(device, memory);
        }
        mapped = nullptr;
    }

    // A null device means the renderer already tore the device down: the handles
    // are dropped without touching the driver.
    void release(VkDevice device, VmaAllocator allocator) {
        if (device == VK_NULL_HANDLE) {
            vma_allocation = VK_NULL_HANDLE;
            memory = VK_NULL_HANDLE;
            vma_owned = false;
            return;
        }
        unmap(device, allocator);
        if (vma_owned) {
            if (vma_allocation != VK_NULL_HANDLE) {
                vmaFreeMemory(allocator, vma_allocation);
            }
        } else if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, memory, nullptr);
        }
        vma_allocation = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        vma_owned = false;
    }
};

void destroy_image_and_memory(VkDevice device, VmaAllocator allocator, VkImage image,
                              VkImageView view, VkImageView transfer_view, MemoryBlock& memory) {
    if (device == VK_NULL_HANDLE) {
        return;
    }
    if (view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, view, nullptr);
    }
    if (transfer_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, transfer_view, nullptr);
    }
    if (image != VK_NULL_HANDLE) {
        if (memory.vma_owned) {
            if (allocator == VK_NULL_HANDLE) {
                LOGE("cannot destroy a VMA-owned image without an allocator; leaking");
            } else {
                vmaDestroyImage(allocator, image, memory.vma_allocation, 0);
                memory.mapped = nullptr;
                memory.vma_allocation = VK_NULL_HANDLE;
                memory.memory = VK_NULL_HANDLE;
                memory.vma_owned = false;
            }
        } else {
            vkDestroyImage(device, image, nullptr);
        }
    }
    memory.release(device, allocator);
}

// VkImage / VkImageView are pointers on 64-bit ABIs and integer handles on
// 32-bit ones, while VK_EXT_debug_utils always wants a uint64_t.
template <typename Handle>
uint64_t handle_id(Handle handle) {
    if constexpr (std::is_pointer_v<Handle>) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    } else {
        return static_cast<uint64_t>(handle);
    }
}

bool looks_like_compressed_container(const void* data, uint64_t size) {
    static const uint8_t kPng[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    static const uint8_t kJpeg[] = {0xFF, 0xD8, 0xFF};
    static const uint8_t kKtx2[] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30};
    static const uint8_t kBasis[] = {0x73, 0x42, 0x61, 0x73, 0x69, 0x73};
    if (data == nullptr || size == 0) {
        return false;
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    if (size >= sizeof(kPng) && std::memcmp(bytes, kPng, sizeof(kPng)) == 0) {
        return true;
    }
    if (size >= sizeof(kJpeg) && std::memcmp(bytes, kJpeg, sizeof(kJpeg)) == 0) {
        return true;
    }
    if (size >= sizeof(kKtx2) && std::memcmp(bytes, kKtx2, sizeof(kKtx2)) == 0) {
        return true;
    }
    if (size >= sizeof(kBasis) && std::memcmp(bytes, kBasis, sizeof(kBasis)) == 0) {
        return true;
    }
    // DDS starts with the ASCII tag "DDS ".
    if (size >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ') {
        return true;
    }
    return false;
}

// TextureManager::createTextureCube() routes through createTextureArray(), and
// the onCreateTextureArray() hook cannot tell a cube map from an ordinary 6-layer
// array. createTextureCube() therefore raises this one-shot flag right before
// delegating to the base class and the hook consumes it. It is thread_local
// rather than a member flag because the base call and the hook run on the same
// thread, and holding a mutex across them would self-deadlock on the
// non-recursive base mutex.
thread_local bool t_cube_pending = false;

} // namespace

class VulkanTextureManager::Impl {
public:
    struct Record {
        uint64_t handle = 0;
        VkImage image = VK_NULL_HANDLE;
        // View matching the image kind (2D / 2D_ARRAY / 3D / CUBE): the one a
        // descriptor set should reference.
        VkImageView view = VK_NULL_HANDLE;
        // 2D_ARRAY (3D for volumes) view over every level and layer. vkCmdBlitImage
        // and vkCmdCopyImage forbid a CUBE view of a cube-compatible image, so the
        // mip chain and cross-texture copies use this one instead.
        VkImageView transfer_view = VK_NULL_HANDLE;
        MemoryBlock memory;
        VkDeviceSize allocation_size = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageType type = VK_IMAGE_TYPE_2D;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 1;
        uint32_t mip_levels = 1;
        uint32_t array_layers = 1;
        bool cube = false;
        bool can_blit = false;
        uint32_t usage = 0;
        uint32_t texture_format = TextureFormat::NONE;
        uint64_t last_gpu_use_frame = kNoFrame;
        bool used_by_gpu = false;
        Texture base;
    };

    explicit Impl(VulkanRenderer* renderer) : renderer(renderer) {}

    ~Impl() {
        destroy_transient_locked();
    }

    VulkanRenderer* renderer = nullptr;

    // Guards `textures` only. Never held across vkQueueSubmit() or a device
    // wait: TextureManager holds its own lock while calling the hooks, so the
    // hook must never call back into a public TextureManager method.
    std::unordered_map<uint64_t, Record> textures;
    std::mutex mutex;

    // --- transient staging resources, serialized by upload_mutex --------
    // One staging buffer + command buffer + fence for every upload, so uploads
    // queue up behind each other instead of racing. A ring of
    // renderer.framesInFlight() slots would remove the per-upload fence wait but
    // needs the renderer's private fence ring.
    std::mutex upload_mutex;
    VkCommandBuffer upload_command_buffer = VK_NULL_HANDLE;
    VkFence upload_fence = VK_NULL_HANDLE;
    VkBuffer staging_buffer = VK_NULL_HANDLE;
    MemoryBlock staging_memory;
    VkDeviceSize staging_size = 0;
    uint64_t uploaded_bytes = 0;
    PFN_vkSetDebugUtilsObjectNameEXT debug_name_fn = nullptr;

    VkDevice device() const { return renderer != nullptr ? renderer->device() : VK_NULL_HANDLE; }
    VmaAllocator allocator() const {
        return renderer != nullptr ? renderer->allocator() : VK_NULL_HANDLE;
    }

    VkPhysicalDeviceMemoryProperties memory_properties() const {
        if (!memory_properties_valid && renderer != nullptr) {
            VkPhysicalDevice physical = renderer->physicalDevice();
            if (physical != VK_NULL_HANDLE) {
                // Memory types never change for the lifetime of a physical device.
                vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);
                memory_properties_valid = true;
            }
        }
        return memory_props;
    }

    uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags required,
                              VkMemoryPropertyFlags preferred,
                              VkMemoryPropertyFlags discouraged) const {
        if (type_bits == 0) {
            type_bits = ~0u;
        }
        const VkPhysicalDeviceMemoryProperties props = memory_properties();
        uint32_t fallback = UINT32_MAX;
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if ((type_bits & (1u << i)) == 0) {
                continue;
            }
            const VkMemoryPropertyFlags flags = props.memoryTypeProperties[i].propertyFlags;
            if ((flags & required) != required) {
                continue;
            }
            if (discouraged != 0 && (flags & discouraged) != 0) {
                if (fallback == UINT32_MAX) {
                    fallback = i;
                }
                continue;
            }
            if (preferred == 0 || (flags & preferred) == preferred) {
                return i;
            }
            if (fallback == UINT32_MAX) {
                fallback = i;
            }
        }
        return fallback;
    }

    // The single memory seam. Prefers VMA when the renderer exposes an allocator
    // and otherwise picks a memory type manually. Nothing is *required* for image
    // memory (a device without a dedicated heap still gets a working, slower
    // allocation), but DEVICE_LOCAL is strongly preferred.
    bool allocate_and_bind(VkImage image, uint32_t memory_type_bits, VkDeviceSize size,
                           MemoryBlock& out) {
        const VkDevice device = this->device();
        if (device == VK_NULL_HANDLE || image == VK_NULL_HANDLE || size == 0) {
            return false;
        }

        const VmaAllocator allocator = this->allocator();
        if (allocator != VK_NULL_HANDLE) {
            VmaAllocationInfo info{};
            // VMA_ALLOCATION_CREATE_HOST_ACCESS_* and VmaMemoryUsage stay at their
            // defaults on purpose: those enums were renamed between VMA releases.
            info.allocationSize = size;
            info.memoryTypeBits = memory_type_bits != 0 ? memory_type_bits : ~0u;

            VmaAllocation allocation = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            if (vmaAllocateMemory(allocator, &info, &allocation, &memory) != VK_SUCCESS) {
                LOGE("vmaAllocateMemory failed for a %llu byte image",
                     static_cast<unsigned long long>(size));
                return false;
            }
            if (vmaBindImageMemory(allocator, allocation, image) != VK_SUCCESS) {
                vmaFreeMemory(allocator, allocation);
                LOGE("vmaBindImageMemory failed");
                return false;
            }
            out.vma_allocation = allocation;
            out.memory = memory;
            out.vma_owned = true;
            out.size = size;
            return true;
        }

        const uint32_t type = find_memory_type(
            memory_type_bits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        if (type == UINT32_MAX) {
            LOGE("no memory type available for a %llu byte image",
                 static_cast<unsigned long long>(size));
            return false;
        }
        VkMemoryAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = size;
        alloc_info.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &alloc_info, nullptr, &out.memory) != VK_SUCCESS) {
            LOGE("vkAllocateMemory(%llu) failed", static_cast<unsigned long long>(size));
            out.memory = VK_NULL_HANDLE;
            return false;
        }
        if (vkBindImageMemory(device, image, out.memory, 0) != VK_SUCCESS) {
            LOGE("vkBindImageMemory failed");
            vkFreeMemory(device, out.memory, nullptr);
            out.memory = VK_NULL_HANDLE;
            return false;
        }
        out.vma_owned = false;
        out.size = size;
        const VkMemoryPropertyFlags flags = memory_properties().memoryTypeProperties[type].propertyFlags;
        out.host_visible = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        out.host_coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        return true;
    }

    // --- image creation --------------------------------------------------

    // Shared path for 2D / 3D / array / cube images. Mip chains are clamped to
    // what the extent actually allows, views are created for both sampling and
    // transfers, and the record is only published once everything succeeded.
    bool create_image(uint64_t handle, VkImageType type, uint32_t width, uint32_t height,
                      uint32_t depth, uint32_t array_layers, uint32_t format_constant,
                      uint32_t usage, uint32_t mip_levels, bool cube) {
        const VkDevice device = this->device();
        if (device == VK_NULL_HANDLE) {
            LOGE("onCreateTexture without a Vulkan device");
            return false;
        }
        if (width == 0 || height == 0 || array_layers == 0) {
            LOGE("texture %llu has a zero extent or layer count",
                 static_cast<unsigned long long>(handle));
            return false;
        }
        if (cube && (array_layers != 6 || width != height)) {
            LOGE("cube texture %llu needs 6 square layers (%u x %u, %u layers)",
                 static_cast<unsigned long long>(handle), width, height, array_layers);
            return false;
        }
        if (cube) {
            // A cube-compatible image must be VK_IMAGE_TYPE_2D with six array
            // layers; 2D_ARRAY is rejected even though the extent is the same.
            type = VK_IMAGE_TYPE_2D;
        }
        if (find_format(format_constant) == nullptr) {
            LOGE("texture %llu: unknown TextureFormat constant %u",
                 static_cast<unsigned long long>(handle), format_constant);
            return false;
        }

        const VkFormat format = resolve_format_for(renderer, format_constant);
        if (format == VK_FORMAT_UNDEFINED) {
            return false;
        }
        const FormatInfo& info = format_info_for(format);
        if (info.compressed && type == VK_IMAGE_TYPE_3D) {
            LOGE("texture %llu: block compressed 3D images are not a valid combination",
                 static_cast<unsigned long long>(handle));
            return false;
        }

        // mip_levels == 0 means "auto" (full chain); anything above the real chain
        // would make vkCreateImage fail, so clamp instead.
        const uint32_t largest = std::max({width, height, type == VK_IMAGE_TYPE_3D ? depth : 1u});
        uint32_t levels = (mip_levels == 0 || mip_levels > full_mip_levels(largest))
                              ? full_mip_levels(largest)
                              : mip_levels;
        if (cube) {
            // A VK_IMAGE_VIEW_TYPE_CUBE view may not cover more levels than the
            // cube chain allows.
            levels = std::min(levels, full_mip_levels(std::max(width, height)));
        }

        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = type;
        image_info.extent = VkExtent3D{width, height, type == VK_IMAGE_TYPE_3D ? depth : 1};
        image_info.mipLevels = levels;
        image_info.arrayLayers = array_layers;
        image_info.format = format;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Optimal tiling only: the image is never CPU-mapped, uploads go through
        // a staging buffer, and compressed data is already in the GPU's block
        // layout.
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = translate_image_usage(usage);
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;

        VkImage image = VK_NULL_HANDLE;
        if (vkCreateImage(device, &image_info, nullptr, &image) != VK_SUCCESS) {
            LOGE("vkCreateImage failed for %u x %u (format %d, %u levels, %u layers)", width,
                 height, static_cast<int>(format), levels, array_layers);
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, image, &requirements);

        Record record;
        record.handle = handle;
        record.image = image;
        record.format = format;
        record.type = type;
        record.aspect = aspect_for(info);
        record.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        record.width = width;
        record.height = height;
        record.depth = type == VK_IMAGE_TYPE_3D ? depth : 1;
        record.mip_levels = levels;
        record.array_layers = array_layers;
        record.cube = cube;
        record.usage = usage;
        record.texture_format = format_constant;
        record.allocation_size = requirements.size;
        record.last_gpu_use_frame = kNoFrame;

        if (!allocate_and_bind(image, requirements.memoryTypeBits, requirements.size,
                               record.memory)) {
            vkDestroyImage(device, image, nullptr);
            return false;
        }

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image;
        view_info.format = format;
        view_info.components = VkComponentMapping{
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        view_info.subresourceRange =
            VkImageSubresourceRange{record.aspect, 0, levels, 0, array_layers};
        view_info.viewType = view_type_for(type, cube);
        if (vkCreateImageView(device, &view_info, nullptr, &record.view) != VK_SUCCESS) {
            LOGE("vkCreateImageView failed for the primary view");
            destroy_image_and_memory(device, allocator(), image, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                     record.memory);
            return false;
        }
        // The transfer view is never CUBE, so blits and image copies accept it.
        view_info.viewType = (type == VK_IMAGE_TYPE_3D) ? VK_IMAGE_VIEW_TYPE_3D
                                                        : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (vkCreateImageView(device, &view_info, nullptr, &record.transfer_view) != VK_SUCCESS) {
            LOGE("vkCreateImageView failed for the transfer view");
            destroy_image_and_memory(device, allocator(), image, record.view, VK_NULL_HANDLE,
                                     record.memory);
            return false;
        }

        // Cache whether vkCmdBlitImage can build the chain from this format.
        const VkPhysicalDevice physical = renderer != nullptr ? renderer->physicalDevice()
                                                              : VK_NULL_HANDLE;
        if (physical != VK_NULL_HANDLE && !info.compressed && !info.depth_stencil) {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(physical, format, &props);
            const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                                VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            record.can_blit = (props.optimalTilingFeatures & needed) == needed;
        }

        record.base.handle = handle;
        record.base.width = width;
        record.base.height = height;
        record.base.depth = record.depth;
        record.base.format = format_constant;
        record.base.usage = usage;
        record.base.mip_levels = levels;
        record.base.array_layers = array_layers;

        {
            std::lock_guard lock(mutex);
            textures[handle] = std::move(record);
        }
        LOGI("texture %llu: %u x %u%s %u level(s) %u layer(s) format=%d%s",
             static_cast<unsigned long long>(handle), width, height,
             type == VK_IMAGE_TYPE_3D ? "xD" : "", levels, array_layers, static_cast<int>(format),
             cube ? " (cube)" : "");
        return true;
    }

    // --- transient resources ---------------------------------------------

    // upload_mutex must be held and the caller must have waited on upload_fence_.
    bool grow_staging_locked(VkDeviceSize bytes) {
        const VkDevice device = this->device();
        if (device == VK_NULL_HANDLE) {
            return false;
        }
        destroy_staging_locked();

        // Grow geometrically: uploads are typically a handful of small blocks and
        // re-allocating on every new maximum would thrash.
        const VkDeviceSize size =
            std::max(bytes, std::max<VkDeviceSize>(kMinStagingBytes, staging_size * 2));

        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        if (vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS) {
            LOGE("vkCreateBuffer failed for the staging buffer");
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);

        const uint32_t type =
            find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) {
            vkDestroyBuffer(device, buffer, nullptr);
            LOGE("no host-visible memory type available for staging");
            return false;
        }
        VkMemoryAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = requirements.size;
        alloc_info.memoryTypeIndex = type;
        MemoryBlock block;
        if (vkAllocateMemory(device, &alloc_info, nullptr, &block.memory) != VK_SUCCESS) {
            vkDestroyBuffer(device, buffer, nullptr);
            LOGE("vkAllocateMemory for staging failed");
            return false;
        }
        if (vkBindBufferMemory(device, buffer, block.memory, 0) != VK_SUCCESS) {
            vkFreeMemory(device, block.memory, nullptr);
            vkDestroyBuffer(device, buffer, nullptr);
            return false;
        }
        const VkMemoryPropertyFlags flags = memory_properties().memoryTypeProperties[type].propertyFlags;
        block.size = requirements.size;
        block.host_visible = true;
        block.host_coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        if (!block.map(device, VK_NULL_HANDLE)) {
            block.release(device, VK_NULL_HANDLE);
            vkDestroyBuffer(device, buffer, nullptr);
            LOGE("staging memory could not be mapped");
            return false;
        }
        staging_buffer = buffer;
        staging_memory = block;
        staging_size = std::max(size, requirements.size);
        return true;
    }

    // upload_mutex must be held.
    void destroy_staging_locked() {
        const VkDevice device = this->device();
        if (staging_buffer != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, staging_buffer, nullptr);
        }
        staging_buffer = VK_NULL_HANDLE;
        // Staging memory is always the manual path here (see grow_staging_locked);
        // if VMA ever takes it over, this has to learn about the allocation.
        staging_memory.release(device, VK_NULL_HANDLE);
        staging_size = 0;
    }

    // upload_mutex must be held.
    void destroy_transient_locked() {
        const VkDevice device = this->device();
        destroy_staging_locked();
        if (device == VK_NULL_HANDLE) {
            upload_command_buffer = VK_NULL_HANDLE;
            upload_fence = VK_NULL_HANDLE;
            return;
        }
        const VkCommandPool pool = renderer != nullptr ? renderer->commandPool() : VK_NULL_HANDLE;
        if (upload_command_buffer != VK_NULL_HANDLE && pool != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(device, pool, 1, &upload_command_buffer);
        }
        upload_command_buffer = VK_NULL_HANDLE;
        if (upload_fence != VK_NULL_HANDLE) {
            vkDestroyFence(device, upload_fence, nullptr);
        }
        upload_fence = VK_NULL_HANDLE;
    }

    // upload_mutex must be held.
    bool wait_for_upload_locked(uint64_t timeout_ns) {
        const VkDevice device = this->device();
        if (upload_fence == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
            return true;
        }
        const VkResult result = vkWaitForFences(device, 1, &upload_fence, VK_TRUE, timeout_ns);
        if (result != VK_SUCCESS) {
            LOGW("waiting for the texture upload fence returned %d", static_cast<int>(result));
            return false;
        }
        return true;
    }

    // upload_mutex must be held.
    bool ensure_transient_locked(VkDeviceSize min_staging_bytes) {
        const VkDevice device = this->device();
        const VkCommandPool pool = renderer != nullptr ? renderer->commandPool() : VK_NULL_HANDLE;
        if (device == VK_NULL_HANDLE || pool == VK_NULL_HANDLE) {
            LOGE("no device/command pool available for texture staging work");
            return false;
        }
        if (upload_fence == VK_NULL_HANDLE) {
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            // Start signalled: nothing has been submitted yet.
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkCreateFence(device, &fence_info, nullptr, &upload_fence) != VK_SUCCESS) {
                upload_fence = VK_NULL_HANDLE;
                return false;
            }
        }
        if (upload_command_buffer == VK_NULL_HANDLE) {
            VkCommandBufferAllocateInfo alloc_info{};
            alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            alloc_info.commandPool = pool;
            alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc_info.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(device, &alloc_info, &upload_command_buffer) != VK_SUCCESS) {
                upload_command_buffer = VK_NULL_HANDLE;
                return false;
            }
        }
        if (min_staging_bytes == 0) {
            return true;
        }
        if (staging_buffer != VK_NULL_HANDLE && staging_size >= min_staging_bytes) {
            return true;
        }
        return grow_staging_locked(min_staging_bytes);
    }

    // upload_mutex must be held. Records on this manager's own transient command
    // buffer and submits it on the graphics queue with upload_fence_.
    //
    // It deliberately does NOT use the renderer's frame command buffer:
    // VulkanRenderer::onEndFrame() resets and re-records command_buffers_[frame]
    // from scratch every frame and is the only place it is open for recording, so
    // anything recorded from outside would be discarded. Submissions to one queue
    // execute in order, so this work is guaranteed to have run before the frame
    // that the render thread submits afterwards.
    bool submit_locked(const std::function<void(VkCommandBuffer)>& record) {
        const VkDevice device = this->device();
        const VkQueue queue = renderer != nullptr ? renderer->graphicsQueue() : VK_NULL_HANDLE;
        if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE ||
            upload_command_buffer == VK_NULL_HANDLE) {
            LOGE("texture transient submit without device/queue/command buffer");
            return false;
        }
        if (vkResetCommandBuffer(upload_command_buffer, 0) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(upload_command_buffer, &begin) != VK_SUCCESS) {
            return false;
        }
        record(upload_command_buffer);
        if (vkEndCommandBuffer(upload_command_buffer) != VK_SUCCESS) {
            return false;
        }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &upload_command_buffer;
        if (vkResetFences(device, 1, &upload_fence) != VK_SUCCESS) {
            return false;
        }
        const VkResult result = vkQueueSubmit(queue, 1, &submit, upload_fence);
        if (result != VK_SUCCESS) {
            LOGW("vkQueueSubmit for a texture upload returned %d", static_cast<int>(result));
            return false;
        }
        return true;
    }

    // submit + wait, for callers that must have the result finished before they
    // continue (mipmap generation, public layout transitions).
    bool submit_and_wait_locked(const std::function<void(VkCommandBuffer)>& record) {
        if (!submit_locked(record)) {
            return false;
        }
        return wait_for_upload_locked(kUploadWaitTimeoutNs);
    }

    // upload_mutex must be held. Host writes to non-coherent memory are only
    // visible to the device after a flush; coherent memory needs none. Offsets
    // and sizes must be multiples of nonCoherentAtomSize, so the range is rounded
    // up (and clamped to the allocation).
    void flush_staging_locked(VkDeviceSize bytes) {
        if (staging_memory.host_coherent || staging_memory.mapped == nullptr) {
            return;
        }
        VkDeviceSize atom = 1;
        if (renderer != nullptr && renderer->physicalDevice() != VK_NULL_HANDLE) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(renderer->physicalDevice(), &properties);
            atom = std::max<VkDeviceSize>(properties.limits.nonCoherentAtomSize, 1);
        }
        VkDeviceSize size = std::min((bytes + atom - 1) & ~(atom - 1), staging_size);
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = staging_memory.memory;
        range.offset = 0;
        range.size = size;
        vkFlushMappedMemoryRanges(device(), 1, &range);
    }

    // --- the single layout transition helper ----------------------------
    // Every transition in this file (first upload, re-upload, cross-texture
    // copy, mip chain, public transitionImage) records exactly one barrier
    // through here, so stages and access masks cannot drift apart between call
    // sites. Both are derived from the two layouts.
    void transition_image_locked(VkCommandBuffer command_buffer, VkImage image,
                                 VkImageAspectFlags aspect, VkImageLayout old_layout,
                                 VkImageLayout new_layout, uint32_t base_mip, uint32_t mip_count,
                                 uint32_t base_layer, uint32_t layer_count) {
        if (image == VK_NULL_HANDLE || old_layout == new_layout) {
            return;
        }
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = access_for_layout(old_layout);
        barrier.dstAccessMask = access_for_layout(new_layout);
        barrier.oldLayout = old_layout;
        barrier.newLayout = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange =
            VkImageSubresourceRange{aspect, base_mip, mip_count, base_layer, layer_count};
        vkCmdPipelineBarrier(command_buffer, stage_for_layout(old_layout),
                             stage_for_layout(new_layout), 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    // --- in-flight bookkeeping ------------------------------------------
    // Same rule as the buffer manager: a frame submitted at index N has retired
    // once the renderer reports frame N + framesInFlight, because onBeginFrame()
    // waits on the fence of the frame slot it is about to reuse. Rewriting an
    // image that a live frame still samples would produce a torn frame, so the
    // write is refused and logged instead.
    bool in_flight_locked(const Record& record) const {
        if (record.last_gpu_use_frame == kNoFrame || renderer == nullptr) {
            return false;
        }
        const uint64_t frame = renderer->getFrameNumber();
        const uint64_t frames = std::max<uint32_t>(renderer->framesInFlight(), 1u);
        return record.last_gpu_use_frame + frames > frame;
    }

    bool can_rewrite_locked(const Record& record) const {
        if (!in_flight_locked(record)) {
            return true;
        }
        LOGW("texture %llu is still in use by an unfinished frame (stamped %llu, now %llu); "
             "write skipped",
             static_cast<unsigned long long>(record.handle),
             static_cast<unsigned long long>(record.last_gpu_use_frame),
             static_cast<unsigned long long>(renderer != nullptr ? renderer->getFrameNumber() : 0));
        return false;
    }

    // mutex must be held.
    void stamp_locked(Record& record) const {
        record.last_gpu_use_frame = renderer != nullptr ? renderer->getFrameNumber() : 0;
        record.used_by_gpu = true;
    }

    void set_layout(uint64_t handle, VkImageLayout layout) {
        std::lock_guard lock(mutex);
        auto it = textures.find(handle);
        if (it != textures.end()) {
            it->second.layout = layout;
        }
    }

    void set_debug_name(VkDevice device, VkObjectType type, uint64_t handle, const std::string& name) {
        if (device == VK_NULL_HANDLE || name.empty()) {
            return;
        }
        if (debug_name_fn == nullptr) {
            // VK_EXT_debug_utils is optional and Android does not link it, so the
            // entry point has to be fetched. Missing debug utils is not an error.
            debug_name_fn = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
                vkGetDeviceProcAddr(device, "vkSetDebugUtilsObjectNameEXT"));
            if (debug_name_fn == nullptr) {
                return;
            }
        }
        VkDebugUtilsObjectNameInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        info.objectType = type;
        info.objectHandle = handle;
        info.pObjectName = name.c_str();
        debug_name_fn(device, &info);
    }

private:
    mutable VkPhysicalDeviceMemoryProperties memory_props{};
    mutable bool memory_properties_valid = false;
};

// --- format handling --------------------------------------------------------

VkFormat VulkanTextureManager::toVkFormat(uint32_t format) {
    const FormatInfo* info = find_format(format);
    return info != nullptr ? info->vk_format : VK_FORMAT_UNDEFINED;
}

bool VulkanTextureManager::isBlockCompressed(uint32_t format) {
    const FormatInfo* info = find_format(format);
    return info != nullptr && info->compressed;
}

uint64_t VulkanTextureManager::mipByteSize(uint32_t format, uint32_t width, uint32_t height,
                                           uint32_t depth) {
    const FormatInfo* info = find_format(format);
    if (info == nullptr) {
        return 0;
    }
    return byte_size_for(*info, width, height, depth);
}

VkFormat VulkanTextureManager::resolveFormat(uint32_t format) const {
    return resolve_format_for(renderer_, format);
}

// --- construction -----------------------------------------------------------

VulkanTextureManager::VulkanTextureManager(VulkanRenderer* renderer)
    : pImpl(std::make_unique<Impl>(renderer)), renderer_(renderer) {
    if (renderer_ == nullptr) {
        LOGE("VulkanTextureManager built without a renderer; every call will fail");
    }
}

VulkanTextureManager::~VulkanTextureManager() {
    releaseDeviceResources();
}

void VulkanTextureManager::releaseDeviceResources() {
    if (!pImpl) {
        return;
    }
    std::lock_guard lock(pImpl->upload_mutex);
    pImpl->destroy_transient_locked();
}

// --- backend queries --------------------------------------------------------

VkImage VulkanTextureManager::image(uint64_t handle) const {
    std::lock_guard lock(pImpl->mutex);
    auto it = pImpl->textures.find(handle);
    return it == pImpl->textures.end() ? VK_NULL_HANDLE : it->second.image;
}

VkImageView VulkanTextureManager::imageView(uint64_t handle) const {
    std::lock_guard lock(pImpl->mutex);
    auto it = pImpl->textures.find(handle);
    return it == pImpl->textures.end() ? VK_NULL_HANDLE : it->second.view;
}

bool VulkanTextureManager::imageExtent(uint64_t handle, uint32_t* width, uint32_t* height,
                                    uint32_t* array_layers) const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    const auto it = pImpl->textures.find(handle);
    if (it == pImpl->textures.end()) {
        return false;
    }
    if (width != nullptr) {
        *width = it->second.width;
    }
    if (height != nullptr) {
        *height = it->second.height;
    }
    if (array_layers != nullptr) {
        *array_layers = it->second.array_layers;
    }
    return true;
}

VkFormat VulkanTextureManager::imageFormat(uint64_t handle) const {
    std::lock_guard lock(pImpl->mutex);
    auto it = pImpl->textures.find(handle);
    return it == pImpl->textures.end() ? VK_FORMAT_UNDEFINED : it->second.format;
}

bool VulkanTextureManager::isInUseByGpu(uint64_t handle) const {
    std::lock_guard lock(pImpl->mutex);
    auto it = pImpl->textures.find(handle);
    return it != pImpl->textures.end() && pImpl->in_flight_locked(it->second);
}

bool VulkanTextureManager::waitForUploads(uint64_t timeout_ns) {
    std::lock_guard lock(pImpl->upload_mutex);
    return pImpl->wait_for_upload_locked(timeout_ns);
}

size_t VulkanTextureManager::getCacheSizeMb() const {
    std::lock_guard lock(pImpl->mutex);
    uint64_t bytes = 0;
    for (const auto& entry : pImpl->textures) {
        bytes += entry.second.allocation_size;
    }
    return static_cast<size_t>(bytes / kMiB);
}

// --- image creation ---------------------------------------------------------

bool VulkanTextureManager::onCreateTexture2D(uint64_t handle, uint32_t width, uint32_t height,
                                             uint32_t format, uint32_t usage,
                                             uint32_t mip_levels) {
    return pImpl->create_image(handle, VK_IMAGE_TYPE_2D, width, height, 1, 1, format, usage,
                               mip_levels, /*cube=*/false);
}

bool VulkanTextureManager::onCreateTexture3D(uint64_t handle, uint32_t width, uint32_t height,
                                             uint32_t depth, uint32_t format, uint32_t usage,
                                             uint32_t mip_levels) {
    if (depth == 0) {
        LOGE("3D texture %llu has depth 0", static_cast<unsigned long long>(handle));
        return false;
    }
    return pImpl->create_image(handle, VK_IMAGE_TYPE_3D, width, height, depth, 1, format, usage,
                               mip_levels, /*cube=*/false);
}

bool VulkanTextureManager::onCreateTextureArray(uint64_t handle, uint32_t width, uint32_t height,
                                                uint32_t array_layers, uint32_t format,
                                                uint32_t usage, uint32_t mip_levels) {
    const bool cube = t_cube_pending;
    t_cube_pending = false;
    return pImpl->create_image(handle,
                               array_layers > 1 ? VK_IMAGE_TYPE_2D_ARRAY : VK_IMAGE_TYPE_2D,
                               width, height, 1, array_layers, format, usage, mip_levels, cube);
}

// TextureManager::createTextureCube() drops the cube-ness on the floor: it just
// forwards to createTextureArray(..., 6, ...), and onCreateTextureArray() then has
// no way to add VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT or build a CUBE view. The
// override raises a thread_local one-shot flag, delegates to the base class (which
// owns the handle bookkeeping) and lets the hook consume the flag.
uint64_t VulkanTextureManager::createTextureCube(uint32_t width, uint32_t height, uint32_t format,
                                                 uint32_t usage, uint32_t mip_levels) {
    t_cube_pending = true;
    const uint64_t handle =
        TextureManager::createTextureArray(width, height, 6, format, usage, mip_levels);
    t_cube_pending = false;
    return handle;
}

void VulkanTextureManager::onDestroyTexture(uint64_t handle) {
    Impl::Record record;
    {
        std::lock_guard lock(pImpl->mutex);
        auto it = pImpl->textures.find(handle);
        if (it == pImpl->textures.end()) {
            return;
        }
        record = std::move(it->second);
        pImpl->textures.erase(it);
    }

    const VkDevice device = renderer_ != nullptr ? renderer_->device() : VK_NULL_HANDLE;
    if (device == VK_NULL_HANDLE) {
        return;
    }
    if (record.used_by_gpu) {
        // vkDeviceWaitIdle() drains the whole device (swapchain included), so it
        // is reserved for teardown - level changes, cache eviction, shutdown - and
        // never used in steady state. The per-frame fence ring in VulkanRenderer
        // would be the cheap alternative, but in_flight_fences_ is private; add an
        // `inFlightFences()` accessor if teardown shows up in a profile.
        vkDeviceWaitIdle(device);
    }
    destroy_image_and_memory(device, renderer_->allocator(), record.image, record.view,
                             record.transfer_view, record.memory);
}

// --- uploads ----------------------------------------------------------------

void VulkanTextureManager::onUpdateTexture(uint64_t handle, uint32_t mip_level, uint32_t array_layer,
                                           uint32_t x, uint32_t y, uint32_t z, uint32_t width,
                                           uint32_t height, uint32_t depth, const void* data,
                                           uint64_t data_size) {
    if (data == nullptr || data_size == 0 || width == 0 || height == 0) {
        return;
    }

    // Snapshot the record and apply the in-flight guard before taking
    // upload_mutex: the staging work must not run while pImpl->mutex is held.
    Impl::Record record;
    {
        std::lock_guard lock(pImpl->mutex);
        auto it = pImpl->textures.find(handle);
        if (it == pImpl->textures.end()) {
            return;
        }
        if (mip_level >= it->second.mip_levels) {
            LOGE("updateTexture: mip %u is outside the %u level chain", mip_level,
                 it->second.mip_levels);
            return;
        }
        if (!pImpl->can_rewrite_locked(it->second)) {
            return;
        }
        record = it->second;
    }

    std::lock_guard upload_lock(pImpl->upload_mutex);
    uploadRegion(record, mip_level, array_layer, x, y, z, width, height, depth, data, data_size);
}

// pImpl->upload_mutex must be held. Validates the region, stages the bytes and
// records old -> TRANSFER_DST_OPTIMAL -> shader-read on the transient command
// buffer. Shared by onUpdateTexture() and by the multi-layer / chain paths, which
// need several regions inside one lock hold.
bool VulkanTextureManager::uploadRegion(const Impl::Record& record, uint32_t mip_level,
                                        uint32_t array_layer, uint32_t x, uint32_t y, uint32_t z,
                                        uint32_t width, uint32_t height, uint32_t depth,
                                        const void* data, uint64_t data_size) {
    if (mip_level >= record.mip_levels) {
        LOGE("upload: mip %u is outside the %u level chain", mip_level, record.mip_levels);
        return false;
    }

    // Size and aspect decisions follow the format actually in use, which may be a
    // fallback of the requested one.
    const FormatInfo& info = format_info_for(record.format);
    const bool is_3d = record.type == VK_IMAGE_TYPE_3D;
    if (!is_3d && depth != 1) {
        LOGE("upload: depth must be 1 for 2D, array and cube textures");
        return false;
    }
    if (!is_3d && z != 0) {
        // imageOffset.z is only meaningful for 3D images; for 2D/array/cube it
        // must be zero or the copy is invalid.
        LOGE("upload: z must be 0 for 2D, array and cube textures");
        return false;
    }
    if (!is_3d && array_layer >= record.array_layers) {
        LOGE("upload: layer %u is outside the %u layers", array_layer, record.array_layers);
        return false;
    }

    const uint32_t mip_depth = is_3d ? record.depth : 1;
    const VkExtent3D mip_extent =
        compute_mip_extent(record.width, record.height, mip_depth, mip_level, info);
    if (x + width > mip_extent.width || y + height > mip_extent.height ||
        z + depth > mip_extent.depth) {
        LOGE("upload: region (%u,%u,%u)+(%u,%u,%u) is outside mip %u extent %u x %u x %u", x, y, z,
             width, height, depth, mip_level, mip_extent.width, mip_extent.height,
             mip_extent.depth);
        return false;
    }
    if (info.compressed &&
        (x % info.block_w != 0 || y % info.block_h != 0 || z % info.block_d != 0 ||
         width % info.block_w != 0 || height % info.block_h != 0 || depth % info.block_d != 0)) {
        // A block compressed region cannot be copied partially: a block cannot be
        // split, so a misaligned region is refused rather than silently truncated.
        LOGE("upload: compressed sub-regions must be block aligned (%dx%dx%d blocks)", info.block_w,
             info.block_h, info.block_d);
        return false;
    }

    const uint64_t copy_size = byte_size_for(info, width, height, depth);
    if (data_size < copy_size) {
        LOGE("upload: %llu bytes supplied, %llu required for %u x %u x %u",
             static_cast<unsigned long long>(data_size), static_cast<unsigned long long>(copy_size),
             width, height, depth);
        return false;
    }

    if (!pImpl->wait_for_upload_locked(kUploadWaitTimeoutNs)) {
        return false;
    }
    if (!pImpl->ensure_transient_locked(copy_size)) {
        return false;
    }
    uint8_t* const staging = static_cast<uint8_t*>(pImpl->staging_memory.mapped);
    if (staging == nullptr) {
        return false;
    }
    // Tightly packed: bufferRowLength / bufferImageHeight stay 0, so Vulkan uses
    // the extent's natural row pitch. Block compressed payloads arrive in the
    // GPU's block order and are copied straight into the optimal-tiled image: no
    // linear-tiled staging image and no re-tiling pass, which is both cheaper and
    // the only path every ETC2/ASTC driver is required to support.
    std::memcpy(staging, data, static_cast<size_t>(copy_size));
    pImpl->flush_staging_locked(copy_size);

    // UNDEFINED -> TRANSFER_DST_OPTIMAL discards the previous contents, which is
    // what an upload wants; TRANSFER_DST_OPTIMAL -> shader-read makes the result
    // visible to the frame that samples it next.
    const VkImageLayout target = shader_read_layout(info);
    const VkImageLayout old_layout = record.layout;
    const VkImage image = record.image;
    const VkImageAspectFlags aspect = record.aspect;
    const uint32_t mip_levels = record.mip_levels;
    const uint32_t layers = record.array_layers;

    const bool submitted = pImpl->submit_locked([&](VkCommandBuffer command_buffer) {
        pImpl->transition_image_locked(command_buffer, image, aspect, old_layout,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, mip_levels, 0,
                                       layers);
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        // For a 3D image the depth slice lives in imageOffset.z and
        // baseArrayLayer stays 0; for everything else imageOffset.z is 0.
        region.imageSubresource =
            VkImageSubresourceLayers{aspect, mip_level, is_3d ? 0u : array_layer, 1};
        region.imageOffset = VkOffset3D{static_cast<int32_t>(x), static_cast<int32_t>(y),
                                        static_cast<int32_t>(z)};
        region.imageExtent = VkExtent3D{width, height, depth};
        vkCmdCopyBufferToImage(command_buffer, pImpl->staging_buffer, image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        pImpl->transition_image_locked(command_buffer, image, aspect,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, target, 0, mip_levels,
                                       0, layers);
    });

    if (!submitted) {
        return false;
    }
    pImpl->uploaded_bytes += copy_size;
    pImpl->set_layout(record.handle, target);
    std::lock_guard lock(pImpl->mutex);
    auto it = pImpl->textures.find(record.handle);
    if (it != pImpl->textures.end()) {
        pImpl->stamp_locked(it->second);
    }
    return true;
}

// --- image copy -------------------------------------------------------------

void VulkanTextureManager::onCopyTexture(uint64_t src, uint64_t dst, uint32_t src_mip,
                                         uint32_t dst_mip, uint32_t src_layer,
                                         uint32_t dst_layer) {
    Impl::Record src_record;
    Impl::Record dst_record;
    {
        std::lock_guard lock(pImpl->mutex);
        auto src_it = pImpl->textures.find(src);
        auto dst_it = pImpl->textures.find(dst);
        if (src_it == pImpl->textures.end() || dst_it == pImpl->textures.end()) {
            return;
        }
        if (src_mip >= src_it->second.mip_levels || dst_mip >= dst_it->second.mip_levels) {
            LOGE("copyTexture: mip level out of range");
            return;
        }
        if (src_layer >= src_it->second.array_layers || dst_layer >= dst_it->second.array_layers) {
            LOGE("copyTexture: array layer out of range");
            return;
        }
        // vkCmdCopyImage needs identical formats and extents; scaling or format
        // conversion is what a blit or a shader pass is for.
        if (src_it->second.format != dst_it->second.format) {
            LOGE("copyTexture: formats differ (%d vs %d)", static_cast<int>(src_it->second.format),
                 static_cast<int>(dst_it->second.format));
            return;
        }
        if (src_it->second.type == VK_IMAGE_TYPE_3D || dst_it->second.type == VK_IMAGE_TYPE_3D) {
            LOGE("copyTexture: 3D images are not supported by this path");
            return;
        }
        if (!pImpl->can_rewrite_locked(dst_it->second)) {
            return;
        }
        src_record = src_it->second;
        dst_record = dst_it->second;
    }

    const FormatInfo& src_info = format_info_for(src_record.format);
    const FormatInfo& dst_info = format_info_for(dst_record.format);
    const VkExtent3D src_extent =
        compute_mip_extent(src_record.width, src_record.height, 1, src_mip, src_info);
    const VkExtent3D dst_extent =
        compute_mip_extent(dst_record.width, dst_record.height, 1, dst_mip, dst_info);
    if (src_extent.width != dst_extent.width || src_extent.height != dst_extent.height) {
        LOGE("copyTexture: mip extents differ (%u x %u vs %u x %u)", src_extent.width,
             src_extent.height, dst_extent.width, dst_extent.height);
        return;
    }

    const VkImageLayout src_target = shader_read_layout(src_info);
    const VkImageLayout dst_target = shader_read_layout(dst_info);

    std::lock_guard upload_lock(pImpl->upload_mutex);
    if (!pImpl->wait_for_upload_locked(kUploadWaitTimeoutNs)) {
        return;
    }
    if (!pImpl->ensure_transient_locked(0)) {
        return;
    }
    // The transfer views are 2D_ARRAY even for cube maps: vkCmdCopyImage forbids a
    // CUBE view of a cube-compatible image.
    const bool submitted = pImpl->submit_locked([&](VkCommandBuffer command_buffer) {
        pImpl->transition_image_locked(command_buffer, src_record.image, src_record.aspect,
                                       src_record.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       src_mip, 1, src_layer, 1);
        pImpl->transition_image_locked(command_buffer, dst_record.image, dst_record.aspect,
                                       dst_record.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       dst_mip, 1, dst_layer, 1);

        VkImageCopy region{};
        region.srcSubresource = VkImageSubresourceLayers{src_record.aspect, src_mip, src_layer, 1};
        region.srcOffset = VkOffset3D{0, 0, 0};
        region.srcExtent = src_extent;
        region.dstSubresource = VkImageSubresourceLayers{dst_record.aspect, dst_mip, dst_layer, 1};
        region.dstOffset = VkOffset3D{0, 0, 0};
        region.dstExtent = dst_extent;
        vkCmdCopyImage(command_buffer, src_record.transfer_view,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_record.transfer_view,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        pImpl->transition_image_locked(command_buffer, src_record.image, src_record.aspect,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src_target, src_mip,
                                       1, src_layer, 1);
        pImpl->transition_image_locked(command_buffer, dst_record.image, dst_record.aspect,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dst_target, dst_mip,
                                       1, dst_layer, 1);
    });

    if (submitted) {
        pImpl->set_layout(src, src_target);
        pImpl->set_layout(dst, dst_target);
        std::lock_guard lock(pImpl->mutex);
        auto src_it = pImpl->textures.find(src);
        auto dst_it = pImpl->textures.find(dst);
        // Both ends are referenced by pending GPU work now.
        if (src_it != pImpl->textures.end()) {
            pImpl->stamp_locked(src_it->second);
        }
        if (dst_it != pImpl->textures.end()) {
            pImpl->stamp_locked(dst_it->second);
        }
    }
}

// --- mip generation ---------------------------------------------------------

void VulkanTextureManager::onGenerateMipmaps(uint64_t handle) {
    // The fence wait comes first and holds upload_mutex for the whole job:
    // generateMipmaps() normally continues an upload this manager submitted a
    // moment earlier (uploadRawPixels() writes mip 0 and then asks for the
    // chain), and that copy must have retired before a chain is built on it. The
    // in-flight refusal that update/copy apply is therefore not used here - the
    // fence wait is the ordering guarantee instead, and holding upload_mutex
    // keeps another upload of this image from slipping in between. A frame that
    // the render thread submitted between the upload and this call could observe
    // a half-generated chain, which is why chains are generated at load time,
    // before the texture is published to the renderer.
    std::lock_guard upload_lock(pImpl->upload_mutex);
    if (!pImpl->wait_for_upload_locked(kUploadWaitTimeoutNs)) {
        return;
    }

    Impl::Record record;
    {
        std::lock_guard lock(pImpl->mutex);
        auto it = pImpl->textures.find(handle);
        if (it == pImpl->textures.end() || it->second.mip_levels <= 1) {
            return;
        }
        record = it->second;
    }

    const FormatInfo& info = format_info_for(record.format);
    if (info.depth_stencil) {
        LOGE("generateMipmaps: depth/stencil images are not supported");
        return;
    }
    if (info.compressed) {
        // Block compressed formats rarely advertise BLIT_SRC/BLIT_DST, and
        // re-tiling would need a shader pass. Documented refusal: ship the chain
        // in the asset instead of generating it here.
        LOGE("generateMipmaps: block compressed textures cannot be blitted; upload the chain");
        return;
    }
    if (!record.can_blit) {
        LOGE("generateMipmaps: the resolved format cannot be blitted (needs BLIT_SRC, BLIT_DST and "
             "SAMPLED_IMAGE_FILTER_LINEAR)");
        return;
    }

    const VkImageLayout target = shader_read_layout(info);
    const uint32_t layers = record.array_layers;
    const uint32_t levels = record.mip_levels;

    if (!pImpl->ensure_transient_locked(0)) {
        return;
    }

    // Synchronous on purpose. A loader thread needs the full chain and the final
    // layout in place before it continues, and the blits must not race a later
    // re-upload of mip 0. The cost is one pipeline stall per generated chain,
    // which is why this only happens at load time and never per frame.
    const bool submitted = pImpl->submit_and_wait_locked([&](VkCommandBuffer command_buffer) {
        // Level 0 holds the real contents and feeds the first blit; the levels
        // below it are undefined and are transitioned as fresh writes.
        pImpl->transition_image_locked(command_buffer, record.image, record.aspect, record.layout,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, layers);
        pImpl->transition_image_locked(command_buffer, record.image, record.aspect,
                                       VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, levels - 1, 0,
                                       layers);

        for (uint32_t level = 1; level < levels; ++level) {
            const uint32_t depth = record.type == VK_IMAGE_TYPE_3D ? record.depth : 1;
            // Source and destination are two different levels, so they have two
            // different extents; reusing one extent for both would sample only a
            // corner of the source.
            const VkExtent3D src_extent = compute_mip_extent(record.width, record.height, depth,
                                                            level - 1, info);
            const VkExtent3D dst_extent = compute_mip_extent(record.width, record.height, depth,
                                                            level, info);
            VkImageBlit blit{};
            // The transfer view spans every level, so the level is selected here
            // rather than through a per-level view.
            blit.srcSubresource = VkImageSubresourceLayers{record.aspect, level - 1, 0, layers};
            blit.srcOffsets[0] = VkOffset3D{0, 0, 0};
            blit.srcOffsets[1] = VkOffset3D{static_cast<int32_t>(src_extent.width),
                                            static_cast<int32_t>(src_extent.height),
                                            static_cast<int32_t>(src_extent.depth)};
            blit.dstSubresource = VkImageSubresourceLayers{record.aspect, level, 0, layers};
            blit.dstOffsets[0] = VkOffset3D{0, 0, 0};
            blit.dstOffsets[1] = VkOffset3D{static_cast<int32_t>(dst_extent.width),
                                            static_cast<int32_t>(dst_extent.height),
                                            static_cast<int32_t>(dst_extent.depth)};
            vkCmdBlitImage(command_buffer, record.transfer_view,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, record.transfer_view,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        }

        // Chain back to a layout the frame can sample.
        pImpl->transition_image_locked(command_buffer, record.image, record.aspect,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, 0, 1, 0,
                                       layers);
        pImpl->transition_image_locked(command_buffer, record.image, record.aspect,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, target, 1,
                                       levels - 1, 0, layers);
    });

    if (submitted) {
        pImpl->set_layout(handle, target);
        std::lock_guard lock(pImpl->mutex);
        auto it = pImpl->textures.find(handle);
        if (it != pImpl->textures.end()) {
            pImpl->stamp_locked(it->second);
        }
    }
}

// --- CPU pixel uploads ------------------------------------------------------

bool VulkanTextureManager::onLoadTextureFromMemory(uint64_t handle, const void* data, uint64_t size,
                                                   uint32_t format, bool generate_mipmaps) {
    // TextureManager::loadTextureFromMemory() hands over a bare pointer with no
    // dimensions, so an image cannot be sized from it. Compressed containers
    // additionally need a decoder (zlib inflate, JPEG IDCT, KTX2 supercompression)
    // that belongs in the loading thread, not in the renderer.
    if (looks_like_compressed_container(data, size)) {
        LOGE("loadTextureFromMemory: compressed containers (PNG/JPEG/KTX2/Basis/DDS) must be "
             "decoded by the loading thread; pass decoded pixels to uploadRawPixels()");
        return false;
    }
    LOGE("loadTextureFromMemory: raw pixels carry no extent, so no image can be created; use "
         "uploadRawPixels(pixels, size, width, height, format, mips)");
    (void)handle;
    (void)format;
    (void)generate_mipmaps;
    return false;
}

uint64_t VulkanTextureManager::uploadRawPixels(const void* pixels, uint64_t size, uint32_t width,
                                               uint32_t height, uint32_t format,
                                               bool generate_mipmaps, uint32_t array_layers) {
    if (pixels == nullptr || size == 0 || width == 0 || height == 0) {
        LOGE("uploadRawPixels: empty upload");
        return 0;
    }
    if (find_format(format) == nullptr) {
        LOGE("uploadRawPixels: unknown TextureFormat constant %u", format);
        return 0;
    }
    const uint32_t layers = array_layers == 0 ? 1 : array_layers;
    const uint64_t layer_size = mipByteSize(format, width, height, 1);
    if (size < layer_size * layers) {
        LOGE("uploadRawPixels: %llu bytes supplied, %llu required for %u layer(s) of %u x %u",
             static_cast<unsigned long long>(size),
             static_cast<unsigned long long>(layer_size * layers), layers, width, height);
        return 0;
    }

    // Public base methods, which is safe here: this function runs outside every
    // TextureManager hook, so no base lock is held. mip_levels 0 asks the base (and
    // then the backend) for a full chain.
    const uint32_t mip_levels = generate_mipmaps ? 0 : 1;
    const uint64_t handle =
        (layers > 1) ? TextureManager::createTextureArray(width, height, layers, format,
                                                           TextureUsage::SAMPLED, mip_levels)
                     : TextureManager::createTexture2D(width, height, format,
                                                        TextureUsage::SAMPLED, mip_levels);
    if (handle == 0) {
        return 0;
    }

    // Every layer is staged under one upload_mutex hold and through uploadRegion()
    // rather than through updateTexture(): the public path would refuse layer 1
    // because layer 0's own copy just made the image "in use by an unfinished
    // frame", and the image was created on this thread microseconds ago, so no
    // frame can be referencing it.
    const uint8_t* bytes = static_cast<const uint8_t*>(pixels);
    {
        std::lock_guard upload_lock(pImpl->upload_mutex);
        for (uint32_t layer = 0; layer < layers; ++layer) {
            Impl::Record record;
            {
                std::lock_guard lock(pImpl->mutex);
                auto it = pImpl->textures.find(handle);
                if (it == pImpl->textures.end()) {
                    LOGE("uploadRawPixels: texture %llu vanished mid-upload",
                         static_cast<unsigned long long>(handle));
                    return 0;
                }
                // Re-snapshotted per layer so the next copy starts from the layout
                // the previous one left behind.
                record = it->second;
            }
            if (!uploadRegion(record, 0, layer, 0, 0, 0, width, height, 1,
                              bytes + static_cast<size_t>(layer_size * layer), layer_size)) {
                return 0;
            }
        }
    }
    if (generate_mipmaps) {
        generateMipmaps(handle);
    }
    return handle;
}

// --- layout transitions for the rest of the backend -------------------------

bool VulkanTextureManager::transitionImage(uint64_t handle, VkImageLayout new_layout,
                                           uint32_t base_mip, uint32_t mip_count,
                                           uint32_t base_layer, uint32_t layer_count) {
    Impl::Record record;
    {
        std::lock_guard lock(pImpl->mutex);
        auto it = pImpl->textures.find(handle);
        if (it == pImpl->textures.end()) {
            return false;
        }
        record = it->second;
    }
    // 0 means "everything"; Vulkan wants a non-zero count.
    if (mip_count == 0) {
        mip_count = record.mip_levels;
    }
    if (layer_count == 0) {
        layer_count = record.array_layers;
    }
    if (record.layout == new_layout) {
        return true;
    }
    const VkImageLayout old_layout = record.layout;

    std::lock_guard upload_lock(pImpl->upload_mutex);
    if (!pImpl->wait_for_upload_locked(kUploadWaitTimeoutNs)) {
        return false;
    }
    if (!pImpl->ensure_transient_locked(0)) {
        return false;
    }
    const bool submitted = pImpl->submit_and_wait_locked([&](VkCommandBuffer command_buffer) {
        pImpl->transition_image_locked(command_buffer, record.image, record.aspect, old_layout,
                                       new_layout, base_mip, mip_count, base_layer, layer_count);
    });
    if (submitted) {
        // Only the tracked whole-image layout is updated: a caller that moved a
        // single level or layer owns the state of the rest from then on.
        if (base_mip == 0 && mip_count == record.mip_levels && base_layer == 0 &&
            layer_count == record.array_layers) {
            pImpl->set_layout(handle, new_layout);
        }
        std::lock_guard lock(pImpl->mutex);
        auto it = pImpl->textures.find(handle);
        if (it != pImpl->textures.end()) {
            pImpl->stamp_locked(it->second);
        }
    }
    return submitted;
}

void VulkanTextureManager::onSetTextureDebugName(uint64_t handle, const std::string& name) {
    const VkDevice device = renderer_ != nullptr ? renderer_->device() : VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    {
        std::lock_guard lock(pImpl->mutex);
        auto it = pImpl->textures.find(handle);
        if (it == pImpl->textures.end()) {
            return;
        }
        it->second.base.debug_name = name;
        image = it->second.image;
        view = it->second.view;
    }
    if (image == VK_NULL_HANDLE) {
        return;
    }
    pImpl->set_debug_name(device, VK_OBJECT_TYPE_IMAGE, handle_id(image), name);
    if (view != VK_NULL_HANDLE) {
        pImpl->set_debug_name(device, VK_OBJECT_TYPE_IMAGE_VIEW, handle_id(view), name);
    }
}

} // namespace copper
