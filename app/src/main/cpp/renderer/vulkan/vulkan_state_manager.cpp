#include "vulkan_state_manager.h"

// The accessors used below (device(), renderPass(), framesInFlight()) are the
// ones the integrator must add to VulkanRenderer; see the INTEGRATOR NOTE in the
// header.
#include "vulkan_renderer.h"

#include <android/log.h>

#include <string>
#include <utility>
#include <vector>

#define LOG_TAG "CopperOxide-VK"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

namespace copper {

namespace {

constexpr uint32_t k_max_vertex_bindings = 16;
constexpr uint32_t k_max_descriptor_sets = 32;

// The base passes the index element size as a plain uint32_t with no shared
// enum, so the mapping is fixed here. Anything unknown degrades to 16 bit (the
// base's default value) and is logged once: binding the wrong index width is a
// validation error, but silently dropping the binding would hide the bug.
VkIndexType to_index_type(uint32_t index_type) {
    switch (index_type) {
        case VulkanStateManager::k_index_type_uint16: return VK_INDEX_TYPE_UINT16;
        case VulkanStateManager::k_index_type_uint32: return VK_INDEX_TYPE_UINT32;
        default: return VK_INDEX_TYPE_UINT16;
    }
}

} // namespace

VulkanStateManager::VulkanStateManager(VulkanRenderer* renderer) : renderer_(renderer) {}

VulkanStateManager::~VulkanStateManager() {
    // Nothing of ours is owned on the device: pipelines, buffers, descriptor
    // sets and framebuffers all belong to the managers that created them. The
    // only thing worth dropping is the recorded command buffer, so a hook that
    // somehow runs after shutdown cannot record into a command buffer whose pool
    // is already destroyed.
    std::lock_guard<std::mutex> lock(mutex_);
    command_buffer_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
}

void VulkanStateManager::setBufferResolver(BufferResolver resolver) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_resolver_ = std::move(resolver);
}

void VulkanStateManager::setPipelineResolver(PipelineResolver resolver) {
    std::lock_guard<std::mutex> lock(mutex_);
    pipeline_resolver_ = std::move(resolver);
}

void VulkanStateManager::setDescriptorSetResolver(DescriptorSetResolver resolver) {
    std::lock_guard<std::mutex> lock(mutex_);
    descriptor_set_resolver_ = std::move(resolver);
}

void VulkanStateManager::setFramebufferResolver(FramebufferResolver resolver) {
    std::lock_guard<std::mutex> lock(mutex_);
    framebuffer_resolver_ = std::move(resolver);
}

void VulkanStateManager::setDynamicRangeCountResolver(DynamicRangeCountResolver resolver) {
    std::lock_guard<std::mutex> lock(mutex_);
    dynamic_range_count_resolver_ = std::move(resolver);
}

void VulkanStateManager::setCommandBuffer(VkCommandBuffer command_buffer, uint32_t frame_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    command_buffer_ = command_buffer;
    frame_index_ = frame_index;
    if (command_buffer != VK_NULL_HANDLE && renderer_ != nullptr && !frame_index_logged_) {
        const uint32_t frames_in_flight = renderer_->framesInFlight();
        // WHY check at all: the frame index only ever reaches the log messages
        // here, so an out-of-range value is not a crash -- but it is always a
        // wiring bug (passing the swapchain image index instead of the frame
        // slot), and it is much cheaper to spot here than in a driver crash.
        if (frames_in_flight != 0 && frame_index >= frames_in_flight) {
            frame_index_logged_ = true;
            LOGW("setCommandBuffer: frame index %u is outside frames_in_flight (%u); pass "
                 "the frames-in-flight slot, not the swapchain image index",
                 frame_index, frames_in_flight);
        }
    }
}

bool VulkanStateManager::hasCommandBuffer() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return command_buffer_ != VK_NULL_HANDLE;
}

uint32_t VulkanStateManager::frameIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frame_index_;
}

uint32_t VulkanStateManager::recordedTopology() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return topology_;
}

VkPipelineLayout VulkanStateManager::bound_pipeline_layout() const {
    return pipeline_layout_;
}

uint32_t VulkanStateManager::dynamic_range_count(uint64_t descriptor_set_handle,
                                                 uint32_t set_index) const {
    if (dynamic_range_count_resolver_) {
        return dynamic_range_count_resolver_(descriptor_set_handle, set_index);
    }
    // Documented fallback: one offset per bound set. It is only correct for
    // layouts where every set has exactly one dynamic buffer (Copper Oxide's
    // current camera/material layouts do), and the consistency check in
    // onBindDescriptorSets refuses the bind when the flat array does not match.
    if (!no_dynamic_range_resolver_logged_) {
        no_dynamic_range_resolver_logged_ = true;
        LOGW("no dynamic range count resolver installed: assuming one dynamic offset "
             "per bound descriptor set");
    }
    return 1;
}

void VulkanStateManager::onBindPipeline(uint64_t pipeline) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (command_buffer_ == VK_NULL_HANDLE) {
        return;
    }

    VulkanPipelineBinding binding;
    if (pipeline_resolver_) {
        binding = pipeline_resolver_(pipeline);
    } else if (!no_pipeline_resolver_logged_) {
        no_pipeline_resolver_logged_ = true;
        LOGW("no pipeline resolver installed: bindPipeline(%llu) records nothing",
             static_cast<unsigned long long>(pipeline));
    }

    if (binding.pipeline == VK_NULL_HANDLE) {
        // WHY refuse instead of recording VK_NULL_HANDLE: vkCmdBindPipeline with
        // a null pipeline is a hard validation error, and a stale pipeline left
        // bound draws with the wrong shader -- which is at least visible.
        LOGW("onBindPipeline: pipeline %llu did not resolve, keeping the previous "
             "pipeline bound",
             static_cast<unsigned long long>(pipeline));
        return;
    }

    // Graphics only: StateManager has no compute bind point, and the pipelines
    // VulkanShaderManager builds for a render pass are graphics pipelines.
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, binding.pipeline);
    // Cache the layout: vkCmdBindDescriptorSets needs it later in the same
    // applyState() round and the hook signature does not carry the pipeline.
    pipeline_layout_ = binding.layout;
}

void VulkanStateManager::onBindVertexBuffers(const std::array<uint64_t, k_max_vertex_bindings>& buffers,
                                             const std::array<uint32_t, k_max_vertex_bindings>& offsets) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (command_buffer_ == VK_NULL_HANDLE) {
        return;
    }

    // Slot -> (buffer, byte offset) table in ascending slot order.
    //
    // WHY the offsets are passed through unchanged: StateManager::bindVertexBuffer
    // stores an absolute byte offset into ITS OWN buffer, and Vulkan's
    // pOffsets is likewise a byte offset into the buffer of the same binding --
    // so no `binding * k_vertex_stride` term is added. The per-vertex advance
    // inside a buffer is k_vertex_stride (VulkanVertexLayout::k_stride), but that
    // value is baked into the pipeline's vertex input binding description and
    // must never be re-derived here or the two could drift apart.
    std::array<VkBuffer, k_max_vertex_bindings> vk_buffers{};
    std::array<VkDeviceSize, k_max_vertex_bindings> vk_offsets{};
    uint32_t count = 0;
    uint32_t first_binding = 0;

    for (uint32_t binding = 0; binding < k_max_vertex_bindings; ++binding) {
        if (buffers[binding] == 0) {
            // Unbound slot: Vulkan keeps whatever was bound to it before, which
            // is what the caller means by "not bound here".
            continue;
        }
        if (count == 0) {
            first_binding = binding;
        }
        if (!buffer_resolver_) {
            // Documented fallback: record the slot with a null buffer so the
            // draw cannot silently read a stale binding, and say so once.
            if (!no_buffer_resolver_logged_) {
                no_buffer_resolver_logged_ = true;
                LOGW("no buffer resolver installed: binding %u records VK_NULL_HANDLE",
                     binding);
            }
            vk_buffers[count] = VK_NULL_HANDLE;
        } else {
            vk_buffers[count] = buffer_resolver_(buffers[binding]);
            if (vk_buffers[count] == VK_NULL_HANDLE) {
                // Dropping the entry is deliberate: a resolved-null buffer would
                // be recorded as VK_NULL_HANDLE (a validation error), while
                // dropping it leaves the previous binding, which validation and
                // debug names make obvious.
                LOGW("onBindVertexBuffers: buffer %llu (binding %u) did not resolve, "
                     "leaving that binding untouched",
                     static_cast<unsigned long long>(buffers[binding]), binding);
                continue;
            }
        }
        vk_offsets[count] = static_cast<VkDeviceSize>(offsets[binding]);
        ++count;
    }

    if (count == 0) {
        // Nothing bound at all: leave the previous binding alone rather than
        // recording an empty call (firstBinding with count 0 is legal but says
        // nothing useful here).
        return;
    }

    // ONE call for every binding: Vulkan takes the whole table at once, and
    // starting at first_binding keeps the slot -> buffer mapping intact even
    // when a lower slot is unbound.
    vkCmdBindVertexBuffers(command_buffer_, first_binding, count, vk_buffers.data(),
                           vk_offsets.data());
}

void VulkanStateManager::onBindIndexBuffer(uint64_t buffer, uint32_t index_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (command_buffer_ == VK_NULL_HANDLE) {
        return;
    }

    const VkIndexType vulkan_index_type = to_index_type(index_type);
    if (index_type != VulkanStateManager::k_index_type_uint16 &&
        index_type != VulkanStateManager::k_index_type_uint32 && !unknown_index_type_logged_) {
        unknown_index_type_logged_ = true;
        LOGW("onBindIndexBuffer: unknown index type %u, assuming 16 bit indices",
             index_type);
    }

    VkBuffer index_buffer = VK_NULL_HANDLE;
    if (buffer_resolver_) {
        index_buffer = buffer_resolver_(buffer);
        if (index_buffer == VK_NULL_HANDLE && buffer != 0) {
            LOGW("onBindIndexBuffer: buffer %llu did not resolve, binding VK_NULL_HANDLE",
                 static_cast<unsigned long long>(buffer));
        }
    } else if (buffer != 0 && !no_buffer_resolver_logged_) {
        no_buffer_resolver_logged_ = true;
        LOGW("no buffer resolver installed: bindIndexBuffer records VK_NULL_HANDLE");
    }

    // WHY binding the null buffer is fine here (unlike the pipeline / vertex
    // buffer cases): vkCmdBindIndexBuffer explicitly allows VK_NULL_BUFFER,
    // which resets the index binding, so an unresolved or never-bound index
    // buffer is recorded honestly instead of leaving a stale one in place.
    vkCmdBindIndexBuffer(command_buffer_, index_buffer, 0, vulkan_index_type);
}

void VulkanStateManager::onBindDescriptorSets(
        const std::array<uint64_t, k_max_descriptor_sets>& descriptor_sets,
        const std::vector<uint32_t>& dynamic_offsets) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (command_buffer_ == VK_NULL_HANDLE) {
        return;
    }

    // 1) The descriptor sets themselves.
    //
    // Vulkan binds a CONTIGUOUS range [firstSet, firstSet + count): the layout
    // of every set in that range comes from the bound pipeline layout, so a set
    // that fails to resolve cannot simply be skipped -- the call is abandoned
    // instead, which leaves the previous binding in place.
    std::vector<VkDescriptorSet> sets;
    std::vector<uint32_t> slot_indices;  // base slots behind sets[]
    uint32_t first_set = 0;
    for (uint32_t slot = 0; slot < k_max_descriptor_sets; ++slot) {
        if (descriptor_sets[slot] == 0) {
            continue;
        }
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        if (descriptor_set_resolver_) {
            descriptor_set = descriptor_set_resolver_(descriptor_sets[slot]);
        } else if (!no_descriptor_set_resolver_logged_) {
            no_descriptor_set_resolver_logged_ = true;
            LOGW("no descriptor set resolver installed: bindDescriptorSet records nothing");
        }
        if (descriptor_set == VK_NULL_HANDLE) {
            LOGW("onBindDescriptorSets: set %u (handle %llu) did not resolve; the whole "
                 "bind is skipped so the sets stay contiguous",
                 slot, static_cast<unsigned long long>(descriptor_sets[slot]));
            return;
        }
        if (sets.empty()) {
            first_set = slot;
        }
        sets.push_back(descriptor_set);
        slot_indices.push_back(slot);
    }
    if (sets.empty()) {
        return;
    }

    // 2) The dynamic offsets.
    //
    // WHY the explicit concatenation: Vulkan has ONE pDynamicOffsets array whose
    // length is the sum of the dynamicOffsetCounts of the sets in the range --
    // there is no per-set array to pass and no per-set length field. The base
    // already handed us the offsets of all 32 slots concatenated in ascending
    // slot order, but it dropped the per-slot lengths on the floor (a flat
    // array "silently dropped every offset whose count was not exactly 32"),
    // so the walk below re-slices that flat vector with each set's dynamic range
    // count and re-checks that the two arrays agree end to end.
    std::vector<uint32_t> contiguous_offsets;
    contiguous_offsets.reserve(dynamic_offsets.size());
    size_t cursor = 0;
    for (const uint32_t slot : slot_indices) {
        const uint32_t count = dynamic_range_count(descriptor_sets[slot], slot);
        if (count > dynamic_offsets.size() - cursor) {
            LOGE("onBindDescriptorSets: set %u claims %u dynamic offsets but only %zu "
                 "remain; refusing to bind (dynamic range count resolver wrong?)",
                 slot, count, dynamic_offsets.size() - cursor);
            return;
        }
        contiguous_offsets.insert(contiguous_offsets.end(), dynamic_offsets.begin() + cursor,
                                  dynamic_offsets.begin() + cursor + count);
        cursor += count;
    }
    if (cursor != dynamic_offsets.size()) {
        // The two arrays disagree, and guessing would shift every later set's
        // offsets by one entry -- a wrong UBO bound instead of a visible error.
        LOGE("onBindDescriptorSets: bound sets consume %zu dynamic offsets but the state "
             "manager holds %zu; refusing to bind",
             cursor, dynamic_offsets.size());
        return;
    }

    // Wait mask: VulkanShaderManager builds its set layouts with
    // VERTEX | FRAGMENT stage flags, so those are the only stages that can read
    // a descriptor behind this bind. A narrower mask would let the draw start
    // before the descriptors are visible.
    const VkPipelineStageFlags wait_dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

    const VkPipelineLayout layout = bound_pipeline_layout();
    if (layout == VK_NULL_HANDLE) {
        // WHY refuse: vkCmdBindDescriptorSets dereferences the layout to look up
        // every set's layout binding, so a null layout there is a crash inside
        // the driver, not a validation warning. Descriptor sets can only be
        // bound once a pipeline has been bound, so bind the pipeline first.
        if (!no_pipeline_layout_logged_) {
            no_pipeline_layout_logged_ = true;
            LOGE("onBindDescriptorSets: no pipeline has been bound, so there is no "
                 "VkPipelineLayout to bind the sets against; call bindPipeline() first");
        }
        return;
    }

    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, first_set,
                            static_cast<uint32_t>(sets.size()), sets.data(),
                            static_cast<uint32_t>(contiguous_offsets.size()),
                            contiguous_offsets.data());
}

void VulkanStateManager::onSetViewport(float x, float y, float width, float height,
                                       float min_depth, float max_depth) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (command_buffer_ == VK_NULL_HANDLE) {
        return;
    }

    // Dynamic state: VulkanShaderManager enables VK_DYNAMIC_STATE_VIEWPORT and
    // leaves pViewports null in the pipeline, so the values have to be recorded
    // here at least once per command buffer.
    //
    // WHY no Y flip: Vulkan's framebuffer Y grows downwards (the swapchain view
    // is created with identity swizzle and no flip), so the viewport is passed
    // through as given and the projection matrix handles the convention.
    VkViewport viewport{};
    viewport.x = x;
    viewport.y = y;
    viewport.width = width;
    viewport.height = height;
    viewport.minDepth = min_depth;
    viewport.maxDepth = max_depth;
    vkCmdSetViewport(command_buffer_, 0, 1, &viewport);
}

void VulkanStateManager::onSetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (command_buffer_ == VK_NULL_HANDLE) {
        return;
    }

    // Dynamic state, see onSetViewport.
    //
    // WHY the signed offset passes through: VkRect2D::offset is an int32_t, so
    // a negative origin is representable and the driver intersects it with the
    // render area. Clamping it to 0 here would silently move the scissor.
    VkRect2D scissor{};
    scissor.offset.x = x;
    scissor.offset.y = y;
    scissor.extent.width = width;
    scissor.extent.height = height;
    vkCmdSetScissor(command_buffer_, 0, 1, &scissor);
}

void VulkanStateManager::onSetTopology(uint32_t topology) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Vulkan has no equivalent of glDrawArrays' topology argument: the primitive
    // topology is baked into VkPipelineInputAssemblyStateCreateInfo when the
    // pipeline is created (VulkanShaderManager always uses
    // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST). There is nothing to record, so the
    // value is only kept for diagnostics and reported once if it disagrees with
    // the baked topology -- a silent mismatch would render the wrong primitives
    // with no error anywhere.
    topology_ = topology;
    if (topology != k_topology_triangle_list && !topology_mismatch_logged_) {
        topology_mismatch_logged_ = true;
        LOGW("setTopology(%u) cannot change the Vulkan primitive topology (the pipeline "
             "bakes in %u triangle list); value recorded for diagnostics only",
             topology, k_topology_triangle_list);
    }
}

void VulkanStateManager::onBindFramebuffer(uint64_t framebuffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (command_buffer_ == VK_NULL_HANDLE) {
        return;
    }

    VkFramebuffer vk_framebuffer = VK_NULL_HANDLE;
    if (framebuffer_resolver_) {
        vk_framebuffer = framebuffer_resolver_(framebuffer);
        if (vk_framebuffer == VK_NULL_HANDLE && framebuffer != 0) {
            LOGW("onBindFramebuffer: framebuffer %llu did not resolve",
                 static_cast<unsigned long long>(framebuffer));
        }
    } else if (framebuffer != 0 && !no_framebuffer_resolver_logged_) {
        no_framebuffer_resolver_logged_ = true;
        LOGW("no framebuffer resolver installed: setFramebuffer(%llu) records nothing",
             static_cast<unsigned long long>(framebuffer));
    }

    // WHY nothing is recorded for an unresolved framebuffer: inside a render
    // pass the framebuffer is fixed by vkCmdBeginRenderPass and
    // vkCmdBindFramebuffer is a documented no-op there, and outside one a null
    // framebuffer would make the next render pass begin invalid. Skipping keeps
    // both cases valid; the swapchain clear path binds the renderer's own
    // swapchain framebuffers directly and never comes through here.
    if (vk_framebuffer == VK_NULL_HANDLE) {
        return;
    }

    // One framebuffer, so an array of exactly one: the base's framebuffer
    // handle space is one handle per render target, not a range.
    const VkFramebuffer framebuffers[1] = {vk_framebuffer};
    vkCmdBindFramebuffer(command_buffer_, 0, 1, framebuffers);
}

} // namespace copper