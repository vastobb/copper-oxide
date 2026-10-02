#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include <memory>

#include "renderer_config.h"

namespace copper {

class RendererBase;

/**
 * Backend-side command sink.
 *
 * A CommandBuffer is backend agnostic, so it cannot issue an API call itself.
 * The backend installs a sink and the recorded (or immediately submitted)
 * commands are translated into real GL / Vulkan calls there.
 *
 * On Vulkan the sink records into the frame's VkCommandBuffer; on OpenGL ES it
 * issues the calls directly because GL is an immediate-mode API.
 */
class CommandSink {
public:
    virtual ~CommandSink() = default;

    virtual void beginRenderPass(uint64_t render_pass, uint64_t framebuffer,
                                 const std::array<float, 4>& clear_color, float clear_depth,
                                 uint32_t clear_stencil) = 0;
    virtual void endRenderPass() = 0;

    virtual void bindPipeline(uint64_t pipeline) = 0;
    virtual void bindVertexBuffers(uint32_t first_binding, const std::vector<uint64_t>& buffers,
                                    const std::vector<uint32_t>& offsets) = 0;
    virtual void bindIndexBuffer(uint64_t buffer, uint32_t index_type) = 0;
    virtual void bindDescriptorSets(uint32_t first_set, const std::vector<uint64_t>& descriptor_sets,
                                     const std::vector<uint32_t>& dynamic_offsets) = 0;

    virtual void setViewport(float x, float y, float width, float height, float min_depth,
                             float max_depth) = 0;
    virtual void setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) = 0;

    virtual void draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
                      uint32_t first_instance) = 0;
    virtual void drawIndexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                             int32_t vertex_offset, uint32_t first_instance) = 0;
    virtual void drawIndirect(uint64_t buffer, uint32_t offset, uint32_t draw_count,
                              uint32_t stride) = 0;
    virtual void dispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) = 0;

    virtual void copyBuffer(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset,
                            uint64_t dst_offset) = 0;
    virtual void copyImage(uint64_t src, uint64_t dst, uint32_t width, uint32_t height, uint32_t depth,
                           uint32_t mip_level, uint32_t array_layer) = 0;
    virtual void pipelineBarrier(uint32_t src_stage, uint32_t dst_stage, uint32_t dependency_flags,
                                 const std::vector<uint64_t>& buffers,
                                 const std::vector<uint64_t>& images) = 0;
    virtual void pushConstants(uint32_t stage_flags, uint32_t offset, uint32_t size, const void* data) = 0;
};

class CommandBuffer {
public:
    CommandBuffer();
    virtual ~CommandBuffer();

    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;
    CommandBuffer(CommandBuffer&&) noexcept = default;
    CommandBuffer& operator=(CommandBuffer&&) noexcept = default;

    bool initialize(RendererBase* renderer, uint32_t frame_index);
    void reset();

    /**
     * Install the backend sink that turns commands into real API calls.
     * Passing nullptr restores deferred recording, where commands are only
     * replayed by an explicit execute() call.
     */
    void setCommandSink(CommandSink* sink);
    CommandSink* commandSink() const;

    /**
     * Replay every recorded command through the sink.
     *
     * Immediate backends (OpenGL ES) never accumulate anything, so this is a
     * no-op for them. Recording backends call it once, after the frame's work
     * has been recorded, to obtain the actual API calls. Returns false when no
     * sink is installed, because otherwise the recorded work would be silently
     * dropped.
     */
    bool execute();

    virtual bool begin();
    virtual void end();

    virtual void beginRenderPass(uint64_t render_pass, uint64_t framebuffer, const std::array<float, 4>& clear_color, float clear_depth, uint32_t clear_stencil);
    virtual void endRenderPass();

    virtual void bindPipeline(uint64_t pipeline);
    virtual void bindVertexBuffers(uint32_t first_binding, const std::vector<uint64_t>& buffers, const std::vector<uint32_t>& offsets);
    virtual void bindIndexBuffer(uint64_t buffer, uint32_t index_type);
    virtual void bindDescriptorSets(uint32_t first_set, const std::vector<uint64_t>& descriptor_sets, const std::vector<uint32_t>& dynamic_offsets);
    virtual void setViewport(float x, float y, float width, float height, float min_depth, float max_depth);
    virtual void setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height);

    virtual void draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance);
    virtual void drawIndexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance);
    virtual void drawIndirect(uint64_t buffer, uint32_t offset, uint32_t draw_count, uint32_t stride);
    virtual void dispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z);

    virtual void copyBuffer(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset, uint64_t dst_offset);
    virtual void copyImage(uint64_t src, uint64_t dst, uint32_t width, uint32_t height, uint32_t depth, uint32_t mip_level, uint32_t array_layer);
    virtual void pipelineBarrier(uint32_t src_stage, uint32_t dst_stage, uint32_t dependency_flags, const std::vector<uint64_t>& buffers, const std::vector<uint64_t>& images);
    virtual void executeCommands(const std::vector<CommandBuffer*>& command_buffers);
    virtual void pushConstants(uint32_t stage_flags, uint32_t offset, uint32_t size, const void* data);

    virtual void defer(std::function<void()> command);

    virtual bool submit();
    virtual void wait();

protected:
    virtual bool onBegin() = 0;
    virtual void onEnd() = 0;
    virtual bool onSubmit() = 0;
    virtual void onWait() = 0;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace copper