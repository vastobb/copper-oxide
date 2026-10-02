#pragma once

// WHY: the platform guard has to be defined before vulkan.h is pulled in,
// otherwise the Android surface/window types stay invisible.
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "state_manager.h"
// Reused, NOT duplicated: VulkanVertexLayout is the single place that owns the
// vertex format, and the pipeline bakes that format into
// VkPipelineVertexInputStateCreateInfo. The bind side therefore takes its stride
// from the same constant instead of hard coding a second copy that could drift.
#include "vulkan_shader_manager.h"

namespace copper {

class VulkanRenderer;

// INTEGRATOR NOTE -- vulkan_renderer.h currently ends with the stubs
//     class VulkanFramebufferManager : public FramebufferManager { /* ... */ };
//     class VulkanStateManager : public StateManager { /* ... */ };
// and it does NOT include this header. Those two stubs have to be deleted and
// replaced with `#include "vulkan_state_manager.h"` (+ the framebuffer manager
// header), otherwise `copper::VulkanStateManager` is defined twice and this
// translation unit does not compile. The same note applies to
// vulkan_shader_manager.h / VulkanShaderManager, whose .cpp has the identical
// include order (own header first, vulkan_renderer.h second).
//
// INTEGRATOR NOTE -- public VulkanRenderer accessors this class calls. They do
// not exist yet and vulkan_renderer.h is off limits to this file, so add them
// to VulkanRenderer's public section (each is a one-line inline getter over the
// existing private member):
//
//     VkDevice   device() const;           // device_
//     VkRenderPass renderPass() const;     // render_pass_
//     uint32_t   framesInFlight() const;   // frames_in_flight_
//
// (`device()` and `renderPass()` are the same two that vulkan_shader_manager.cpp
// already expects, so the three managers share one accessor set.)
//
// Everything else this class needs from the engine arrives through the injected
// resolvers below, because ShaderManager / BufferManager / FramebufferManager
// expose only opaque uint64_t handles through their cross-backend API.
//
// Vulkan backend for StateManager.
//
// StateManager owns the dirty tracking and the command ordering; this class only
// translates the state into vkCmd* calls on the command buffer the integrator
// hands it with setCommandBuffer().
//
// SAFETY: every hook is a no-op until setCommandBuffer() has installed a non-null
// command buffer, so the manager is safe to use outside a frame (during init, or
// while a frame was skipped by a failed vkAcquireNextImageKHR).
//
// DEADLOCK RULE: the base calls the on* hooks while holding ITS OWN mutex. These
// hooks therefore never call any public method of StateManager / this class, and
// the resolvers installed below must be pure handle -> VkObject lookups that do
// not call back into the renderer or the managers. The backend mutex is never
// held across vkQueueSubmit / vkDeviceWaitIdle (this backend never submits).
class VulkanStateManager : public StateManager {
public:
    // Byte stride between consecutive vertices of one vertex buffer.
    //
    // WHY reuse VulkanVertexLayout::k_stride: the value is baked into the
    // pipeline at creation time, so the bind side and the pipeline cannot drift
    // apart if they read the same constant. The caller-supplied offsets are
    // byte offsets into their own buffer (see onBindVertexBuffers), which is
    // why this constant is documentation here rather than an arithmetic term.
    static constexpr uint32_t k_vertex_stride = VulkanVertexLayout::k_stride;
    static constexpr uint32_t k_vertex_binding = VulkanVertexLayout::k_binding;

    // index_type values accepted by StateManager::bindIndexBuffer. No shared
    // enum exists in the cross-backend API, so the backend fixes the encoding
    // here and integrators pass these constants (they coincide with Vulkan's own
    // VkIndexType values, but are NOT relied upon to).
    static constexpr uint32_t k_index_type_uint16 = 0;
    static constexpr uint32_t k_index_type_uint32 = 1;

    // Topology VulkanShaderManager bakes into every graphics pipeline. Recorded
    // values other than this one cannot be honoured by a pipeline built by that
    // manager (see onSetTopology).
    static constexpr uint32_t k_topology_triangle_list =
            static_cast<uint32_t>(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

    // BufferManager handle -> VkBuffer. Vertex and index buffers both come
    // through here; return VK_NULL_HANDLE for an unknown / destroyed handle.
    using BufferResolver = std::function<VkBuffer(uint64_t buffer_handle)>;

    // ShaderManager pipeline handle -> the two objects the state manager needs.
    //
    // WHY a pair and not two callbacks: vkCmdBindDescriptorSets requires a
    // VkPipelineLayout, but the base's onBindDescriptorSets hook does not carry
    // the pipeline, so the layout has to be recovered from the currently bound
    // pipeline. Resolving both at once keeps the two consistent.
    struct VulkanPipelineBinding {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
    };
    using PipelineResolver = std::function<VulkanPipelineBinding(uint64_t pipeline_handle)>;

    // Descriptor-set handle -> VkDescriptorSet (the descriptor pool / set manager
    // lives outside this class). Return VK_NULL_HANDLE when unknown.
    using DescriptorSetResolver = std::function<VkDescriptorSet(uint64_t descriptor_set_handle)>;

    // FramebufferManager handle -> VkFramebuffer. VulkanFramebufferManager
    // exposes vkFramebuffer(handle) for exactly this; wire it straight through.
    using FramebufferResolver = std::function<VkFramebuffer(uint64_t framebuffer_handle)>;

    // Number of dynamic offsets one bound set consumes.
    //
    // WHY this is needed: Vulkan wants a single flat pDynamicOffsets array whose
    // length is the SUM of the dynamicOffsetCounts of the sets being bound, but
    // the base hook hands over one already-concatenated vector and no per-set
    // lengths. Without this the backend cannot know where set N's offsets end.
    // Return the count the set layout was built with (0 for a set whose layout
    // declares no dynamic buffers).
    using DynamicRangeCountResolver =
            std::function<uint32_t(uint64_t descriptor_set_handle, uint32_t set_index)>;

    explicit VulkanStateManager(VulkanRenderer* renderer);
    ~VulkanStateManager() override;

    VulkanStateManager(const VulkanStateManager&) = delete;
    VulkanStateManager& operator=(const VulkanStateManager&) = delete;

    // Optional wiring, install before the first frame. Passing an empty
    // std::function clears the resolver and restores the documented fallback.
    void setBufferResolver(BufferResolver resolver);
    void setPipelineResolver(PipelineResolver resolver);
    void setDescriptorSetResolver(DescriptorSetResolver resolver);
    void setFramebufferResolver(FramebufferResolver resolver);
    void setDynamicRangeCountResolver(DynamicRangeCountResolver resolver);

    // Installs the command buffer the on* hooks record into. Call it after
    // vkResetCommandBuffer + vkBeginCommandBuffer and before applyState(), and
    // pass VK_NULL_HANDLE when no frame is being recorded so the hooks go quiet.
    //
    // frame_index is the frames-in-flight slot (NOT the swapchain image index);
    // it is only used for diagnostics, and an out-of-range value is reported.
    void setCommandBuffer(VkCommandBuffer command_buffer, uint32_t frame_index);
    bool hasCommandBuffer() const;
    uint32_t frameIndex() const;

    // Topology requested through StateManager::setTopology(). Vulkan cannot
    // change it at record time, so the value is only kept for diagnostics.
    uint32_t recordedTopology() const;

protected:
    void onBindPipeline(uint64_t pipeline) override;
    void onBindVertexBuffers(const std::array<uint64_t, 16>& buffers,
                             const std::array<uint32_t, 16>& offsets) override;
    void onBindIndexBuffer(uint64_t buffer, uint32_t index_type) override;
    void onBindDescriptorSets(const std::array<uint64_t, 32>& descriptor_sets,
                              const std::vector<uint32_t>& dynamic_offsets) override;
    void onSetViewport(float x, float y, float width, float height, float min_depth,
                       float max_depth) override;
    void onSetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) override;
    void onSetTopology(uint32_t topology) override;
    void onBindFramebuffer(uint64_t framebuffer) override;

private:
    // All helpers below must be called with mutex_ held.
    VkPipelineLayout bound_pipeline_layout() const;
    uint32_t dynamic_range_count(uint64_t descriptor_set_handle, uint32_t set_index) const;

    VulkanRenderer* renderer_ = nullptr;

    BufferResolver buffer_resolver_;
    PipelineResolver pipeline_resolver_;
    DescriptorSetResolver descriptor_set_resolver_;
    FramebufferResolver framebuffer_resolver_;
    DynamicRangeCountResolver dynamic_range_count_resolver_;

    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    uint32_t frame_index_ = 0;
    uint32_t topology_ = k_topology_triangle_list;
    // Cached so onBindDescriptorSets always names the layout of the pipeline
    // that is actually bound, even when the pipeline itself was not dirty this
    // applyState() round.
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;

    // The base calls the hooks while holding its own mutex, so this one only
    // guards the backend's own state. Never held across a queue submit.
    mutable std::mutex mutex_;

    // Log-once guards: a missing resolver or an unresolvable handle is a
    // configuration bug that repeats every frame, and logcat is the slowest
    // thing in the frame path.
    mutable bool no_buffer_resolver_logged_ = false;
    mutable bool no_pipeline_resolver_logged_ = false;
    mutable bool no_descriptor_set_resolver_logged_ = false;
    mutable bool no_framebuffer_resolver_logged_ = false;
    mutable bool no_pipeline_layout_logged_ = false;
    mutable bool no_dynamic_range_resolver_logged_ = false;
    mutable bool unknown_index_type_logged_ = false;
    mutable bool topology_mismatch_logged_ = false;
    mutable bool frame_index_logged_ = false;
};

} // namespace copper