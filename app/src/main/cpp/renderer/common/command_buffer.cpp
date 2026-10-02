#include "command_buffer.h"
#include "renderer_base.h"

#include <array>
#include <cstdint>
#include <vector>
#include <mutex>
#include <functional>

namespace copper {

class CommandBuffer::Impl {
public:
    struct Command {
        enum class Type {
            BeginRenderPass,
            EndRenderPass,
            BindPipeline,
            BindVertexBuffers,
            BindIndexBuffer,
            BindDescriptorSets,
            SetViewport,
            SetScissor,
            Draw,
            DrawIndexed,
            DrawIndirect,
            Dispatch,
            CopyBuffer,
            CopyImage,
            PipelineBarrier,
            ExecuteCommands,
            PushConstants,
        } type;

        // Command-specific data
        uint64_t render_pass = 0;
        uint64_t framebuffer = 0;
        uint64_t pipeline = 0;
        uint64_t buffer = 0;
        uint64_t dst_buffer = 0;
        uint64_t src_buffer = 0;
        uint64_t image = 0;
        uint64_t dst_image = 0;
        uint64_t src_image = 0;
        uint32_t count = 0;
        uint32_t instance_count = 0;
        uint32_t first = 0;
        uint32_t first_instance = 0;
        uint32_t index_count = 0;
        uint32_t first_index = 0;
        int32_t vertex_offset = 0;
        uint32_t binding = 0;
        uint32_t offset = 0;
        uint32_t size = 0;
        uint32_t index_type = 0;
        uint32_t stage_flags = 0;
        uint32_t mip_level = 0;
        uint64_t buffer_size = 0;
        uint64_t buffer_offset = 0;
        uint64_t dst_offset = 0;
        std::vector<uint64_t> images;
        // push-constant bytes must be copied: the caller's pointer is only
        // valid for the duration of the call.
        std::vector<uint8_t> payload;
        std::vector<uint32_t> dynamic_offsets;
        std::array<float, 6> viewport{};
        std::array<int32_t, 4> scissor{};
        std::vector<uint64_t> buffers;
        std::vector<uint32_t> buffer_offsets;
    };

    std::vector<Command> commands;
    std::vector<std::function<void()>> deferred_commands;
    bool recording = false;
    bool submitted = false;
    std::mutex mutex;
    RendererBase* renderer = nullptr;
    uint32_t frame_index = 0;
    // Backend command sink. When set, commands are forwarded immediately and
    // nothing is accumulated, which is the correct model for immediate-mode
    // backends such as OpenGL ES.
    CommandSink* sink = nullptr;
};

CommandBuffer::CommandBuffer() : pImpl(std::make_unique<Impl>()) {}
CommandBuffer::~CommandBuffer() = default;

bool CommandBuffer::initialize(RendererBase* renderer, uint32_t frame_index) {
    pImpl->renderer = renderer;
    pImpl->frame_index = frame_index;
    return true;
}

void CommandBuffer::reset() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->commands.clear();
    pImpl->deferred_commands.clear();
    pImpl->recording = false;
    pImpl->submitted = false;
}

void CommandBuffer::setCommandSink(CommandSink* sink) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->sink = sink;
}

CommandSink* CommandBuffer::commandSink() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->sink;
}

bool CommandBuffer::execute() {
    // Move the recorded commands out under the lock, then replay with the lock
    // released: a sink records into the GPU command buffer and must never run
    // while this object's mutex is held.
    std::vector<Impl::Command> pending;
    std::vector<std::function<void()>> deferred;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        if (pImpl->commands.empty() && pImpl->deferred_commands.empty()) {
            return true;
        }
        pending.swap(pImpl->commands);
        deferred.swap(pImpl->deferred_commands);
    }

    CommandSink* sink = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        sink = pImpl->sink;
    }
    if (sink == nullptr) {
        // Refusing here is the whole point: silently dropping a recorded frame
        // reported success for work that never reached the GPU.
        return false;
    }

    for (const Impl::Command& cmd : pending) {
        switch (cmd.type) {
            case Impl::Command::Type::BeginRenderPass:
                sink->beginRenderPass(cmd.render_pass, cmd.framebuffer,
                                      {cmd.viewport[0], cmd.viewport[1], cmd.viewport[2], cmd.viewport[3]},
                                      cmd.viewport[4], static_cast<uint32_t>(cmd.viewport[5]));
                break;
            case Impl::Command::Type::EndRenderPass:
                sink->endRenderPass();
                break;
            case Impl::Command::Type::BindPipeline:
                sink->bindPipeline(cmd.pipeline);
                break;
            case Impl::Command::Type::BindVertexBuffers:
                sink->bindVertexBuffers(cmd.binding, cmd.buffers, cmd.buffer_offsets);
                break;
            case Impl::Command::Type::BindIndexBuffer:
                sink->bindIndexBuffer(cmd.buffer, cmd.index_type);
                break;
            case Impl::Command::Type::BindDescriptorSets:
                sink->bindDescriptorSets(cmd.binding, cmd.buffers, cmd.dynamic_offsets);
                break;
            case Impl::Command::Type::SetViewport:
                sink->setViewport(cmd.viewport[0], cmd.viewport[1], cmd.viewport[2], cmd.viewport[3],
                                  cmd.viewport[4], cmd.viewport[5]);
                break;
            case Impl::Command::Type::SetScissor:
                sink->setScissor(cmd.scissor[0], cmd.scissor[1],
                                 static_cast<uint32_t>(cmd.scissor[2]),
                                 static_cast<uint32_t>(cmd.scissor[3]));
                break;
            case Impl::Command::Type::Draw:
                sink->draw(cmd.count, cmd.instance_count, cmd.first, cmd.first_instance);
                break;
            case Impl::Command::Type::DrawIndexed:
                sink->drawIndexed(cmd.index_count, cmd.instance_count, cmd.first_index, cmd.vertex_offset,
                                  cmd.first_instance);
                break;
            case Impl::Command::Type::DrawIndirect:
                sink->drawIndirect(cmd.buffer, cmd.offset, cmd.count, cmd.size);
                break;
            case Impl::Command::Type::Dispatch:
                sink->dispatch(cmd.count, cmd.instance_count, cmd.first);
                break;
            case Impl::Command::Type::CopyBuffer:
                sink->copyBuffer(cmd.src_buffer, cmd.dst_buffer, cmd.buffer_size, cmd.buffer_offset,
                                 cmd.dst_offset);
                break;
            case Impl::Command::Type::CopyImage:
                sink->copyImage(cmd.src_image, cmd.dst_image, cmd.count, cmd.instance_count,
                                static_cast<uint32_t>(cmd.first), cmd.mip_level, cmd.first_instance);
                break;
            case Impl::Command::Type::PipelineBarrier:
                sink->pipelineBarrier(cmd.stage_flags, cmd.instance_count, cmd.index_type, cmd.buffers,
                                     cmd.images);
                break;
            case Impl::Command::Type::PushConstants:
                sink->pushConstants(cmd.stage_flags, cmd.offset, cmd.size, cmd.payload.data());
                break;
            case Impl::Command::Type::ExecuteCommands:
                // Secondary command buffers are not supported yet; the recorded
                // list is empty for every command we emit today.
                break;
        }
    }

    for (auto& deferred_command : deferred) {
        deferred_command();
    }
    return true;
}

bool CommandBuffer::begin() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (pImpl->recording) {
        return false;
    }
    pImpl->recording = true;
    pImpl->commands.clear();
    return onBegin();
}

void CommandBuffer::end() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->recording = false;
    onEnd();
}

void CommandBuffer::beginRenderPass(uint64_t render_pass, uint64_t framebuffer, const std::array<float, 4>& clear_color, float clear_depth, uint32_t clear_stencil) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Command cmd;
    cmd.type = Impl::Command::Type::BeginRenderPass;
    cmd.render_pass = render_pass;
    cmd.framebuffer = framebuffer;
    for (int i = 0; i < 4; ++i) cmd.viewport[i] = clear_color[i];
    cmd.viewport[4] = clear_depth;
    cmd.viewport[5] = static_cast<float>(clear_stencil);
    pImpl->commands.push_back(std::move(cmd));
}

void CommandBuffer::endRenderPass() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Command cmd;
    cmd.type = Impl::Command::Type::EndRenderPass;
    pImpl->commands.push_back(std::move(cmd));
}

void CommandBuffer::bindPipeline(uint64_t pipeline) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Command cmd;
    cmd.type = Impl::Command::Type::BindPipeline;
    cmd.pipeline = pipeline;
    pImpl->commands.push_back(std::move(cmd));
}

void CommandBuffer::bindVertexBuffers(uint32_t first_binding, const std::vector<uint64_t>& buffers, const std::vector<uint32_t>& offsets) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Command cmd;
    cmd.type = Impl::Command::Type::BindVertexBuffers;
    cmd.binding = first_binding;
    cmd.buffers = buffers;
    cmd.buffer_offsets = offsets;
    pImpl->commands.push_back(std::move(cmd));
}

void CommandBuffer::bindIndexBuffer(uint64_t buffer, uint32_t index_type) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Command cmd;
    cmd.type = Impl::Command::Type::BindIndexBuffer;
    cmd.buffer = buffer;
    cmd.count = index_type;
    pImpl->commands.push_back(std::move(cmd));
}

void CommandBuffer::bindDescriptorSets(uint32_t first_set, const std::vector<uint64_t>& descriptor_sets, const std::vector<uint32_t>& dynamic_offsets) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Command cmd;
    cmd.type = Impl::Command::Type::BindDescriptorSets;
    cmd.binding = first_set;
    cmd.buffers = descriptor_sets;
    cmd.dynamic_offsets = dynamic_offsets;
    pImpl->commands.push_back(std::move(cmd));
}

void CommandBuffer::setViewport(float x, float y, float width, float height, float min_depth, float max_depth) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Command cmd;
    cmd.type = Impl::Command::Type::SetViewport;
    cmd.viewport = {x, y, width, height, min_depth, max_depth};
    pImpl->commands.push_back(std::move(cmd));
}

void CommandBuffer::setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Command cmd;
    cmd.type = Impl::Command::Type::SetScissor;
    cmd.scissor = {x, y, static_cast<int32_t>(width), static_cast<int32_t>(height)};
    pImpl->commands.push_back(std::move(cmd));
}

void CommandBuffer::draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Command cmd;
    cmd.type = Impl::Command::Type::Draw;
    cmd.count = vertex_count;
    cmd.instance_count = instance_count;
    cmd.first = first_vertex;
    cmd.first_instance = first_instance;
    pImpl->commands.push_back(std::move(cmd));
}

void CommandBuffer::drawIndexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Command cmd;
    cmd.type = Impl::Command::Type::DrawIndexed;
    cmd.index_count = index_count;
    cmd.instance_count = instance_count;
    cmd.first_index = first_index;
    cmd.vertex_offset = vertex_offset;
    cmd.first_instance = first_instance;
    pImpl->commands.push_back(std::move(cmd));
}

void CommandBuffer::drawIndirect(uint64_t buffer, uint32_t offset, uint32_t draw_count, uint32_t stride) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Command cmd;
    cmd.type = Impl::Command::Type::DrawIndirect;
    cmd.buffer = buffer;
    cmd.offset = offset;
    cmd.count = draw_count;
    cmd.size = stride;
    pImpl->commands.push_back(std::move(cmd));
}

void CommandBuffer::dispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Command cmd;
    cmd.type = Impl::Command::Type::Dispatch;
    cmd.count = group_count_x;
    cmd.instance_count = group_count_y;
    cmd.first = group_count_z;
    pImpl->commands.push_back(std::move(cmd));
}

void CommandBuffer::copyBuffer(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset, uint64_t dst_offset) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Command cmd;
    cmd.type = Impl::Command::Type::CopyBuffer;
    cmd.src_buffer = src;
    cmd.dst_buffer = dst;
    cmd.buffer_size = size;
    cmd.buffer_offset = src_offset;
    cmd.dst_offset = dst_offset;
    pImpl->commands.push_back(std::move(cmd));
}

void CommandBuffer::copyImage(uint64_t src, uint64_t dst, uint32_t width, uint32_t height, uint32_t depth, uint32_t mip_level, uint32_t array_layer) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Command cmd;
    cmd.type = Impl::Command::Type::CopyImage;
    cmd.src_image = src;
    cmd.dst_image = dst;
    cmd.count = width;
    cmd.instance_count = height;
    cmd.first = depth;
    cmd.mip_level = mip_level;
    cmd.first_instance = array_layer;
    pImpl->commands.push_back(std::move(cmd));
}

void CommandBuffer::pipelineBarrier(uint32_t src_stage, uint32_t dst_stage, uint32_t dependency_flags, const std::vector<uint64_t>& buffers, const std::vector<uint64_t>& images) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Command cmd;
    cmd.type = Impl::Command::Type::PipelineBarrier;
    cmd.stage_flags = src_stage;
    cmd.instance_count = dst_stage;
    cmd.index_type = dependency_flags;
    cmd.buffers = buffers;
    cmd.images = images;
    pImpl->commands.push_back(std::move(cmd));
}

void CommandBuffer::executeCommands(const std::vector<CommandBuffer*>& command_buffers) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Command cmd;
    cmd.type = Impl::Command::Type::ExecuteCommands;
    for (auto* cb : command_buffers) {
        cmd.buffers.push_back(reinterpret_cast<uint64_t>(cb));
    }
    pImpl->commands.push_back(std::move(cmd));
}

void CommandBuffer::pushConstants(uint32_t stage_flags, uint32_t offset, uint32_t size, const void* data) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Impl::Command cmd;
    cmd.type = Impl::Command::Type::PushConstants;
    cmd.stage_flags = stage_flags;
    cmd.offset = offset;
    cmd.size = size;
    if (data != nullptr && size > 0) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        cmd.payload.assign(bytes, bytes + size);
    }
    pImpl->commands.push_back(std::move(cmd));
}

void CommandBuffer::defer(std::function<void()> command) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->deferred_commands.push_back(std::move(command));
}

bool CommandBuffer::submit() {
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        if (pImpl->submitted || pImpl->recording) {
            return false;
        }
        pImpl->submitted = true;
    }
    // Replay before the backend submits: with a sink installed this is what
    // turns recorded commands into real API calls. Skipping it would submit an
    // empty batch and report success.
    if (commandSink() != nullptr && !execute()) {
        return false;
    }
    return onSubmit();
}

void CommandBuffer::wait() {
    onWait();
}

} // namespace copper