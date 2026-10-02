#pragma once

#include "command_buffer.h"
#include "profiler.h"
#include "state_manager.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <vector>

namespace copper {

class VulkanRenderer;
class VulkanStateManager;

// Vulkan backend for CommandSink.
//
// CommandBuffer records the frame's work; this sink records it into the frame's
// VkCommandBuffer. Unlike GLES there is nothing immediate here: every call is a
// vkCmd* that must happen between vkBeginCommandBuffer and vkEndCommandBuffer,
// which the renderer owns.
//
// State that VulkanStateManager already implements (pipeline, vertex buffers,
// index buffer, descriptor sets, viewport, scissor, framebuffer) is delegated to
// it so the dirty-flag logic stays in one place. The sink adds what
// StateManager has no concept of: the render pass, the draws themselves, image
// layout transitions and copy commands.
class VulkanCommandSink : public CommandSink {
public:
    VulkanCommandSink(VulkanRenderer* renderer, VulkanStateManager* state);
    ~VulkanCommandSink() override;

    // Must be called after vkBeginCommandBuffer for the frame and before the
    // commands are recorded; passing VK_NULL_HANDLE marks the sink idle so the
    // hooks refuse to record into a command buffer that is not being built.
    void setCommandBuffer(VkCommandBuffer command_buffer, uint32_t frame_index);
    bool hasCommandBuffer() const;

    void setProfiler(Profiler* profiler);
    void setSwapchainFramebuffers(const std::vector<VkFramebuffer>& framebuffers);
    VkFramebuffer defaultFramebuffer() const;

    uint64_t droppedCommandCount() const;

    void beginRenderPass(uint64_t render_pass, uint64_t framebuffer,
                         const std::array<float, 4>& clear_color, float clear_depth,
                         uint32_t clear_stencil) override;
    void endRenderPass() override;

    void bindPipeline(uint64_t pipeline) override;
    void bindVertexBuffers(uint32_t first_binding, const std::vector<uint64_t>& buffers,
                            const std::vector<uint32_t>& offsets) override;
    void bindIndexBuffer(uint64_t buffer, uint32_t index_type) override;
    void bindDescriptorSets(uint32_t first_set, const std::vector<uint64_t>& descriptor_sets,
                             const std::vector<uint32_t>& dynamic_offsets) override;

    void setViewport(float x, float y, float width, float height, float min_depth,
                     float max_depth) override;
    void setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) override;

    void draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
              uint32_t first_instance) override;
    void drawIndexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                     int32_t vertex_offset, uint32_t first_instance) override;
    void drawIndirect(uint64_t buffer, uint32_t offset, uint32_t draw_count,
                      uint32_t stride) override;
    void dispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) override;

    void copyBuffer(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset,
                    uint64_t dst_offset) override;
    void copyImage(uint64_t src, uint64_t dst, uint32_t width, uint32_t height, uint32_t depth,
                   uint32_t mip_level, uint32_t array_layer) override;
    void pipelineBarrier(uint32_t src_stage, uint32_t dst_stage, uint32_t dependency_flags,
                         const std::vector<uint64_t>& buffers,
                         const std::vector<uint64_t>& images) override;
    void pushConstants(uint32_t stage_flags, uint32_t offset, uint32_t size,
                       const void* data) override;

private:
    mutable std::mutex mutex_;
    VulkanRenderer* renderer_ = nullptr;
    VulkanStateManager* state_ = nullptr;
    Profiler* profiler_ = nullptr;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    uint32_t frame_index_ = 0;
    std::vector<VkFramebuffer> swapchain_framebuffers_;
    uint64_t dropped_ = 0;
    bool in_render_pass_ = false;
};

} // namespace copper