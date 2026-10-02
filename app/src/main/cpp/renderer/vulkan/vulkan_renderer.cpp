#include "vulkan_renderer.h"
#include "renderer_base.h"
#include "gpu_capabilities.h"

#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>
#include <set>

namespace copper {

class VulkanRenderer::Impl {
public:
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkQueue present_queue = VK_NULL_HANDLE;
    VkQueue compute_queue = VK_NULL_HANDLE;
    uint32_t graphics_queue_family = UINT32_MAX;
    uint32_t present_queue_family = UINT32_MAX;
    uint32_t compute_queue_family = UINT32_MAX;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapchain_images;
    std::vector<VkImageView> swapchain_image_views;
    VkFormat swapchain_format = VK_FORMAT_B8G8R8A8_SRGB;
    VkColorSpaceKHR swapchain_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D swapchain_extent{};
    uint32_t current_frame = 0;
    uint32_t max_frames_in_flight = 3;
    std::vector<VkSemaphore> image_available_semaphores;
    std::vector<VkSemaphore> render_finished_semaphores;
    std::vector<VkFence> in_flight_fences;
    std::vector<VkFence> images_in_flight;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;
    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    bool debug_markers_enabled = false;
    bool validation_enabled = false;
    std::mutex mutex;
    RendererConfig config;
    PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT vkCmdBeginDebugUtilsLabelEXT = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT vkCmdEndDebugUtilsLabelEXT = nullptr;
    PFN_vkCmdInsertDebugUtilsLabelEXT vkCmdInsertDebugUtilsLabelEXT = nullptr;
    PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT = nullptr;
};

VulkanRenderer::VulkanRenderer() : RendererBase(), pImpl(std::make_unique<Impl>()) {}
VulkanRenderer::~VulkanRenderer() = default;

bool VulkanRenderer::initialize(const RendererConfig& config) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    
    if (!RendererBase::initialize(config)) {
        return false;
    }
    
    pImpl->config = config;
    pImpl->max_frames_in_flight = config.maxFramesInFlight;
    pImpl->validation_enabled = config.enableValidation;
    pImpl->debug_markers_enabled = config.enableDebugMarkers;

    if (!createInstance()) return false;
    if (!setupDebugMessenger()) return false;
    if (!pickPhysicalDevice()) return false;
    if (!createLogicalDevice()) return false;
    if (!createSwapchain()) return false;
    if (!createImageViews()) return false;
    if (!createRenderPass()) return false;
    if (!createCommandPool()) return false;
    if (!createCommandBuffers()) return false;
    if (!createSyncObjects()) return false;
    if (!createFramebuffers()) return false;

    pImpl->initialized = true;
    return true;
}

void VulkanRenderer::shutdown() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    
    if (pImpl->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(pImpl->device);
    }

    for (auto fence : pImpl->in_flight_fences) {
        vkDestroyFence(pImpl->device, fence, nullptr);
    }
    for (auto semaphore : pImpl->render_finished_semaphores) {
        vkDestroySemaphore(pImpl->device, semaphore, nullptr);
    }
    for (auto semaphore : pImpl->image_available_semaphores) {
        vkDestroySemaphore(pImpl->device, semaphore, nullptr);
    }

    vkDestroyCommandPool(pImpl->device, pImpl->command_pool, nullptr);
    for (auto framebuffer : pImpl->framebuffers) {
        vkDestroyFramebuffer(pImpl->device, framebuffer, nullptr);
    }
    vkDestroyRenderPass(pImpl->device, pImpl->render_pass, nullptr);
    for (auto image_view : pImpl->swapchain_image_views) {
        vkDestroyImageView(pImpl->device, image_view, nullptr);
    }
    vkDestroySwapchainKHR(pImpl->device, pImpl->swapchain, nullptr);
    vkDestroyPipelineCache(pImpl->device, pImpl->pipeline_cache, nullptr);
    vkDestroyDescriptorPool(pImpl->device, pImpl->descriptor_pool, nullptr);
    vkDestroyDevice(pImpl->device, nullptr);

    if (pImpl->debug_messenger != VK_NULL_HANDLE && pImpl->vkDestroyDebugUtilsMessengerEXT) {
        pImpl->vkDestroyDebugUtilsMessengerEXT(pImpl->instance, pImpl->debug_messenger, nullptr);
    }
    vkDestroySurfaceKHR(pImpl->instance, pImpl->surface, nullptr);
    vkDestroyInstance(pImpl->instance, nullptr);

    RendererBase::shutdown();
    pImpl->initialized = false;
}

bool VulkanRenderer::createInstance() {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Copper Oxide";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "Copper Oxide Engine";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME
    };

    std::vector<const char*> layers;
    if (pImpl->validation_enabled) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();
    create_info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    create_info.ppEnabledLayerNames = layers.data();

    VkResult result = vkCreateInstance(&create_info, nullptr, &pImpl->instance);
    return result == VK_SUCCESS;
}

bool VulkanRenderer::setupDebugMessenger() {
    if (!pImpl->validation_enabled) return true;

    pImpl->vkCreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(pImpl->instance, "vkCreateDebugUtilsMessengerEXT"));
    pImpl->vkDestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(pImpl->instance, "vkDestroyDebugUtilsMessengerEXT"));
    pImpl->vkCmdBeginDebugUtilsLabelEXT = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(pImpl->instance, "vkCmdBeginDebugUtilsLabelEXT"));
    pImpl->vkCmdEndDebugUtilsLabelEXT = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(pImpl->instance, "vkCmdEndDebugUtilsLabelEXT"));
    pImpl->vkCmdInsertDebugUtilsLabelEXT = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(pImpl->instance, "vkCmdInsertDebugUtilsLabelEXT"));
    pImpl->vkSetDebugUtilsObjectNameEXT = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetInstanceProcAddr(pImpl->instance, "vkSetDebugUtilsObjectNameEXT"));

    if (!pImpl->vkCreateDebugUtilsMessengerEXT) return false;

    VkDebugUtilsMessengerCreateInfoEXT create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    create_info.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                     VkDebugUtilsMessageTypeFlagsEXT type,
                                     const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
                                     void* user_data) -> VkBool32 {
        return VK_FALSE;
    };

    return pImpl->vkCreateDebugUtilsMessengerEXT(pImpl->instance, &create_info, nullptr, &pImpl->debug_messenger) == VK_SUCCESS;
}

bool VulkanRenderer::pickPhysicalDevice() {
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(pImpl->instance, &device_count, nullptr);
    if (device_count == 0) return false;

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(pImpl->instance, &device_count, devices.data());

    for (const auto& device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);

        VkPhysicalDeviceFeatures features;
        vkGetPhysicalDeviceFeatures(device, &features);

        // Check if device supports required features
        bool suitable = true;
        // ... feature checks

        if (suitable) {
            pImpl->physical_device = device;
            return true;
        }
    }

    return false;
}

bool VulkanRenderer::createLogicalDevice() {
    // Simplified - in reality would query queue families, etc.
    return true;
}

bool VulkanRenderer::createSwapchain() {
    // Simplified
    return true;
}

bool VulkanRenderer::createImageViews() {
    return true;
}

bool VulkanRenderer::createRenderPass() {
    return true;
}

bool VulkanRenderer::createCommandPool() {
    return true;
}

bool VulkanRenderer::createCommandBuffers() {
    return true;
}

bool VulkanRenderer::createSyncObjects() {
    pImpl->image_available_semaphores.resize(pImpl->max_frames_in_flight);
    pImpl->render_finished_semaphores.resize(pImpl->max_frames_in_flight);
    pImpl->in_flight_fences.resize(pImpl->max_frames_in_flight);
    pImpl->images_in_flight.resize(pImpl->max_frames_in_flight, VK_NULL_HANDLE);

    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < pImpl->max_frames_in_flight; i++) {
        if (vkCreateSemaphore(pImpl->device, &semaphore_info, nullptr, &pImpl->image_available_semaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(pImpl->device, &semaphore_info, nullptr, &pImpl->render_finished_semaphores[i]) != VK_SUCCESS ||
            vkCreateFence(pImpl->device, &fence_info, nullptr, &pImpl->in_flight_fences[i]) != VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

bool VulkanRenderer::createFramebuffers() {
    return true;
}

bool VulkanRenderer::onBeginFrame() {
    // Wait for fence
    vkWaitForFences(pImpl->device, 1, &pImpl->in_flight_fences[pImpl->current_frame], VK_TRUE, UINT64_MAX);
    vkResetFences(pImpl->device, 1, &pImpl->in_flight_fences[pImpl->current_frame]);

    // Acquire next image
    uint32_t image_index;
    VkResult result = vkAcquireNextImageKHR(pImpl->device, pImpl->swapchain, UINT64_MAX,
                                            pImpl->image_available_semaphores[pImpl->current_frame],
                                            VK_NULL_HANDLE, &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // Recreate swapchain
        return false;
    }

    // Reset command buffer
    vkResetCommandBuffer(pImpl->command_buffers[pImpl->current_frame], 0);

    // Begin command buffer
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(pImpl->command_buffers[pImpl->current_frame], &begin_info);

    return true;
}

void VulkanRenderer::onEndFrame() {
    // End command buffer
    vkEndCommandBuffer(pImpl->command_buffers[pImpl->current_frame]);

    // Submit command buffer
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore wait_semaphores[] = {pImpl->image_available_semaphores[pImpl->current_frame]};
    VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = wait_semaphores;
    submit_info.pWaitDstStageMask = wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &pImpl->command_buffers[pImpl->current_frame];
    VkSemaphore signal_semaphores[] = {pImpl->render_finished_semaphores[pImpl->current_frame]};
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = signal_semaphores;

    vkQueueSubmit(pImpl->graphics_queue, 1, &submit_info, pImpl->in_flight_fences[pImpl->current_frame]);
}

void VulkanRenderer::onPresent() {
    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &pImpl->render_finished_semaphores[pImpl->current_frame];
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &pImpl->swapchain;
    // present_info.pImageIndices = &image_index;

    vkQueuePresentKHR(pImpl->present_queue, &present_info);

    pImpl->current_frame = (pImpl->current_frame + 1) % pImpl->max_frames_in_flight;
}

void VulkanRenderer::onResize(uint32_t width, uint32_t height) {
    // Recreate swapchain
}

void VulkanRenderer::onWaitIdle() {
    if (pImpl->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(pImpl->device);
    }
}

RendererBackend VulkanRenderer::getBackendImpl() const {
    return RendererBackend::Vulkan;
}

std::string VulkanRenderer::getGpuRendererStringImpl() const {
    if (pImpl->physical_device != VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pImpl->physical_device, &props);
        return props.deviceName;
    }
    return "Unknown Vulkan Device";
}

std::string VulkanRenderer::getGpuVendorStringImpl() const {
    if (pImpl->physical_device != VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pImpl->physical_device, &props);
        switch (props.vendorID) {
            case 0x13B5: return "ARM"; // Mali
            case 0x5143: return "Qualcomm"; // Adreno
            case 0x1010: return "Imagination"; // PowerVR
            default: return "Unknown";
        }
    }
    return "Unknown";
}

std::string VulkanRenderer::getGpuVersionStringImpl() const {
    if (pImpl->physical_device != VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pImpl->physical_device, &props);
        return std::to_string(VK_VERSION_MAJOR(props.apiVersion)) + "." +
               std::to_string(VK_VERSION_MINOR(props.apiVersion)) + "." +
               std::to_string(VK_VERSION_PATCH(props.apiVersion));
    }
    return "Unknown";
}

} // namespace copper