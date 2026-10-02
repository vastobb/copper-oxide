#include "state_manager.h"
#include "renderer_base.h"

#include <algorithm>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <array>

namespace copper {

class StateManager::Impl {
public:
    struct PipelineState {
        uint64_t pipeline = 0;
        std::array<uint64_t, 16> vertex_buffers{};
        std::array<uint32_t, 16> vertex_buffer_offsets{};
        uint64_t index_buffer = 0;
        uint32_t index_type = 0;
        uint32_t topology = 0;
        // One flag for the whole group meant a single index-buffer rebind
        // re-issued the pipeline and all sixteen vertex buffers.
        bool pipeline_dirty = true;
        bool vertex_buffers_dirty = true;
        bool index_buffer_dirty = true;
        bool topology_dirty = true;
        bool dirty() const {
            return pipeline_dirty || vertex_buffers_dirty || index_buffer_dirty || topology_dirty;
        }
    };

    struct DescriptorState {
        std::array<uint64_t, 32> descriptor_sets{};
        // Per-set dynamic offsets: a flat array silently dropped every offset
        // whose count was not exactly 32.
        std::array<std::vector<uint32_t>, 32> set_dynamic_offsets{};
        bool dirty = true;
    };

    struct ViewportState {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float min_depth = 0.0f;
        float max_depth = 1.0f;
        bool dirty = true;
    };

    struct ScissorState {
        int32_t x = 0;
        int32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        bool dirty = true;
    };

    PipelineState pipeline_state;
    DescriptorState descriptor_state;
    ViewportState viewport_state;
    ScissorState scissor_state;
    uint64_t current_framebuffer = 0;
    bool framebuffer_dirty = true;
    std::mutex mutex;
    RendererBase* renderer = nullptr;
    std::unordered_map<uint64_t, std::vector<uint8_t>> pipeline_cache;
};

StateManager::StateManager() : pImpl(std::make_unique<Impl>()) {}
StateManager::~StateManager() = default;

bool StateManager::initialize(RendererBase* renderer) {
    pImpl->renderer = renderer;
    return true;
}

void StateManager::shutdown() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->pipeline_cache.clear();
}

void StateManager::bindPipeline(uint64_t pipeline) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (pImpl->pipeline_state.pipeline != pipeline) {
        pImpl->pipeline_state.pipeline = pipeline;
        pImpl->pipeline_state.pipeline_dirty = true;
    }
}

void StateManager::bindVertexBuffer(uint32_t binding, uint64_t buffer, uint32_t offset) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (binding < 16) {
        // Only mark dirty when something actually changed.
        if (pImpl->pipeline_state.vertex_buffers[binding] != buffer ||
            pImpl->pipeline_state.vertex_buffer_offsets[binding] != offset) {
            pImpl->pipeline_state.vertex_buffers[binding] = buffer;
            pImpl->pipeline_state.vertex_buffer_offsets[binding] = offset;
            pImpl->pipeline_state.vertex_buffers_dirty = true;
        }
    }
}

void StateManager::bindIndexBuffer(uint64_t buffer, uint32_t index_type) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (pImpl->pipeline_state.index_buffer != buffer ||
        pImpl->pipeline_state.index_type != index_type) {
        pImpl->pipeline_state.index_buffer = buffer;
        pImpl->pipeline_state.index_type = index_type;
        pImpl->pipeline_state.index_buffer_dirty = true;
    }
}

void StateManager::bindDescriptorSet(uint32_t set, uint64_t descriptor_set, const std::vector<uint32_t>& dynamic_offsets) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (set < 32) {
        const bool changed = pImpl->descriptor_state.descriptor_sets[set] != descriptor_set ||
                             pImpl->descriptor_state.set_dynamic_offsets[set] != dynamic_offsets;
        pImpl->descriptor_state.descriptor_sets[set] = descriptor_set;
        pImpl->descriptor_state.set_dynamic_offsets[set] = dynamic_offsets;
        if (changed) {
            pImpl->descriptor_state.dirty = true;
        }
    }
}

void StateManager::setViewport(float x, float y, float width, float height, float min_depth, float max_depth) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (pImpl->viewport_state.x == x && pImpl->viewport_state.y == y &&
        pImpl->viewport_state.width == width && pImpl->viewport_state.height == height &&
        pImpl->viewport_state.min_depth == min_depth && pImpl->viewport_state.max_depth == max_depth) {
        return;
    }
    pImpl->viewport_state.x = x;
    pImpl->viewport_state.y = y;
    pImpl->viewport_state.width = width;
    pImpl->viewport_state.height = height;
    pImpl->viewport_state.min_depth = min_depth;
    pImpl->viewport_state.max_depth = max_depth;
    pImpl->viewport_state.dirty = true;
}

void StateManager::setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (pImpl->scissor_state.x == x && pImpl->scissor_state.y == y &&
        pImpl->scissor_state.width == width && pImpl->scissor_state.height == height) {
        return;
    }
    pImpl->scissor_state.x = x;
    pImpl->scissor_state.y = y;
    pImpl->scissor_state.width = width;
    pImpl->scissor_state.height = height;
    pImpl->scissor_state.dirty = true;
}

void StateManager::setTopology(uint32_t topology) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (pImpl->pipeline_state.topology != topology) {
        pImpl->pipeline_state.topology = topology;
        pImpl->pipeline_state.topology_dirty = true;
    }
}

void StateManager::setFramebuffer(uint64_t framebuffer) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (pImpl->current_framebuffer != framebuffer) {
        pImpl->current_framebuffer = framebuffer;
        pImpl->framebuffer_dirty = true;
    }
}

void StateManager::applyState() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    
    // Apply only the parts that changed, in dependency order.
    if (pImpl->pipeline_state.pipeline_dirty) {
        onBindPipeline(pImpl->pipeline_state.pipeline);
        pImpl->pipeline_state.pipeline_dirty = false;
    }
    if (pImpl->pipeline_state.vertex_buffers_dirty) {
        onBindVertexBuffers(pImpl->pipeline_state.vertex_buffers, pImpl->pipeline_state.vertex_buffer_offsets);
        pImpl->pipeline_state.vertex_buffers_dirty = false;
    }
    if (pImpl->pipeline_state.index_buffer_dirty) {
        onBindIndexBuffer(pImpl->pipeline_state.index_buffer, pImpl->pipeline_state.index_type);
        pImpl->pipeline_state.index_buffer_dirty = false;
    }
    if (pImpl->pipeline_state.topology_dirty) {
        onSetTopology(pImpl->pipeline_state.topology);
        pImpl->pipeline_state.topology_dirty = false;
    }

    if (pImpl->descriptor_state.dirty) {
        // Flatten the per-set offsets for the backend call.
        std::vector<uint32_t> flat_offsets;
        for (uint32_t set = 0; set < pImpl->descriptor_state.set_dynamic_offsets.size(); ++set) {
            const auto& offsets = pImpl->descriptor_state.set_dynamic_offsets[set];
            flat_offsets.insert(flat_offsets.end(), offsets.begin(), offsets.end());
        }
        onBindDescriptorSets(pImpl->descriptor_state.descriptor_sets, flat_offsets);
        pImpl->descriptor_state.dirty = false;
    }

    if (pImpl->viewport_state.dirty) {
        onSetViewport(pImpl->viewport_state.x, pImpl->viewport_state.y, pImpl->viewport_state.width, pImpl->viewport_state.height, pImpl->viewport_state.min_depth, pImpl->viewport_state.max_depth);
        pImpl->viewport_state.dirty = false;
    }

    if (pImpl->scissor_state.dirty) {
        onSetScissor(pImpl->scissor_state.x, pImpl->scissor_state.y, pImpl->scissor_state.width, pImpl->scissor_state.height);
        pImpl->scissor_state.dirty = false;
    }

    if (pImpl->framebuffer_dirty) {
        onBindFramebuffer(pImpl->current_framebuffer);
        pImpl->framebuffer_dirty = false;
    }
}

void StateManager::resetState() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->pipeline_state = {};
    pImpl->descriptor_state = {};
    pImpl->viewport_state = {};
    pImpl->scissor_state = {};
    pImpl->current_framebuffer = 0;
    pImpl->pipeline_state.pipeline_dirty = true;
    pImpl->pipeline_state.vertex_buffers_dirty = true;
    pImpl->pipeline_state.index_buffer_dirty = true;
    pImpl->pipeline_state.topology_dirty = true;
    pImpl->descriptor_state.dirty = true;
    pImpl->viewport_state.dirty = true;
    pImpl->scissor_state.dirty = true;
    pImpl->framebuffer_dirty = true;
}

uint64_t StateManager::getBoundPipeline() const {
    return pImpl->pipeline_state.pipeline;
}

uint64_t StateManager::getBoundFramebuffer() const {
    return pImpl->current_framebuffer;
}

} // namespace copper