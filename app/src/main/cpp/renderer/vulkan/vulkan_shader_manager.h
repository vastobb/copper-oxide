#pragma once

// WHY: the platform guard has to be defined before vulkan.h is pulled in,
// otherwise the Android surface/window types stay invisible.
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shader_manager.h"

namespace copper {

class VulkanRenderer;

// Vertex format Copper Oxide bakes into its first real draw call:
//
//   binding 0, stride 28
//     location 0 : vec3 position, R32G32B32_SFLOAT, offset 0
//     location 1 : vec4 colour,    R8G8B8A8_UNORM,   offset 12
//
// WHY: VkPipelineVertexInputStateCreateInfo bakes the vertex format into the
// pipeline, so a different mesh format means a different pipeline rather than
// a different bind call. Keeping the whole description in one small struct
// makes swapping the format a single-place edit (and the only place that has
// to agree with the stride the CPU-side vertex writer uses).
struct VulkanVertexLayout {
    static constexpr uint32_t k_binding = 0;
    static constexpr uint32_t k_stride = 28;  // 12 bytes position + 4 bytes colour
    static constexpr uint32_t k_location_position = 0;
    static constexpr uint32_t k_location_color = 1;
    static constexpr uint32_t k_attribute_count = 2;

    static VkVertexInputBindingDescription binding_description();
    static std::vector<VkVertexInputAttributeDescription> attribute_descriptions();
    static VkFormat position_format();
    static VkFormat color_format();
};

// Vulkan backend for ShaderManager.
//
// The base class owns handle allocation, caching and the debug-name /
// specialization bookkeeping; everything below is real Vulkan work driven from
// the base's on* hooks.
//
// INTEGRATOR NOTE: vulkan_renderer.h still declares an inline stub of this
// class name (and of VulkanFramebufferManager / VulkanStateManager). Those
// stubs must be deleted from vulkan_renderer.h and replaced with includes of
// these headers, otherwise the two definitions of `copper::VulkanShaderManager`
// collide.
class VulkanShaderManager : public ShaderManager {
public:
    // Turns one entry of PipelineLayoutDesc::set_layouts (a descriptor COUNT)
    // into the bindings of a VkDescriptorSetLayout for that set.
    //
    // WHY a hook: PipelineLayoutDesc carries counts, not descriptor types, so
    // the backend has to guess. set_binding_policy() installs the real policy
    // once the engine knows the shader layouts; the built-in default is the
    // documented heuristic (see default_binding_policy in the .cpp).
    using BindingPolicy = std::function<std::vector<VkDescriptorSetLayoutBinding>(
            uint32_t set_index, uint32_t binding_count, VkShaderStageFlags stage_flags)>;

    // Maps a specialization constant NAME to the OpSpecConstant id declared in
    // the SPIR-V. Vulkan addresses constants numerically, so resolving names
    // needs a reflection pass (SPIRV-Tools / spirv-cross / glslang reflection)
    // that is not linked into this build. Without a resolver the backend falls
    // back to "ids in insertion order" and logs it -- see the .cpp.
    using SpecializationConstantIdResolver = std::function<bool(const std::string& name,
                                                                uint32_t& constant_id)>;

    explicit VulkanShaderManager(VulkanRenderer* renderer);
    ~VulkanShaderManager() override;

    VulkanShaderManager(const VulkanShaderManager&) = delete;
    VulkanShaderManager& operator=(const VulkanShaderManager&) = delete;

    // Optional hooks, set by the integrator before any pipeline is created.
    void set_binding_policy(BindingPolicy policy);
    void set_specialization_constant_id_resolver(SpecializationConstantIdResolver resolver);

    // The pipeline owns the VkDescriptorSetLayouts it was built with, and
    // allocating a VkDescriptorSet needs the layout, so the integrator reads
    // them back here. Returns VK_NULL_HANDLE for an unknown pipeline or set.
    VkDescriptorSetLayout descriptor_set_layout(uint64_t pipeline, uint32_t set_index) const;

    // vkCmdBindPipeline and vkCmdBindDescriptorSets both need the raw objects.
    // Returns VK_NULL_HANDLE for an unknown or destroyed pipeline handle.
    VkPipeline pipeline(uint64_t handle) const;
    VkPipelineLayout pipelineLayout(uint64_t handle) const;

protected:
    bool onCreateShader(uint64_t handle, ShaderStage stage, const std::vector<uint32_t>& spirv,
                        const std::string& entry_point) override;
    bool onCreateShaderFromGLSL(uint64_t handle, ShaderStage stage, const std::string& glsl_source,
                                const std::string& entry_point,
                                const std::vector<std::string>& defines) override;
    void onDestroyShader(uint64_t handle) override;
    bool onCreateGraphicsPipeline(uint64_t handle, uint64_t vertex_shader, uint64_t fragment_shader,
                                  const PipelineLayoutDesc& layout) override;
    bool onCreateComputePipeline(uint64_t handle, uint64_t compute_shader,
                                 const PipelineLayoutDesc& layout) override;
    void onDestroyPipeline(uint64_t handle) override;
    void onSetShaderDebugName(uint64_t handle, const std::string& name) override;
    void onSetPipelineDebugName(uint64_t handle, const std::string& name) override;
    void onAddSpecializationConstant(uint64_t shader_handle, const std::string& name,
                                     uint32_t value) override;

private:
    struct ShaderModuleData {
        VkShaderModule module = VK_NULL_HANDLE;
        ShaderStage stage = ShaderStage::Vertex;
        std::string entry_point = "main";
        std::string debug_name;
        // Vulkan specialization constants are only legal at
        // vkCreateGraphicsPipelines / vkCreateComputePipelines time through
        // VkSpecializationInfo, never after the module exists. The base API
        // therefore only records name/value here and the pipeline hooks below
        // consume the list when the pipeline is built. That is the only place
        // the values can legally be applied.
        std::vector<std::pair<std::string, uint32_t>> specialization_constants;
    };

    struct PipelineData {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        std::vector<VkDescriptorSetLayout> descriptor_set_layouts;
        bool compute = false;
        std::string debug_name;
    };

    // VK_EXT_debug_utils object naming. The function pointer is resolved once,
    // lazily, from the instance; when it is unreachable every helper degrades
    // to a debug log instead of failing.
    PFN_vkSetDebugUtilsObjectNameEXT debug_object_name_fn() const;
    void set_object_name(VkObjectType type, uint64_t object_handle, const std::string& name) const;

    VulkanRenderer* renderer_ = nullptr;
    std::unordered_map<uint64_t, ShaderModuleData> modules_;
    std::unordered_map<uint64_t, PipelineData> pipelines_;
    BindingPolicy binding_policy_;
    SpecializationConstantIdResolver specialization_constant_id_resolver_;

    // The base calls the on* hooks while holding ITS lock, so this mutex only
    // guards the backend's own maps. It is never held across
    // vkQueueSubmit / vkDeviceWaitIdle (this backend never submits).
    mutable std::mutex mutex_;
    mutable PFN_vkSetDebugUtilsObjectNameEXT debug_object_name_fn_ = nullptr;
    mutable bool debug_object_name_fn_loaded_ = false;
    bool specialization_fallback_logged_ = false;
};

} // namespace copper
