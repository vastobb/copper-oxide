#include "vulkan_shader_manager.h"

// The accessors used below (device(), renderPass(), pipelineCache(),
// physicalDevice(), instance()) are the ones the integrator must add to
// VulkanRenderer; see the class comment in the header.
#include "vulkan_renderer.h"

#include <android/log.h>

#include <string>
#include <utility>
#include <vector>

#define LOG_TAG "CopperOxide-VK-SM"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

namespace {

// SPIR-V magic, little endian. A mismatch usually means the caller handed us
// something that is not SPIR-V at all (GLSL text, DXBC, ...).
constexpr uint32_t k_spirv_magic = 0x07230203u;

// Copper Oxide's documented guess at a set layout:
//   binding 0        : uniform buffer (camera / per-object UBO)
//   binding 1..n-1   : combined image sampler (textures)
// PipelineLayoutDesc only carries a descriptor COUNT per set, so this is a
// heuristic. Install a real policy with set_binding_policy().
std::vector<VkDescriptorSetLayoutBinding> default_binding_policy(
        uint32_t /*set_index*/, uint32_t binding_count, VkShaderStageFlags stage_flags) {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(binding_count);
    for (uint32_t binding = 0; binding < binding_count; ++binding) {
        VkDescriptorSetLayoutBinding entry{};
        entry.binding = binding;
        entry.descriptorType = binding == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                             : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        entry.descriptorCount = 1;
        entry.stageFlags = stage_flags;
        // WHY pImmutableSamplers stays null: the base API has no sampler
        // allocation path, so samplers are baked into descriptor writes.
        bindings.push_back(entry);
    }
    return bindings;
}

const char* result_name(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        default: return "VkResult(unmapped)";
    }
}

// Owns the memory a VkSpecializationInfo points at; both vectors have to stay
// alive until vkCreate*Pipelines returns.
struct SpecializationBundle {
    std::vector<VkSpecializationMapEntry> entries;
    std::vector<uint32_t> data;

    VkSpecializationInfo info() const {
        VkSpecializationInfo specialization{};
        specialization.mapEntryCount = static_cast<uint32_t>(entries.size());
        specialization.pMapEntries = entries.data();
        specialization.dataSize = static_cast<size_t>(data.size() * sizeof(uint32_t));
        specialization.pData = data.data();
        return specialization;
    }
};

SpecializationBundle build_specialization(
        const std::vector<std::pair<std::string, uint32_t>>& constants,
        const VulkanShaderManager::SpecializationConstantIdResolver& resolver,
        bool& used_id_fallback) {
    SpecializationBundle bundle;
    bundle.entries.reserve(constants.size());
    bundle.data.reserve(constants.size());

    for (size_t i = 0; i < constants.size(); ++i) {
        uint32_t constant_id = 0;
        if (resolver && resolver(constants[i].first, constant_id)) {
            // real reflection id
        } else {
            // Fallback: ids in insertion order. Only correct for shaders whose
            // OpSpecConstant ids were assigned in that same order.
            constant_id = static_cast<uint32_t>(i);
            used_id_fallback = true;
        }

        VkSpecializationMapEntry entry{};
        entry.constantID = constant_id;
        // WHY offset in units of bytes into pData, not an index: Vulkan
        // validates that offset is a multiple of the entry size.
        entry.offset = static_cast<uint32_t>(i * sizeof(uint32_t));
        entry.size = static_cast<uint32_t>(sizeof(uint32_t));
        bundle.entries.push_back(entry);
        bundle.data.push_back(constants[i].second);
    }
    return bundle;
}

void destroy_set_layouts(VkDevice device, std::vector<VkDescriptorSetLayout>& set_layouts) {
    if (device == VK_NULL_HANDLE) {
        set_layouts.clear();
        return;
    }
    for (VkDescriptorSetLayout set_layout : set_layouts) {
        if (set_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        }
    }
    set_layouts.clear();
}

// PipelineLayoutDesc::push_constant_ranges is a flat list of (offset, size)
// pairs. Vulkan wants a real VkPushConstantRange array, so the pairs are
// unpacked here and checked against the device limit -- an out-of-range push
// constant block is a validation error and can take the device down.
bool build_push_constant_ranges(const std::vector<uint32_t>& pairs,
                                VkShaderStageFlags stage_flags,
                                uint32_t max_push_constant_size,
                                std::vector<VkPushConstantRange>& out,
                                std::string& error) {
    if (pairs.empty()) {
        return true;
    }
    if ((pairs.size() % 2) != 0) {
        error = "push_constant_ranges must be (offset, size) pairs, got " +
                std::to_string(pairs.size()) + " values";
        return false;
    }

    out.reserve(pairs.size() / 2);
    for (size_t i = 0; i < pairs.size(); i += 2) {
        const uint32_t offset = pairs[i];
        const uint32_t size = pairs[i + 1];
        if (size == 0) {
            error = "push constant range " + std::to_string(i / 2) + " has zero size";
            return false;
        }
        if ((offset % 4) != 0) {
            error = "push constant range " + std::to_string(i / 2) + " offset " +
                    std::to_string(offset) + " is not 4 byte aligned";
            return false;
        }
        if (max_push_constant_size != 0 && (offset + size) > max_push_constant_size) {
            error = "push constant range " + std::to_string(i / 2) + " exceeds maxPushConstantsSize " +
                    std::to_string(max_push_constant_size);
            return false;
        }

        VkPushConstantRange range{};
        range.stageFlags = stage_flags;
        range.offset = offset;
        range.size = size;
        out.push_back(range);
    }
    return true;
}

// Builds one VkDescriptorSetLayout per PipelineLayoutDesc::set_layouts entry
// and then the VkPipelineLayout that references them. On any failure everything
// created so far is destroyed before returning, so a rejected pipeline never
// leaks a layout.
bool build_pipeline_layout(VkDevice device,
                           uint32_t max_push_constant_size,
                           const PipelineLayoutDesc& desc,
                           VkShaderStageFlags stage_flags,
                           const VulkanShaderManager::BindingPolicy& policy,
                           std::vector<VkDescriptorSetLayout>& set_layouts,
                           VkPipelineLayout& pipeline_layout,
                           std::string& error) {
    set_layouts.clear();
    pipeline_layout = VK_NULL_HANDLE;

    set_layouts.reserve(desc.set_layouts.size());
    for (size_t set_index = 0; set_index < desc.set_layouts.size(); ++set_index) {
        const std::vector<VkDescriptorSetLayoutBinding> bindings =
                policy(static_cast<uint32_t>(set_index), desc.set_layouts[set_index], stage_flags);

        VkDescriptorSetLayoutCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        create_info.bindingCount = static_cast<uint32_t>(bindings.size());
        create_info.pBindings = bindings.data();

        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        const VkResult result =
                vkCreateDescriptorSetLayout(device, &create_info, nullptr, &set_layout);
        if (result != VK_SUCCESS) {
            error = "vkCreateDescriptorSetLayout(set " + std::to_string(set_index) + ") failed: " +
                    result_name(result);
            destroy_set_layouts(device, set_layouts);
            return false;
        }
        // An empty set still gets a layout so that binding the set is legal.
        set_layouts.push_back(set_layout);
    }

    std::vector<VkPushConstantRange> push_constant_ranges;
    if (!build_push_constant_ranges(desc.push_constant_ranges, stage_flags,
                                    max_push_constant_size, push_constant_ranges, error)) {
        destroy_set_layouts(device, set_layouts);
        return false;
    }

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = static_cast<uint32_t>(push_constant_ranges.size());
    layout_info.pPushConstantRanges = push_constant_ranges.empty() ? nullptr
                                                                   : push_constant_ranges.data();

    const VkResult result = vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout);
    if (result != VK_SUCCESS) {
        error = std::string("vkCreatePipelineLayout failed: ") + result_name(result);
        destroy_set_layouts(device, set_layouts);
        return false;
    }
    return true;
}

VkPipelineViewportStateCreateInfo make_viewport_scissor_state() {
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    // WHY pViewports / pScissors stay null: both are VK_DYNAMIC_STATE_* below,
    // so the values come from vkCmdSetViewport / vkCmdSetScissor at record time.
    // That keeps one pipeline valid for every surface size instead of forcing a
    // rebuild on resize, at the cost of the state manager having to record
    // both at least once per command buffer (it does).
    return viewport_state;
}

VkPipelineColorBlendStateCreateInfo make_color_blend_state() {
    // WHY: VulkanRenderer's render pass declares exactly one color attachment
    // (swapchain_format_), so the blend state must describe exactly one
    // attachment or the pipeline is incompatible with the render pass.
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = VK_FALSE;
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.logicOpEnable = VK_FALSE;
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    return blend;
}

VkPipelineDepthStencilStateCreateInfo make_depth_stencil_state() {
    // WHY: the renderer's render pass has no depth attachment, so depth test
    // and depth write must stay off or the pipeline does not match the pass.
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_FALSE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    depth_stencil.depthBoundsTestEnable = VK_FALSE;
    depth_stencil.stencilTestEnable = VK_FALSE;
    return depth_stencil;
}

VkPipelineRasterizationStateCreateInfo make_rasterization_state() {
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.depthClampEnable = VK_FALSE;
    rasterization.rasterizerDiscardEnable = VK_FALSE;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    // WHY cull BACK + CCW front face: Copper Oxide's first triangle is wound
    // counter-clockwise in Vulkan's screen space (y flipped versus OpenGL), and
    // back faces are the winding a normal (non two-sided) surface does not
    // show. Two-sided materials can be handled by the fragment shader or by a
    // dedicated pipeline variant.
    rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // WHY pViewports / pScissors stay null: both are dynamic state below.
    rasterization.lineWidth = 1.0f;
    return rasterization;
}

VkPipelineMultisampleStateCreateInfo make_multisample_state() {
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    // WHY 1 sample: the render pass attachment uses VK_SAMPLE_COUNT_1_BIT and
    // the two have to match exactly.
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    return multisample;
}

} // namespace

VkVertexInputBindingDescription VulkanVertexLayout::binding_description() {
    VkVertexInputBindingDescription binding{};
    binding.binding = k_binding;
    binding.stride = k_stride;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return binding;
}

std::vector<VkVertexInputAttributeDescription> VulkanVertexLayout::attribute_descriptions() {
    std::vector<VkVertexInputAttributeDescription> attributes;
    attributes.reserve(k_attribute_count);

    VkVertexInputAttributeDescription position{};
    position.location = k_location_position;
    position.binding = k_binding;
    position.format = position_format();
    position.offset = 0;
    attributes.push_back(position);

    VkVertexInputAttributeDescription color{};
    color.location = k_location_color;
    color.binding = k_binding;
    color.format = color_format();
    color.offset = 12;
    attributes.push_back(color);

    return attributes;
}

VkFormat VulkanVertexLayout::position_format() {
    return VK_FORMAT_R32G32B32_SFLOAT;
}

VkFormat VulkanVertexLayout::color_format() {
    // WHY UNORM and not SRGB: the swapchain view is the sRGB view, so it
    // already performs the linear -> sRGB conversion on store. Baking SRGB in
    // here as well would double-encode the colour.
    return VK_FORMAT_R8G8B8A8_UNORM;
}

VulkanShaderManager::VulkanShaderManager(VulkanRenderer* renderer)
    : renderer_(renderer), binding_policy_(&default_binding_policy) {}

VulkanShaderManager::~VulkanShaderManager() {
    // ShaderManager::shutdown() destroys every pipeline and then every module;
    // this only guards against a manager that was never shut down.
    if (renderer_ == nullptr || renderer_->device() == VK_NULL_HANDLE) {
        return;
    }
    const VkDevice device = renderer_->device();
    for (auto& [handle, pipeline] : pipelines_) {
        for (VkDescriptorSetLayout set_layout : pipeline.descriptor_set_layouts) {
            vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        }
        if (pipeline.pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipeline.pipeline_layout, nullptr);
        }
        if (pipeline.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline.pipeline, nullptr);
        }
    }
    for (auto& [handle, module] : modules_) {
        if (module.module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(renderer_->device(), module.module, nullptr);
        }
    }
    pipelines_.clear();
    modules_.clear();
}

void VulkanShaderManager::set_binding_policy(BindingPolicy policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    binding_policy_ = policy ? std::move(policy) : BindingPolicy(&default_binding_policy);
}

void VulkanShaderManager::set_specialization_constant_id_resolver(
        SpecializationConstantIdResolver resolver) {
    std::lock_guard<std::mutex> lock(mutex_);
    specialization_constant_id_resolver_ = std::move(resolver);
}

VkDescriptorSetLayout VulkanShaderManager::descriptor_set_layout(uint64_t pipeline,
                                                                 uint32_t set_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = pipelines_.find(pipeline);
    if (it == pipelines_.end() || set_index >= it->second.descriptor_set_layouts.size()) {
        return VK_NULL_HANDLE;
    }
    return it->second.descriptor_set_layouts[set_index];
}

VkPipeline VulkanShaderManager::pipeline(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = pipelines_.find(handle);
    return it == pipelines_.end() ? VK_NULL_HANDLE : it->second.pipeline;
}

VkPipelineLayout VulkanShaderManager::pipelineLayout(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = pipelines_.find(handle);
    return it == pipelines_.end() ? VK_NULL_HANDLE : it->second.pipeline_layout;
}

PFN_vkSetDebugUtilsObjectNameEXT VulkanShaderManager::debug_object_name_fn() const {
    if (debug_object_name_fn_loaded_) {
        return debug_object_name_fn_;
    }
    debug_object_name_fn_loaded_ = true;
    if (renderer_ != nullptr && renderer_->instance() != VK_NULL_HANDLE) {
        // WHY resolved here and not cached by the renderer: the renderer's own
        // debug pointers are only populated when validation is enabled, while
        // VK_EXT_debug_utils is always requested at instance creation.
        debug_object_name_fn_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
                vkGetInstanceProcAddr(renderer_->instance(), "vkSetDebugUtilsObjectNameEXT"));
    }
    if (debug_object_name_fn_ == nullptr) {
        LOGD("VK_EXT_debug_utils unavailable, object names fall back to logcat");
    }
    return debug_object_name_fn_;
}

void VulkanShaderManager::set_object_name(VkObjectType type, uint64_t object_handle,
                                         const std::string& name) const {
    if (object_handle == 0) {
        return;
    }
    PFN_vkSetDebugUtilsObjectNameEXT set_name = debug_object_name_fn();
    if (set_name == nullptr || renderer_ == nullptr || renderer_->instance() == VK_NULL_HANDLE) {
        LOGD("object %llu named '%s' in logcat only (VK_EXT_debug_utils unavailable)",
             static_cast<unsigned long long>(object_handle), name.c_str());
        return;
    }

    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = type;
    info.objectHandle = object_handle;
    info.pObjectName = name.c_str();
    set_name(renderer_->device(), &info);
}

bool VulkanShaderManager::onCreateShader(uint64_t handle, ShaderStage stage,
                                         const std::vector<uint32_t>& spirv,
                                         const std::string& entry_point) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (renderer_ == nullptr || renderer_->device() == VK_NULL_HANDLE) {
        LOGE("onCreateShader: Vulkan device is not ready");
        return false;
    }
    // pCode is never allowed to be null: vkCreateShaderModule dereferences it
    // without checking, so an empty translation unit has to be rejected here.
    if (spirv.empty() || spirv.data() == nullptr) {
        LOGE("onCreateShader: empty SPIR-V payload for handle %llu",
             static_cast<unsigned long long>(handle));
        return false;
    }
    if ((spirv.size() % 4) != 0) {
        LOGE("onCreateShader: SPIR-V word count %zu is not a multiple of 4",
             spirv.size());
        return false;
    }
    if (spirv[0] != k_spirv_magic) {
        // Not fatal by itself (the driver reports the real error), but it is
        // almost always a wrong input format worth shouting about.
        LOGW("onCreateShader: SPIR-V magic is 0x%08x, expected 0x07230203",
             spirv[0]);
    }

    VkShaderModuleCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = spirv.size() * sizeof(uint32_t);
    create_info.pCode = spirv.data();

    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult result = vkCreateShaderModule(renderer_->device(), &create_info, nullptr, &module);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateShaderModule failed: %s", result_name(result));
        return false;
    }

    ShaderModuleData data;
    data.module = module;
    data.stage = stage;
    data.entry_point = entry_point.empty() ? "main" : entry_point;
    modules_[handle] = std::move(data);
    return true;
}

bool VulkanShaderManager::onCreateShaderFromGLSL(uint64_t /*handle*/, ShaderStage /*stage*/,
                                                 const std::string& /*glsl_source*/,
                                                 const std::string& /*entry_point*/,
                                                 const std::vector<std::string>& /*defines*/) {
    // Reached only if a caller invokes this hook directly. The normal path does
    // not: ShaderManager::createShaderFromGLSL asks compilesGlslNatively(), gets
    // false from Vulkan, compiles GLSL to SPIR-V itself, and then calls
    // onCreateShader with the words - which is what makes vkCreateShaderModule
    // work. The hook exists because the base declares it pure virtual, so it has
    // to say something honest rather than pretend.
    LOGW("VulkanShaderManager does not compile GLSL: ShaderManager does the translation. "
         "Call ShaderManager::createShaderFromGLSL instead of reaching for this hook.");
    return false;
}

void VulkanShaderManager::onDestroyShader(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    // The base refuses to destroy a shader a live pipeline still references,
    // so reaching here with a live pipeline means the caller violated that.
    const auto it = modules_.find(handle);
    if (it == modules_.end()) {
        return;
    }
    if (it->second.module == VK_NULL_HANDLE) {
        modules_.erase(it);
        return;
    }
    if (renderer_ == nullptr || renderer_->device() == VK_NULL_HANDLE) {
        LOGW("onDestroyShader: device already gone, leaking shader module for handle %llu",
             static_cast<unsigned long long>(handle));
        modules_.erase(it);
        return;
    }
    vkDestroyShaderModule(renderer_->device(), it->second.module, nullptr);
    modules_.erase(it);
}

bool VulkanShaderManager::onCreateGraphicsPipeline(uint64_t handle, uint64_t vertex_shader,
                                                   uint64_t fragment_shader,
                                                   const PipelineLayoutDesc& layout) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (renderer_ == nullptr || renderer_->device() == VK_NULL_HANDLE) {
        LOGE("onCreateGraphicsPipeline: Vulkan device is not ready");
        return false;
    }
    const VkRenderPass render_pass = renderer_->renderPass();
    if (render_pass == VK_NULL_HANDLE) {
        LOGE("onCreateGraphicsPipeline: renderer has no render pass yet");
        return false;
    }

    const auto vertex_it = modules_.find(vertex_shader);
    const auto fragment_it = modules_.find(fragment_shader);
    if (vertex_it == modules_.end() || vertex_it->second.module == VK_NULL_HANDLE) {
        LOGE("onCreateGraphicsPipeline: vertex shader %llu is not a live module",
             static_cast<unsigned long long>(vertex_shader));
        return false;
    }
    if (fragment_it == modules_.end() || fragment_it->second.module == VK_NULL_HANDLE) {
        LOGE("onCreateGraphicsPipeline: fragment shader %llu is not a live module",
             static_cast<unsigned long long>(fragment_shader));
        return false;
    }

    const VkDevice device = renderer_->device();

    // Push constant limit comes from the physical device, so that an
    // out-of-bounds range is refused here instead of by the driver.
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(renderer_->physicalDevice(), &properties);

    PipelineData data;
    std::string error;
    const VkShaderStageFlags layout_stage_flags = VK_SHADER_STAGE_VERTEX_BIT |
                                                  VK_SHADER_STAGE_FRAGMENT_BIT;
    if (!build_pipeline_layout(device, properties.limits.maxPushConstantsSize, layout,
                               layout_stage_flags, binding_policy_, data.descriptor_set_layouts,
                               data.pipeline_layout, error)) {
        LOGE("onCreateGraphicsPipeline: %s", error.c_str());
        return false;
    }

    // Specialization constants have to be handed over at pipeline creation.
    bool used_id_fallback = false;
    SpecializationBundle vertex_spec = build_specialization(
            vertex_it->second.specialization_constants, specialization_constant_id_resolver_,
            used_id_fallback);
    SpecializationBundle fragment_spec = build_specialization(
            fragment_it->second.specialization_constants, specialization_constant_id_resolver_,
            used_id_fallback);
    if (used_id_fallback && !specialization_fallback_logged_) {
        specialization_fallback_logged_ = true;
        LOGW("specialization constants resolved in insertion order (no reflection "
             "resolver installed); ids only match shaders that declare their "
             "constants in that order");
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    // WHY the locals: VkSpecializationInfo is copied by value into the stage,
    // but the pointers inside it (pMapEntries / pData) point at the bundle
    // vectors, which have to stay alive until vkCreateGraphicsPipelines
    // returns, so the structs cannot be temporaries.
    const VkSpecializationInfo vertex_spec_info = vertex_spec.info();
    const VkSpecializationInfo fragment_spec_info = fragment_spec.info();

    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_it->second.module;
    stages[0].pName = vertex_it->second.entry_point.c_str();
    stages[0].pSpecializationInfo = vertex_spec.entries.empty() ? nullptr : &vertex_spec_info;

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_it->second.module;
    stages[1].pName = fragment_it->second.entry_point.c_str();
    stages[1].pSpecializationInfo =
            fragment_spec.entries.empty() ? nullptr : &fragment_spec_info;

    // ---- Fixed function state -------------------------------------------------
    const VkVertexInputBindingDescription vertex_binding = VulkanVertexLayout::binding_description();
    const std::vector<VkVertexInputAttributeDescription> attributes =
            VulkanVertexLayout::attribute_descriptions();

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    // Topology is baked in: Vulkan has no runtime topology command, which is
    // why VulkanStateManager::onSetTopology only records the value.
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = VK_FALSE;

    const VkPipelineViewportStateCreateInfo viewport_state = make_viewport_scissor_state();

    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                              VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    VkPipelineColorBlendStateCreateInfo color_blend = make_color_blend_state();
    VkPipelineDepthStencilStateCreateInfo depth_stencil = make_depth_stencil_state();
    const VkPipelineRasterizationStateCreateInfo rasterization = make_rasterization_state();
    const VkPipelineMultisampleStateCreateInfo multisample = make_multisample_state();

    VkGraphicsPipelineCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    create_info.stageCount = 2;
    create_info.pStages = stages;
    create_info.pVertexInputState = &vertex_input;
    create_info.pInputAssemblyState = &input_assembly;
    create_info.pViewportState = &viewport_state;
    create_info.pRasterizationState = &rasterization;
    create_info.pMultisampleState = &multisample;
    create_info.pDepthStencilState = &depth_stencil;
    create_info.pColorBlendState = &color_blend;
    create_info.pDynamicState = &dynamic_state;
    // WHY the renderer's swapchain render pass: that is the pass the clear path
    // in VulkanRenderer::onEndFrame() begins, so a pipeline compatible with it
    // can be used there without building a second pass.
    create_info.renderPass = render_pass;
    create_info.subpass = 0;
    create_info.basePipelineHandle = VK_NULL_HANDLE;
    create_info.basePipelineIndex = -1;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(device, renderer_->pipelineCache(), 1,
                                                      &create_info, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines failed: %s", result_name(result));
        destroy_set_layouts(device, data.descriptor_set_layouts);
        if (data.pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, data.pipeline_layout, nullptr);
        }
        return false;
    }

    data.pipeline = pipeline;
    data.compute = false;
    pipelines_[handle] = std::move(data);
    LOGD("graphics pipeline %llu created (stride %u, %u descriptor sets, %zu push constant ranges)",
         static_cast<unsigned long long>(handle), VulkanVertexLayout::k_stride,
         static_cast<uint32_t>(layout.set_layouts.size()), layout.push_constant_ranges.size() / 2);
    return true;
}

bool VulkanShaderManager::onCreateComputePipeline(uint64_t handle, uint64_t compute_shader,
                                                  const PipelineLayoutDesc& layout) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (renderer_ == nullptr || renderer_->device() == VK_NULL_HANDLE) {
        LOGE("onCreateComputePipeline: Vulkan device is not ready");
        return false;
    }
    const auto compute_it = modules_.find(compute_shader);
    if (compute_it == modules_.end() || compute_it->second.module == VK_NULL_HANDLE) {
        LOGE("onCreateComputePipeline: shader %llu is not a live module",
             static_cast<unsigned long long>(compute_shader));
        return false;
    }

    const VkDevice device = renderer_->device();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(renderer_->physicalDevice(), &properties);

    PipelineData data;
    std::string error;
    if (!build_pipeline_layout(device, properties.limits.maxPushConstantsSize, layout,
                               VK_SHADER_STAGE_COMPUTE_BIT, binding_policy_,
                               data.descriptor_set_layouts, data.pipeline_layout, error)) {
        LOGE("onCreateComputePipeline: %s", error.c_str());
        return false;
    }

    bool used_id_fallback = false;
    SpecializationBundle compute_spec = build_specialization(
            compute_it->second.specialization_constants, specialization_constant_id_resolver_,
            used_id_fallback);
    const VkSpecializationInfo compute_spec_info = compute_spec.info();
    if (used_id_fallback && !specialization_fallback_logged_) {
        specialization_fallback_logged_ = true;
        LOGW("specialization constants resolved in insertion order (no reflection "
             "resolver installed)");
    }

    VkComputePipelineCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    create_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    create_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    create_info.stage.module = compute_it->second.module;
    create_info.stage.pName = compute_it->second.entry_point.c_str();
    create_info.stage.pSpecializationInfo =
            compute_spec.entries.empty() ? nullptr : &compute_spec_info;
    // No pNext: a compute pipeline has no render pass, no vertex input and no
    // fixed function state at all.
    create_info.layout = data.pipeline_layout;
    create_info.basePipelineHandle = VK_NULL_HANDLE;
    create_info.basePipelineIndex = -1;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result =
            vkCreateComputePipelines(device, renderer_->pipelineCache(), 1, &create_info, nullptr,
                                     &pipeline);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateComputePipelines failed: %s", result_name(result));
        destroy_set_layouts(device, data.descriptor_set_layouts);
        if (data.pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, data.pipeline_layout, nullptr);
        }
        return false;
    }

    data.pipeline = pipeline;
    data.compute = true;
    pipelines_[handle] = std::move(data);
    return true;
}

void VulkanShaderManager::onDestroyPipeline(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = pipelines_.find(handle);
    if (it == pipelines_.end()) {
        return;
    }
    PipelineData& data = it->second;
    if (renderer_ == nullptr || renderer_->device() == VK_NULL_HANDLE) {
        LOGW("onDestroyPipeline: device already gone, leaking pipeline %llu",
             static_cast<unsigned long long>(handle));
        pipelines_.erase(it);
        return;
    }

    const VkDevice device = renderer_->device();
    destroy_set_layouts(device, data.descriptor_set_layouts);
    if (data.pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, data.pipeline_layout, nullptr);
        data.pipeline_layout = VK_NULL_HANDLE;
    }
    if (data.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, data.pipeline, nullptr);
        data.pipeline = VK_NULL_HANDLE;
    }
    pipelines_.erase(it);
}

void VulkanShaderManager::onSetShaderDebugName(uint64_t handle, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = modules_.find(handle);
    if (it == modules_.end()) {
        return;
    }
    it->second.debug_name = name;
    set_object_name(VK_OBJECT_TYPE_SHADER_MODULE,
                    reinterpret_cast<uint64_t>(it->second.module), name);
}

void VulkanShaderManager::onSetPipelineDebugName(uint64_t handle, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = pipelines_.find(handle);
    if (it == pipelines_.end()) {
        return;
    }
    it->second.debug_name = name;
    set_object_name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(it->second.pipeline),
                    name);
}

void VulkanShaderManager::onAddSpecializationConstant(uint64_t shader_handle, const std::string& name,
                                                      uint32_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = modules_.find(shader_handle);
    if (it == modules_.end()) {
        return;
    }
    // The base stores name -> value in a map, so re-adding a name must replace
    // the value instead of appending a second entry with the same name: Vulkan
    // would see two map entries for one OpSpecConstant.
    for (auto& [existing_name, existing_value] : it->second.specialization_constants) {
        if (existing_name == name) {
            existing_value = value;
            return;
        }
    }
    it->second.specialization_constants.emplace_back(name, value);
}

} // namespace copper
