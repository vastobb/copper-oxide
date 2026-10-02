#pragma once

#include <cstdint>
#include <array>
#include <vector>
#include <memory>

namespace copper {

class RendererBase;

class StateManager {
public:
    StateManager();
    virtual ~StateManager();

    StateManager(const StateManager&) = delete;
    StateManager& operator=(const StateManager&) = delete;
    StateManager(StateManager&&) noexcept = default;
    StateManager& operator=(StateManager&&) noexcept = default;

    bool initialize(RendererBase* renderer);
    void shutdown();

    virtual void bindPipeline(uint64_t pipeline);
    virtual void bindVertexBuffer(uint32_t binding, uint64_t buffer, uint32_t offset);
    virtual void bindIndexBuffer(uint64_t buffer, uint32_t index_type);
    virtual void bindDescriptorSet(uint32_t set, uint64_t descriptor_set, const std::vector<uint32_t>& dynamic_offsets);
    virtual void setViewport(float x, float y, float width, float height, float min_depth, float max_depth);
    virtual void setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height);
    virtual void setTopology(uint32_t topology);
    virtual void setFramebuffer(uint64_t framebuffer);
    virtual void applyState();
    virtual void resetState();

    virtual uint64_t getBoundPipeline() const;
    virtual uint64_t getBoundFramebuffer() const;

protected:
    virtual void onBindPipeline(uint64_t pipeline) = 0;
    virtual void onBindVertexBuffers(const std::array<uint64_t, 16>& buffers, const std::array<uint32_t, 16>& offsets) = 0;
    virtual void onBindIndexBuffer(uint64_t buffer, uint32_t index_type) = 0;
    virtual void onBindDescriptorSets(const std::array<uint64_t, 32>& descriptor_sets, const std::array<uint32_t, 32>& dynamic_offsets) = 0;
    virtual void onSetViewport(float x, float y, float width, float height, float min_depth, float max_depth) = 0;
    virtual void onSetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) = 0;
    virtual void onSetTopology(uint32_t topology) = 0;
    virtual void onBindFramebuffer(uint64_t framebuffer) = 0;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace copper