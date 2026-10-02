#include "vulkan_buffer_manager.h"
#include "vulkan_renderer.h"

// VMA is compiled into vma_impl.cpp; this translation unit only needs its
// declarations. The allocator does not exist yet (VulkanRenderer::create_allocator()
// is never called), so allocate_and_bind() below is the single seam where the
// manual memory path is swapped for vmaAllocateMemory()/vmaBindBufferMemory().
#include <vk_mem_alloc.h>

#include <android/log.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <type_traits>
#include <unordered_map>
#include <utility>

#define LOG_TAG "CopperOxide-VKBuffer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

namespace {

// "This buffer was never handed to the GPU": no in-flight state to track.
constexpr uint64_t kNoFrame = UINT64_MAX;

// Uploads smaller than this share one staging block. Larger ones grow it.
constexpr VkDeviceSize kMinStagingBytes = 64 * 1024;

// vkCmdCopyBuffer requires 4-byte aligned offsets and a 4-byte multiple size.
constexpr VkDeviceSize kCopyAlignment = 4;

// Uploads are serialized on one fence, so a wedged GPU must not turn the wait
// into an unbounded hang. Five seconds is far beyond a healthy frame.
constexpr uint64_t kUploadWaitTimeoutNs = 5ull * 1000ull * 1000ull * 1000ull;

// One VkDeviceMemory block plus whether VMA or we own it. Keeping ownership in
// a single struct means the destroy path is one branch instead of two.
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
        // Map the whole allocation: flush/invalidate ranges then only need the
        // caller's offset, with no base-offset adjustment.
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

    // Frees the memory. The VkBuffer must already be gone. A null device means the
    // renderer already tore the device down: the handles are dropped without
    // touching the driver.
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

// Destroys `buffer` and the memory bound to it. VMA needs both the allocation
// handle and the buffer handle, so the two ownership models cannot be split.
void destroy_buffer_and_memory(VkDevice device, VmaAllocator allocator, VkBuffer buffer,
                               MemoryBlock& memory) {
    if (buffer == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
        memory.release(device, allocator);
        return;
    }
    if (memory.vma_owned) {
        if (allocator == VK_NULL_HANDLE) {
            // The allocator went away before the buffer: nothing can free it, so
            // the handles are dropped rather than handed to a null allocator.
            LOGE("cannot destroy a VMA-owned buffer without an allocator; leaking");
            memory.mapped = nullptr;
            memory.vma_allocation = VK_NULL_HANDLE;
            memory.memory = VK_NULL_HANDLE;
            memory.vma_owned = false;
            return;
        }
        vmaDestroyBuffer(allocator, buffer, memory.vma_allocation);
        // vmaDestroyBuffer() already unmapped anything we had mapped.
        memory.mapped = nullptr;
        memory.vma_allocation = VK_NULL_HANDLE;
        memory.memory = VK_NULL_HANDLE;
        memory.vma_owned = false;
        memory.release(device, allocator);
        return;
    }
    vkDestroyBuffer(device, buffer, nullptr);
    memory.release(device, allocator);
}

// Conservative availability/visibility barrier around a buffer copy. The
// destination is sampled by whatever pipeline uses it next and the manager has
// no pipeline layout to be precise about, so the barrier covers every stage
// instead of guessing. The cost is a wider wait than strictly necessary; the
// benefit is that the copy recorded here is visible to whatever the frame
// submits afterwards.
void record_buffer_barrier(VkCommandBuffer command_buffer, VkBuffer buffer) {
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 1, &barrier, 0,
                         nullptr);
}

// Reads the atomicity the device wants for non-coherent flush/invalidate.
VkDeviceSize non_coherent_alignment(VkPhysicalDevice physical) {
    if (physical == VK_NULL_HANDLE) {
        return 1;
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    return std::max<VkDeviceSize>(properties.limits.nonCoherentAtomSize, 1);
}

// VkBuffer is a dispatchable-style pointer on 64-bit ABIs and an integer handle
// on 32-bit ones, while VK_EXT_debug_utils always wants a uint64_t.
template <typename Handle>
uint64_t handle_id(Handle handle) {
    if constexpr (std::is_pointer_v<Handle>) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    } else {
        return static_cast<uint64_t>(handle);
    }
}

} // namespace

class VulkanBufferManager::Impl {
public:
    struct Record {
        uint64_t handle = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        MemoryBlock memory;
        uint32_t usage = 0;
        bool host_visible = false;
        bool host_coherent = false;
        void* mapped_ptr = nullptr;
        // Frame in which the GPU was last told to touch this buffer, or kNoFrame.
        uint64_t last_gpu_use_frame = kNoFrame;
        bool used_by_gpu = false;
        Buffer base;
    };

    explicit Impl(VulkanRenderer* renderer) : renderer(renderer) {}

    ~Impl() {
        destroy_transient_locked();
    }

    VulkanRenderer* renderer = nullptr;

    // Guards `buffers` only. Never held across vkQueueSubmit() or a device wait.
    std::unordered_map<uint64_t, Record> buffers;
    std::mutex mutex;

    // --- Transient upload resources -------------------------------------
    // A single staging buffer + command buffer + fence, serialized by
    // upload_mutex. Uploads therefore queue up behind each other instead of
    // racing, and the fence wait at the start of every upload is what makes
    // reuse safe. A ring of renderer.framesInFlight() slots would hide the
    // stall, but it needs the renderer's private fence ring.
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
                // Memory types never change for the lifetime of a physical
                // device, so one query is enough.
                vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);
                memory_properties_valid = true;
            }
        }
        return memory_props;
    }

    // required must hold, preferred is chosen when available, discouraged is
    // only used when nothing better exists (tiled mobile GPUs put DEVICE_LOCAL
    // and HOST_VISIBLE on the same heap, so "prefer" is a hint, not a filter).
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
            const VkMemoryPropertyFlags flags = props.memoryTypes[i].propertyFlags;
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

    // The single memory seam. Prefers VMA when VulkanRenderer exposes a live
    // allocator and falls back to vkAllocateMemory()/vkBindBufferMemory() on a
    // memory type satisfying `required`. Swapping VMA in later means editing
    // only this function.
    bool allocate_and_bind(VkBuffer buffer, uint32_t memory_type_bits,
                           VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
                           VkMemoryPropertyFlags discouraged, VkDeviceSize size, bool staging_like,
                           MemoryBlock& out) {
        const VkDevice device = this->device();
        if (device == VK_NULL_HANDLE || buffer == VK_NULL_HANDLE || size == 0) {
            return false;
        }

        const VmaAllocator allocator = this->allocator();
        if (allocator != VK_NULL_HANDLE) {
            // vmaAllocateMemory() takes the buffer's own VkMemoryRequirements, so
            // the size and the allowed memory types are handed over unchanged.
            // VmaAllocationCreateInfo stays at its defaults on purpose: VMA_MEMORY_USAGE_*
            // and VMA_ALLOCATION_CREATE_HOST_ACCESS_* were renamed between VMA releases,
            // and with memory requirements supplied the defaults already pick the right
            // block for both DEVICE_LOCAL and staging allocations.
            VkMemoryRequirements vk_requirements{};
            vk_requirements.size = size;
            vk_requirements.alignment = 1;
            vk_requirements.memoryTypeBits = memory_type_bits;
            VmaAllocationCreateInfo create_info{};

            VmaAllocation allocation = VK_NULL_HANDLE;
            VmaAllocationInfo info{};
            if (vmaAllocateMemory(allocator, &vk_requirements, &create_info, &allocation,
                                  &info) != VK_SUCCESS) {
                LOGE("vmaAllocateMemory failed for a %llu byte buffer",
                     static_cast<unsigned long long>(size));
                return false;
            }
            if (vmaBindBufferMemory(allocator, allocation, buffer) != VK_SUCCESS) {
                vmaFreeMemory(allocator, allocation);
                LOGE("vmaBindBufferMemory failed");
                return false;
            }

            out.vma_allocation = allocation;
            // VMA reports the backing VkDeviceMemory through VmaAllocationInfo::deviceMemory.
            out.memory = info.deviceMemory;
            out.vma_owned = true;
            out.size = size;
            // VMA chose the memory type, so report what a matching type can
            // offer: that is what decides whether mapBuffer() may succeed.
            const uint32_t type = find_memory_type(memory_type_bits, required, preferred, discouraged);
            const VkPhysicalDeviceMemoryProperties props = memory_properties();
            if (type != UINT32_MAX) {
                const VkMemoryPropertyFlags flags = props.memoryTypes[type].propertyFlags;
                out.host_visible = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
                out.host_coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            }
            return true;
        }

        const uint32_t type = find_memory_type(memory_type_bits, required, preferred, discouraged);
        if (type == UINT32_MAX) {
            LOGE("no memory type satisfies the request (required=0x%x preferred=0x%x)",
                 static_cast<unsigned>(required), static_cast<unsigned>(preferred));
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
        if (vkBindBufferMemory(device, buffer, out.memory, 0) != VK_SUCCESS) {
            LOGE("vkBindBufferMemory failed");
            vkFreeMemory(device, out.memory, nullptr);
            out.memory = VK_NULL_HANDLE;
            return false;
        }

        const VkPhysicalDeviceMemoryProperties props = memory_properties();
        const VkMemoryPropertyFlags flags = props.memoryTypes[type].propertyFlags;
        out.vma_owned = false;
        out.size = size;
        // Record what was granted, not what was asked: a device may hand out
        // HOST_VISIBLE without HOST_COHERENT, and then every host write needs an
        // explicit flush.
        out.host_visible = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        out.host_coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        if (staging_like && !out.host_visible) {
            LOGE("staging buffer got non-host-visible memory; uploads cannot work");
            return false;
        }
        return true;
    }

    // upload_mutex must be held. Callers must have waited on upload_fence_ before
    // calling this: the old staging memory may still be read by the GPU.
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

        MemoryBlock block;
        if (!allocate_and_bind(buffer, requirements.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, requirements.size,
                               /*staging_like=*/true, block)) {
            vkDestroyBuffer(device, buffer, nullptr);
            return false;
        }
        if (!block.map(device, allocator())) {
            LOGE("staging memory could not be mapped");
            destroy_buffer_and_memory(device, allocator(), buffer, block);
            return false;
        }

        staging_buffer = buffer;
        staging_memory = block;
        // Requirements can round the size up (alignment) and a mapped range
        // smaller than the copy is a validation error, so keep the real size.
        staging_size = std::max(size, requirements.size);
        return true;
    }

    // upload_mutex must be held.
    void destroy_staging_locked() {
        const VkDevice device = this->device();
        destroy_buffer_and_memory(device, allocator(), staging_buffer, staging_memory);
        staging_buffer = VK_NULL_HANDLE;
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
            LOGW("waiting for the upload fence returned %d", static_cast<int>(result));
            return false;
        }
        return true;
    }

    // upload_mutex must be held.
    bool ensure_transient_locked(VkDeviceSize min_staging_bytes) {
        const VkDevice device = this->device();
        const VkCommandPool pool = renderer != nullptr ? renderer->commandPool() : VK_NULL_HANDLE;
        if (device == VK_NULL_HANDLE || pool == VK_NULL_HANDLE) {
            LOGE("no device/command pool available for staging work");
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
        // The caller already waited on upload_fence_, so the previous staging
        // memory is idle and can be replaced outright.
        return grow_staging_locked(min_staging_bytes);
    }

    // upload_mutex must be held. Records into this manager's own command buffer
    // and submits it on the graphics queue with upload_fence_. Submissions to a
    // single queue execute in order, so the recorded work has run before the
    // frame that the render thread submits afterwards.
    //
    // This is a transient command buffer rather than the render thread's frame
    // command buffer on purpose: VulkanRenderer::onEndFrame() resets and
    // re-records command_buffers_[frame] from scratch every frame, and it is
    // only open for recording inside that function, so anything recorded from
    // outside would be discarded or recorded into a non-recording command buffer.
    bool submit_locked(const std::function<void(VkCommandBuffer)>& record) {
        const VkDevice device = this->device();
        const VkQueue queue = renderer != nullptr ? renderer->graphicsQueue() : VK_NULL_HANDLE;
        if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE ||
            upload_command_buffer == VK_NULL_HANDLE) {
            LOGE("transient submit requested without device/queue/command buffer");
            return false;
        }
        if (vkResetCommandBuffer(upload_command_buffer, 0) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(upload_command_buffer, &begin) != VK_SUCCESS) {
            LOGE("vkBeginCommandBuffer failed");
            return false;
        }
        record(upload_command_buffer);
        if (vkEndCommandBuffer(upload_command_buffer) != VK_SUCCESS) {
            LOGE("vkEndCommandBuffer failed");
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
            LOGW("vkQueueSubmit for an upload returned %d", static_cast<int>(result));
            return false;
        }
        return true;
    }

    // upload_mutex must be held. Host writes to non-coherent memory are only
    // visible to the device after a flush; coherent memory needs none. Offsets
    // and sizes must be multiples of nonCoherentAtomSize, so the range is rounded
    // up (and clamped to the allocation).
    void flush_staging_locked(VkDeviceSize bytes) {
        if (staging_memory.host_coherent || staging_memory.mapped == nullptr) {
            return;
        }
        const VkDeviceSize atom = non_coherent_alignment(
            renderer != nullptr ? renderer->physicalDevice() : VK_NULL_HANDLE);
        VkDeviceSize size = (bytes + atom - 1) & ~(atom - 1);
        size = std::min(size, staging_size);
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = staging_memory.memory;
        range.offset = 0;
        range.size = size;
        vkFlushMappedMemoryRanges(device(), 1, &range);
    }

    // Staging upload into a device-local buffer: memcpy into the staging block,
    // then vkCmdCopyBuffer on the transient command buffer.
    bool upload_to_buffer(VkBuffer dst, VkDeviceSize dst_offset, uint64_t buffer_size,
                          const void* data, VkDeviceSize size) {
        if (data == nullptr || size == 0) {
            return false;
        }
        // vkCmdCopyBuffer needs a 4-byte multiple size; the tail is zero filled.
        // An unaligned destination offset cannot be padded (that would read
        // memory in front of the caller's pointer), so it is refused loudly.
        if ((dst_offset % kCopyAlignment) != 0) {
            LOGW("buffer upload at offset %llu is not 4-byte aligned; refused",
                 static_cast<unsigned long long>(dst_offset));
            return false;
        }
        const VkDeviceSize copy_size = (size + (kCopyAlignment - 1)) & ~(kCopyAlignment - 1);
        if (dst_offset + copy_size > buffer_size) {
            LOGE("buffer upload runs past the end of the buffer");
            return false;
        }

        std::lock_guard lock(upload_mutex);
        if (!wait_for_upload_locked(kUploadWaitTimeoutNs)) {
            return false;
        }
        if (!ensure_transient_locked(copy_size)) {
            return false;
        }

        uint8_t* const staging = static_cast<uint8_t*>(staging_memory.mapped);
        if (staging == nullptr) {
            return false;
        }
        std::memcpy(staging, data, static_cast<size_t>(size));
        if (copy_size > size) {
            std::memset(staging + size, 0, static_cast<size_t>(copy_size - size));
        }
        flush_staging_locked(copy_size);

        const VkBuffer staging_handle = staging_buffer;
        const VkDeviceSize copy_offset = 0;
        const bool submitted = submit_locked([&](VkCommandBuffer command_buffer) {
            record_buffer_barrier(command_buffer, dst);
            VkBufferCopy region{};
            region.srcOffset = copy_offset;
            region.dstOffset = dst_offset;
            region.size = copy_size;
            vkCmdCopyBuffer(command_buffer, staging_handle, dst, 1, &region);
            // Also make the result visible to whatever samples the buffer next.
            record_buffer_barrier(command_buffer, dst);
        });
        if (submitted) {
            uploaded_bytes += size;
        }
        return submitted;
    }

    // vkCmdCopyBuffer between two buffers; both ends need transfer usage.
    bool copy_between_buffers(VkBuffer src, VkDeviceSize src_offset, uint64_t src_size, VkBuffer dst,
                              VkDeviceSize dst_offset, uint64_t dst_size, VkDeviceSize size) {
        if (size == 0) {
            return false;
        }
        if ((src_offset % kCopyAlignment) != 0 || (dst_offset % kCopyAlignment) != 0) {
            LOGW("buffer copy offsets must be 4-byte aligned; refused");
            return false;
        }
        const VkDeviceSize copy_size = (size + (kCopyAlignment - 1)) & ~(kCopyAlignment - 1);
        // The padded copy must still fit: the base class only validated the
        // unpadded size.
        if (src_offset + copy_size > src_size || dst_offset + copy_size > dst_size) {
            LOGE("buffer copy runs past the end of a buffer");
            return false;
        }

        std::lock_guard lock(upload_mutex);
        if (!wait_for_upload_locked(kUploadWaitTimeoutNs)) {
            return false;
        }
        if (!ensure_transient_locked(0)) {
            return false;
        }
        VkBufferCopy region{};
        region.srcOffset = src_offset;
        region.dstOffset = dst_offset;
        region.size = copy_size;
        return submit_locked([&](VkCommandBuffer command_buffer) {
            record_buffer_barrier(command_buffer, src);
            record_buffer_barrier(command_buffer, dst);
            vkCmdCopyBuffer(command_buffer, src, dst, 1, &region);
            record_buffer_barrier(command_buffer, dst);
        });
    }

    // mutex must be held. A frame submitted at index N has retired by the time
    // the renderer reports frame N + framesInFlight, because onBeginFrame() waits
    // on the fence of the frame slot it is about to reuse. Anything newer may
    // still be executing.
    bool in_flight_locked(const Record& record) const {
        if (record.last_gpu_use_frame == kNoFrame || renderer == nullptr) {
            return false;
        }
        const uint64_t frame = renderer->getFrameNumber();
        const uint64_t frames = std::max<uint32_t>(renderer->framesInFlight(), 1u);
        return record.last_gpu_use_frame + frames > frame;
    }

    // mutex must be held. Refuses (and logs) when the GPU may still be reading
    // the buffer's current contents: rewriting memory that a live frame reads
    // would produce a torn, non-deterministic frame. The caller stamps the
    // current frame with stamp_locked() only once the write actually happened,
    // so a failed upload does not block the next attempt.
    bool can_rewrite_locked(const Record& record) const {
        if (!in_flight_locked(record)) {
            return true;
        }
        LOGW("buffer %llu is still in use by an unfinished frame (stamped %llu, now %llu); "
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

    void mark_gpu_use(uint64_t handle) {
        std::lock_guard lock(mutex);
        auto it = buffers.find(handle);
        if (it != buffers.end()) {
            it->second.used_by_gpu = true;
        }
    }

    void set_debug_name(VkDevice device, VkObjectType type, uint64_t handle, const std::string& name) {
        if (device == VK_NULL_HANDLE || name.empty()) {
            return;
        }
        if (debug_name_fn == nullptr) {
            // VK_EXT_debug_utils is optional and Android does not link it, so the
            // entry point has to be fetched. A missing extension is not an error.
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

// --- usage / memory flag translation ---------------------------------------

VkBufferUsageFlags VulkanBufferManager::translateUsage(uint32_t usage) {
    VkBufferUsageFlags flags = 0;
    if (usage & BufferUsage::VERTEX)       flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (usage & BufferUsage::INDEX)        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (usage & BufferUsage::UNIFORM)      flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (usage & BufferUsage::STORAGE)      flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (usage & BufferUsage::TRANSFER_SRC) flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (usage & BufferUsage::TRANSFER_DST) flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (usage & BufferUsage::INDIRECT)     flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (flags == 0) {
        // A buffer with no usage is a caller bug; vertex is the least surprising
        // recovery and keeps vkCreateBuffer() valid.
        flags = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    return flags;
}

VkMemoryPropertyFlags VulkanBufferManager::translateMemoryFlags(uint32_t memory_flags) {
    VkMemoryPropertyFlags flags = 0;
    if (memory_flags & BufferMemory::HOST_VISIBLE)  flags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    if (memory_flags & BufferMemory::HOST_COHERENT) flags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (memory_flags & BufferMemory::DEVICE_LOCAL)  flags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (memory_flags & BufferMemory::STAGING) {
        // STAGING is the "give me a host-visible, coherent upload buffer" request.
        flags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    if (memory_flags & BufferMemory::PREFER_DEVICE_LOCAL) {
        flags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }
    return flags;
}

VkMemoryPropertyFlags VulkanBufferManager::defaultMemoryFlags(uint32_t usage, uint32_t memory_flags) {
    if (memory_flags != BufferMemory::NONE) {
        return translateMemoryFlags(memory_flags);
    }
    // Default policy: uniform buffers are rewritten by the CPU every frame, so
    // they must be mappable or every update turns into a staging copy. Anything
    // else is device-local and written through a staging copy.
    if (usage & BufferUsage::UNIFORM) {
        return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
}

// --- construction ----------------------------------------------------------

VulkanBufferManager::VulkanBufferManager(VulkanRenderer* renderer)
    : pImpl(std::make_unique<Impl>(renderer)), renderer_(renderer) {
    if (renderer_ == nullptr) {
        LOGE("VulkanBufferManager built without a renderer; every call will fail");
    }
}

VulkanBufferManager::~VulkanBufferManager() {
    releaseDeviceResources();
}

void VulkanBufferManager::releaseDeviceResources() {
    if (!pImpl) {
        return;
    }
    std::lock_guard lock(pImpl->upload_mutex);
    pImpl->destroy_transient_locked();
}

// --- backend queries -------------------------------------------------------

VkBuffer VulkanBufferManager::vkBuffer(uint64_t handle) const {
    std::lock_guard lock(pImpl->mutex);
    auto it = pImpl->buffers.find(handle);
    return it == pImpl->buffers.end() ? VK_NULL_HANDLE : it->second.buffer;
}

bool VulkanBufferManager::isMapped(uint64_t handle) const {
    std::lock_guard lock(pImpl->mutex);
    auto it = pImpl->buffers.find(handle);
    return it != pImpl->buffers.end() && it->second.mapped_ptr != nullptr;
}

bool VulkanBufferManager::isInUseByGpu(uint64_t handle) const {
    std::lock_guard lock(pImpl->mutex);
    auto it = pImpl->buffers.find(handle);
    return it != pImpl->buffers.end() && pImpl->in_flight_locked(it->second);
}

bool VulkanBufferManager::waitForUploads(uint64_t timeout_ns) {
    std::lock_guard lock(pImpl->upload_mutex);
    return pImpl->wait_for_upload_locked(timeout_ns);
}

uint64_t VulkanBufferManager::getUploadedBytes() const {
    std::lock_guard lock(pImpl->upload_mutex);
    return pImpl->uploaded_bytes;
}

const VulkanBufferManager::Buffer* VulkanBufferManager::getBuffer(uint64_t handle) const {
    std::lock_guard lock(pImpl->mutex);
    auto it = pImpl->buffers.find(handle);
    return it == pImpl->buffers.end() ? nullptr : &it->second.base;
}

uint64_t VulkanBufferManager::getBufferSize(uint64_t handle) const {
    std::lock_guard lock(pImpl->mutex);
    auto it = pImpl->buffers.find(handle);
    return it == pImpl->buffers.end() ? 0 : it->second.base.size;
}

// --- creation / destruction ------------------------------------------------

bool VulkanBufferManager::onCreateBuffer(uint64_t handle, uint64_t size, uint32_t usage,
                                         uint32_t memory_flags) {
    const VkDevice device = renderer_ != nullptr ? renderer_->device() : VK_NULL_HANDLE;
    if (device == VK_NULL_HANDLE) {
        LOGE("onCreateBuffer without a Vulkan device");
        return false;
    }
    if (size == 0) {
        LOGE("buffer %llu has size 0", static_cast<unsigned long long>(handle));
        return false;
    }
    if (usage == BufferUsage::NONE) {
        LOGW("buffer %llu has no usage bits; treating it as a vertex buffer",
             static_cast<unsigned long long>(handle));
    }

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = translateUsage(usage);
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS) {
        LOGE("vkCreateBuffer(%llu) failed", static_cast<unsigned long long>(size));
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);

    Impl::Record record;
    record.handle = handle;
    record.buffer = buffer;
    record.usage = usage;
    const VkMemoryPropertyFlags requested = defaultMemoryFlags(usage, memory_flags);
    if (!pImpl->allocate_and_bind(buffer, requirements.memoryTypeBits, requested,
                                  requested & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                  requirements.size, /*staging_like=*/false, record.memory)) {
        vkDestroyBuffer(device, buffer, nullptr);
        return false;
    }

    record.host_visible = record.memory.host_visible;
    record.host_coherent = record.memory.host_coherent;
    record.base.handle = handle;
    record.base.size = size;
    record.base.usage = usage;
    record.base.memory_flags = memory_flags;
    // Never on the GPU yet, so there is no in-flight state to protect.
    record.last_gpu_use_frame = kNoFrame;

    std::lock_guard lock(pImpl->mutex);
    pImpl->buffers[handle] = record;
    LOGI("buffer %llu: %llu bytes usage=0x%x%s", static_cast<unsigned long long>(handle),
         static_cast<unsigned long long>(size), usage,
         record.host_visible ? " (host visible)" : "");
    return true;
}

void VulkanBufferManager::onDestroyBuffer(uint64_t handle) {
    // Take the record out under the lock and only then talk to the driver: the
    // map lock is never held across a wait.
    Impl::Record record;
    {
        std::lock_guard lock(pImpl->mutex);
        auto it = pImpl->buffers.find(handle);
        if (it == pImpl->buffers.end()) {
            return;
        }
        record = std::move(it->second);
        pImpl->buffers.erase(it);
    }

    const VkDevice device = renderer_ != nullptr ? renderer_->device() : VK_NULL_HANDLE;
    if (device == VK_NULL_HANDLE) {
        return;
    }
    if (record.used_by_gpu) {
        // vkDeviceWaitIdle() drains the whole device, including the swapchain and
        // every other manager, so it is reserved for teardown (level changes,
        // framebuffer rebuilds, shutdown) and never used in steady state. The
        // per-frame fence ring would be the cheap alternative, but
        // VulkanRenderer keeps in_flight_fences_ private; add an
        // `inFlightFences()` accessor if teardown ever shows up in a profile.
        vkDeviceWaitIdle(device);
    }
    destroy_buffer_and_memory(device, renderer_->allocator(), record.buffer, record.memory);
}

// --- mapping ---------------------------------------------------------------

void* VulkanBufferManager::onMapBuffer(uint64_t handle, uint64_t offset, uint64_t size) {
    const VkDevice device = renderer_ != nullptr ? renderer_->device() : VK_NULL_HANDLE;
    if (device == VK_NULL_HANDLE) {
        return nullptr;
    }
    std::lock_guard lock(pImpl->mutex);
    auto it = pImpl->buffers.find(handle);
    if (it == pImpl->buffers.end()) {
        return nullptr;
    }
    Impl::Record& record = it->second;
    if (offset > record.base.size || (size != 0 && size > record.base.size - offset)) {
        LOGE("mapBuffer(%llu) range is outside the buffer",
             static_cast<unsigned long long>(handle));
        return nullptr;
    }
    if (!record.host_visible) {
        // Documented failure, not a silent one: device-local memory cannot be
        // mapped. Callers that need direct CPU access must create the buffer with
        // BufferMemory::HOST_VISIBLE (or BufferUsage::UNIFORM, which defaults to
        // host-visible memory).
        LOGW("mapBuffer(%llu) refused: the buffer lives in device-local memory",
             static_cast<unsigned long long>(handle));
        return nullptr;
    }
    if (!record.memory.map(device, renderer_->allocator())) {
        LOGE("vkMapMemory failed for buffer %llu", static_cast<unsigned long long>(handle));
        return nullptr;
    }
    record.mapped_ptr = record.memory.mapped;
    // The whole allocation is mapped, so the caller's offset is simply added.
    return static_cast<uint8_t*>(record.memory.mapped) + offset;
}

void VulkanBufferManager::onUnmapBuffer(uint64_t handle) {
    const VkDevice device = renderer_ != nullptr ? renderer_->device() : VK_NULL_HANDLE;
    if (device == VK_NULL_HANDLE) {
        return;
    }
    MemoryBlock memory;
    {
        std::lock_guard lock(pImpl->mutex);
        auto it = pImpl->buffers.find(handle);
        if (it == pImpl->buffers.end()) {
            return;
        }
        if (!it->second.host_coherent && it->second.mapped_ptr != nullptr) {
            // Best effort: flush everything the caller could have written. On
            // coherent memory this would be a no-op, so it is skipped there.
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = it->second.memory.memory;
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            vkFlushMappedMemoryRanges(device, 1, &range);
        }
        memory = it->second.memory;
        it->second.memory.mapped = nullptr;
        it->second.mapped_ptr = nullptr;
    }
    // Unmapping can be expensive, so it happens outside the lock.
    memory.unmap(device, renderer_->allocator());
}

void VulkanBufferManager::onFlushBuffer(uint64_t handle, uint64_t offset, uint64_t size) {
    const VkDevice device = renderer_ != nullptr ? renderer_->device() : VK_NULL_HANDLE;
    if (device == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard lock(pImpl->mutex);
    auto it = pImpl->buffers.find(handle);
    if (it == pImpl->buffers.end() || it->second.mapped_ptr == nullptr) {
        return;
    }
    if (it->second.host_coherent) {
        // Coherent memory needs no flush; skipping it keeps the per-frame
        // uniform-buffer path free of driver calls.
        return;
    }
    const VkDeviceSize alignment = non_coherent_alignment(renderer_->physicalDevice());
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = it->second.memory.memory;
    if (size == 0) {
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
    } else {
        // Non-coherent ranges must be aligned outwards on both ends.
        const VkDeviceSize begin = std::min(offset & ~(alignment - 1), it->second.base.size);
        const VkDeviceSize end =
            std::min((offset + size + alignment - 1) & ~(alignment - 1), it->second.base.size);
        range.offset = begin;
        range.size = (end > begin) ? (end - begin) : VK_WHOLE_SIZE;
    }
    const VkResult result = vkFlushMappedMemoryRanges(device, 1, &range);
    if (result != VK_SUCCESS) {
        LOGW("vkFlushMappedMemoryRanges returned %d", static_cast<int>(result));
    }
}

void VulkanBufferManager::onInvalidateBuffer(uint64_t handle, uint64_t offset, uint64_t size) {
    const VkDevice device = renderer_ != nullptr ? renderer_->device() : VK_NULL_HANDLE;
    if (device == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard lock(pImpl->mutex);
    auto it = pImpl->buffers.find(handle);
    if (it == pImpl->buffers.end() || it->second.mapped_ptr == nullptr || it->second.host_coherent) {
        return;
    }
    const VkDeviceSize alignment = non_coherent_alignment(renderer_->physicalDevice());
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = it->second.memory.memory;
    if (size == 0) {
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
    } else {
        const VkDeviceSize begin = std::min(offset & ~(alignment - 1), it->second.base.size);
        const VkDeviceSize end =
            std::min((offset + size + alignment - 1) & ~(alignment - 1), it->second.base.size);
        range.offset = begin;
        range.size = (end > begin) ? (end - begin) : VK_WHOLE_SIZE;
    }
    const VkResult result = vkInvalidateMappedMemoryRanges(device, 1, &range);
    if (result != VK_SUCCESS) {
        LOGW("vkInvalidateMappedMemoryRanges returned %d", static_cast<int>(result));
    }
}

// --- updates ---------------------------------------------------------------

void VulkanBufferManager::onUpdateBuffer(uint64_t handle, uint64_t offset, const void* data,
                                         uint64_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
    // Check and copy the record out under the lock, then release it: the upload
    // submit below must not run with the map lock held.
    Impl::Record record;
    {
        std::lock_guard lock(pImpl->mutex);
        auto it = pImpl->buffers.find(handle);
        if (it == pImpl->buffers.end()) {
            return;
        }
        if (!pImpl->can_rewrite_locked(it->second)) {
            return;
        }
        record = it->second;
    }

    if (record.host_visible) {
        const VkDevice device = renderer_ != nullptr ? renderer_->device() : VK_NULL_HANDLE;
        if (device == VK_NULL_HANDLE) {
            return;
        }
        uint8_t* mapped = nullptr;
        {
            std::lock_guard lock(pImpl->mutex);
            auto it = pImpl->buffers.find(handle);
            if (it == pImpl->buffers.end()) {
                return;
            }
            if (it->second.memory.mapped == nullptr) {
                // updateBuffer() without an explicit mapBuffer(): map on demand so
                // the per-frame uniform path needs no begin/end pairs. The mapping
                // is kept for the buffer's lifetime.
                if (!it->second.memory.map(device, renderer_->allocator())) {
                    LOGE("buffer %llu could not be mapped for update",
                         static_cast<unsigned long long>(handle));
                    return;
                }
                it->second.mapped_ptr = it->second.memory.mapped;
            }
            mapped = static_cast<uint8_t*>(it->second.memory.mapped);
            pImpl->stamp_locked(it->second);
        }
        std::memcpy(mapped + offset, data, static_cast<size_t>(size));
        if (!record.host_coherent) {
            // The caller wrote through a raw pointer without flushBuffer(), so the
            // manager flushes the dirty range itself.
            const VkDeviceSize alignment = non_coherent_alignment(renderer_->physicalDevice());
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = record.memory.memory;
            const VkDeviceSize begin = std::min(offset & ~(alignment - 1), record.base.size);
            const VkDeviceSize end =
                std::min((offset + size + alignment - 1) & ~(alignment - 1), record.base.size);
            range.offset = begin;
            range.size = (end > begin) ? (end - begin) : VK_WHOLE_SIZE;
            vkFlushMappedMemoryRanges(device, 1, &range);
        }
        return;
    }

    // Device-local memory: stage the bytes and let the GPU copy them in.
    if (pImpl->upload_to_buffer(record.buffer, offset, record.base.size, data, size)) {
        std::lock_guard lock(pImpl->mutex);
        auto it = pImpl->buffers.find(handle);
        if (it != pImpl->buffers.end()) {
            pImpl->stamp_locked(it->second);
        }
    }
}

void VulkanBufferManager::onCopyBuffer(uint64_t src, uint64_t dst, uint64_t size,
                                       uint64_t src_offset, uint64_t dst_offset) {
    if (size == 0) {
        return;
    }
    Impl::Record src_record;
    Impl::Record dst_record;
    {
        std::lock_guard lock(pImpl->mutex);
        auto src_it = pImpl->buffers.find(src);
        auto dst_it = pImpl->buffers.find(dst);
        if (src_it == pImpl->buffers.end() || dst_it == pImpl->buffers.end()) {
            return;
        }
        // vkCmdCopyBuffer is only valid when both ends declared transfer usage.
        if (!(src_it->second.usage & (BufferUsage::TRANSFER_SRC | BufferUsage::STORAGE))) {
            LOGW("copyBuffer: source %llu lacks TRANSFER_SRC usage",
                 static_cast<unsigned long long>(src));
            return;
        }
        if (!(dst_it->second.usage & (BufferUsage::TRANSFER_DST | BufferUsage::STORAGE))) {
            LOGW("copyBuffer: destination %llu lacks TRANSFER_DST usage",
                 static_cast<unsigned long long>(dst));
            return;
        }
        if (!pImpl->can_rewrite_locked(dst_it->second)) {
            return;
        }
        src_record = src_it->second;
        dst_record = dst_it->second;
    }

    if (pImpl->copy_between_buffers(src_record.buffer, src_offset, src_record.base.size,
                                    dst_record.buffer, dst_offset, dst_record.base.size, size)) {
        std::lock_guard lock(pImpl->mutex);
        auto src_it = pImpl->buffers.find(src);
        auto dst_it = pImpl->buffers.find(dst);
        if (dst_it != pImpl->buffers.end()) {
            pImpl->stamp_locked(dst_it->second);
        }
        // The source is now read by pending GPU work too, so its teardown has to
        // wait for the fence just like the destination's.
        if (src_it != pImpl->buffers.end()) {
            src_it->second.used_by_gpu = true;
        }
    }
}

void VulkanBufferManager::onSetBufferDebugName(uint64_t handle, const std::string& name) {
    const VkDevice device = renderer_ != nullptr ? renderer_->device() : VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    {
        std::lock_guard lock(pImpl->mutex);
        auto it = pImpl->buffers.find(handle);
        if (it == pImpl->buffers.end()) {
            return;
        }
        it->second.base.debug_name = name;
        buffer = it->second.buffer;
    }
    if (buffer != VK_NULL_HANDLE) {
        pImpl->set_debug_name(device, VK_OBJECT_TYPE_BUFFER, handle_id(buffer), name);
    }
}

} // namespace copper
