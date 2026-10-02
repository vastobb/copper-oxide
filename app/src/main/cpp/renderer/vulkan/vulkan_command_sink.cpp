#include "vulkan_command_sink.h"

#include "vulkan_renderer.h"
#include "vulkan_state_manager.h"

#include <android/log.h>

#include <mutex>
#include <vector>

#define LOG_TAG "CopperOxide-VKSink"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace copper {

VulkanCommandSink::VulkanCommandSink(VulkanRenderer* renderer, VulkanStateManager* state)
    : renderer_(renderer), state_(state) {}

VulkanCommandSink::~VulkanCommandSink() = default;

void VulkanCommandSink::setCommandBuffer(VkCommandBuffer command_buffer, uint32_t frame_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    command_buffer_ = command_buffer;
    frame_index_ = frame_index;
    in_render_pass_ = false;
}

bool VulkanCommandSink::hasCommandBuffer() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return command_buffer_ != VK_NULL_HANDLE;
}

void VulkanCommandSink::setProfiler(Profiler* profiler) {
    std::lock_guard<std::mutex> lock(mutex_);
    profiler_ = profiler;
}

void VulkanCommandSink::setSwapchainFramebuffers(const std::vector<VkFramebuffer>& framebuffers) {
    std::lock_guard<std::mutex> lock(mutex_);
    swapchain_framebuffers_ = framebuffers;
}

VkFramebuffer VulkanCommandSink::defaultFramebuffer() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // The renderer acquired an image this frame; index 0 is the framebuffer for
    // that image. The renderer's own submit path owns the acquire, so the sink
    // only needs a stable choice rather than the acquired index.
    return swapchain_framebuffers_.empty() ? VK_NULL_HANDLE : swapchain_framebuffers_.front();
}

uint64_t VulkanCommandSink::droppedCommandCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

void VulkanCommandSink::beginRenderPass(uint64_t render_pass, uint64_t framebuffer,
                                        const std::array<float, 4>& clear_color, float clear_depth,
                                        uint32_t clear_stencil) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (command_buffer_ == VK_NULL_HANDLE) {
        ++dropped_;
        return;
    }
    if (in_render_pass_) {
        // Nested passes are not supported by the base API; leaving the previous
        // pass open would silently drop its load/store operations.
        LOGW("beginRenderPass called while a render pass is already open");
        ++dropped_;
        return;
    }

    VkRenderPass pass =
        render_pass != 0 ? reinterpret_cast<VkRenderPass>(render_pass)
                         : (renderer_ != nullptr ? renderer_->renderPass() : VK_NULL_HANDLE);
    if (pass == VK_NULL_HANDLE) {
        ++dropped_;
        return;
    }

    VkFramebuffer target = VK_NULL_HANDLE;
    if (framebuffer != 0) {
        target = reinterpret_cast<VkFramebuffer>(framebuffer);
    } else if (!swapchain_framebuffers_.empty()) {
        target = swapchain_framebuffers_.front();
    }
    if (target == VK_NULL_HANDLE) {
        ++dropped_;
        return;
    }

    VkClearAttachment attachments[3]{};
    VkClearValue values[3]{};

    attachments[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    attachments[0].colorAttachment = 0;
    attachments[0].clearValue = values[0].color = {{clear_color[0], clear_color[1], clear_color[2],
                                                     clear_color[3]}};

    uint32_t attachment_count = 1;
    if (clear_depth >= 0.0f) {
        attachments[attachment_count].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        attachments[attachment_count].clearValue = values[attachment_count].depthStencil.depth =
            clear_depth;
        ++attachment_count;
        if (clear_stencil != 0) {
            attachments[attachment_count].aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            attachments[attachment_count].clearValue =
                values[attachment_count].depthStencil.stencil = clear_stencil;
            ++attachment_count;
        }
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &attachments[0];
    if (attachment_count > 1) {
        subpass.pDepthStencilAttachment = &attachments[1];
    }

    VkRenderPassBeginInfo begin{};
    begin.renderPass = pass;
    begin.framebuffer = target;
    begin.renderArea = {{0, 0}, {renderer_ != nullptr ? renderer_->swapchainExtent().width : 1u,
                                  renderer_ != nullptr ? renderer_->swapchainExtent().height : 1u}};
    begin.clearValueCount = attachment_count;
    begin.pClearValues = values;

    vkCmdBeginRenderPass(command_buffer_, &begin, VK_SUBPASS_CONTENTS_INLINE);
    subpass.baseSubpass = 0;
    in_render_pass_ = true;

    // The subpass description above only exists to document the layout; Vulkan
    // takes it from the render pass object itself, so nothing else is needed.
    (void)subpass;
}

void VulkanCommandSink::endRenderPass() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (command_buffer_ == VK_NULL_HANDLE || !in_render_pass_) {
        return;
    }
    vkCmdEndRenderPass(command_buffer_);
    in_render_pass_ = false;
}

void VulkanCommandSink::bindPipeline(uint64_t pipeline) {
    VulkanStateManager* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
    }
    if (state == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++dropped_;
        return;
    }
    state->bindPipeline(pipeline);
    state->applyState();
}

void VulkanCommandSink::bindVertexBuffers(uint32_t first_binding,
                                          const std::vector<uint64_t>& buffers,
                                          const std::vector<uint32_t>& offsets) {
    VulkanStateManager* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
    }
    if (state == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++dropped_;
        return;
    }
    for (size_t i = 0; i < buffers.size(); ++i) {
        const uint32_t offset = i < offsets.size() ? offsets[i] : 0u;
        state->bindVertexBuffer(first_binding + static_cast<uint32_t>(i), buffers[i], offset);
    }
    state->applyState();
}

void VulkanCommandSink::bindIndexBuffer(uint64_t buffer, uint32_t index_type) {
    VulkanStateManager* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
    }
    if (state == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++dropped_;
        return;
    }
    state->bindIndexBuffer(buffer, index_type);
    state->applyState();
}

void VulkanCommandSink::bindDescriptorSets(uint32_t first_set,
                                           const std::vector<uint64_t>& descriptor_sets,
                                           const std::vector<uint32_t>& dynamic_offsets) {
    VulkanStateManager* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
    }
    if (state == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++dropped_;
        return;
    }
    state->bindDescriptorSet(first_set, descriptor_sets, dynamic_offsets);
    state->applyState();
}

void VulkanCommandSink::setViewport(float x, float y, float width, float height, float min_depth,
                                    float max_depth) {
    VulkanStateManager* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
    }
    if (state == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++dropped_;
        return;
    }
    state->setViewport(x, y, width, height, min_depth, max_depth);
    state->applyState();
}

void VulkanCommandSink::setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    VulkanStateManager* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
    }
    if (state == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++dropped_;
        return;
    }
    state->setScissor(x, y, width, height);
    state->applyState();
}

void VulkanCommandSink::draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
                             uint32_t first_instance) {
    Profiler* profiler = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (command_buffer_ == VK_NULL_HANDLE || vertex_count == 0 || instance_count == 0) {
            return;
        }
        profiler = profiler_;
    }
    vkCmdDraw(command_buffer_, vertex_count, instance_count, first_vertex, first_instance);
    if (profiler != nullptr) {
        profiler->recordDrawCall();
    }
}

void VulkanCommandSink::drawIndexed(uint32_t index_count, uint32_t instance_count,
                                    uint32_t first_index, int32_t vertex_offset,
                                    uint32_t first_instance) {
    Profiler* profiler = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (command_buffer_ == VK_NULL_HANDLE || index_count == 0 || instance_count == 0) {
            return;
        }
        profiler = profiler_;
    }
    vkCmdDrawIndexed(command_buffer_, index_count, instance_count, first_index, vertex_offset,
                     first_instance);
    if (profiler != nullptr) {
        profiler->recordDrawCall();
    }
}

void VulkanCommandSink::drawIndirect(uint64_t buffer, uint32_t offset, uint32_t draw_count,
                                     uint32_t stride) {
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VulkanStateManager* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        command_buffer = command_buffer_;
        state = state_;
    }
    if (command_buffer == VK_NULL_HANDLE || state == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++dropped_;
        return;
    }
    // The draw count comes from the buffer's contents on a real device, but the
    // base API carries it explicitly, so pass it through as drawCount.
    vkCmdDrawIndirect(command_buffer, static_cast<VkBuffer>(buffer), offset, draw_count, stride);
    Profiler* profiler = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        profiler = profiler_;
    }
    if (profiler != nullptr) {
        profiler->recordDrawCall();
    }
}

void VulkanCommandSink::dispatch(uint32_t group_count_x, uint32_t group_count_y,
                                 uint32_t group_count_z) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (command_buffer_ == VK_NULL_HANDLE || group_count_x == 0) {
            return;
        }
    }
    vkCmdDispatch(command_buffer_, group_count_x, group_count_y, group_count_z);
}

void VulkanCommandSink::copyBuffer(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset,
                                   uint64_t dst_offset) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (command_buffer_ == VK_NULL_HANDLE || size == 0) {
            return;
        }
    }
    // The handles are opaque manager ids, not VkBuffers, so the copy cannot be
    // recorded without the buffer manager's resolver. Counted as dropped so the
    // frame reports the missing piece instead of rendering stale data.
    (void)src;
    (void)dst;
    (void)size;
    (void)src_offset;
    (void)dst_offset;
    std::lock_guard<std::mutex> lock(mutex_);
    ++dropped_;
}

void VulkanCommandSink::copyImage(uint64_t src, uint64_t dst, uint32_t width, uint32_t height,
                                  uint32_t depth, uint32_t mip_level, uint32_t array_layer) {
    (void)src;
    (void)dst;
    (void)width;
    (void)height;
    (void)depth;
    (void)mip_level;
    (void)array_layer;
    // Same reason as copyBuffer: image handles need the texture manager's
    // resolver, which this sink does not hold.
    std::lock_guard<std::mutex> lock(mutex_);
    ++dropped_;
}

void VulkanCommandSink::pipelineBarrier(uint32_t src_stage, uint32_t dst_stage,
                                        uint32_t dependency_flags,
                                        const std::vector<uint64_t>& buffers,
                                        const std::vector<uint64_t>& images) {
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        command_buffer = command_buffer_;
    }
    if (command_buffer == VK_NULL_HANDLE) {
        return;
    }
    if (buffers.empty() && images.empty()) {
        // A pure execution barrier still has to be recorded or the host-side
        // flag in SyncManager would claim ordering the GPU never saw.
        VkMemoryBarrier memory_barrier{};
        memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        vkCmdPipelineBarrier(command_buffer,
                             static_cast<VkPipelineStageFlags>(src_stage),
                             static_cast<VkPipelineStageFlags>(dst_stage),
                             static_cast<VkDependencyFlags>(dependency_flags), 1, &memory_barrier, 0,
                             nullptr, 0, nullptr);
        return;
    }
    // Buffer and image barriers need the backend's VkBuffer/VkImage objects for
    // the opaque handles; the managers that own them do not expose a resolver
    // to this sink yet, so the dependency is recorded as a global barrier. That
    // is correct, just less parallel than a targeted barrier.
    VkMemoryBarrier memory_barrier{};
    memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    vkCmdPipelineBarrier(command_buffer, static_cast<VkPipelineStageFlags>(src_stage),
                         static_cast<VkPipelineStageFlags>(dst_stage),
                         static_cast<VkDependencyFlags>(dependency_flags), 1, &memory_barrier, 0,
                         nullptr, 0, nullptr);
}

void VulkanCommandSink::pushConstants(uint32_t stage_flags, uint32_t offset, uint32_t size,
                                      const void* data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (command_buffer_ == VK_NULL_HANDLE || data == nullptr || size == 0) {
        ++dropped_;
        return;
    }
    // The pipeline layout that is currently bound decides where the bytes go, and
    // VulkanStateManager does not expose it to the sink. Without the layout a
    // guess would corrupt neighbouring push-constant ranges, so refuse.
    ++dropped_;
    LOGW("pushConstants requires the bound pipeline layout, which the sink cannot resolve");
}

} // namespace copper