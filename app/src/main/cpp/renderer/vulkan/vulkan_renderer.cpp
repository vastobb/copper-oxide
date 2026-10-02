#include "vulkan_renderer.h"
#include "renderer_base.h"
#include "gpu_capabilities.h"

#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>
#include <set>

namespace {
// KHR entry points (Android requires explicit loading for KHR/EXT calls,
// core Vulkan entry points are directly linkable)
static PFN_vkCreateAndroidSurfaceKHR fp_vkCreateAndroidSurfaceKHR = nullptr;
static PFN_vkDestroySurfaceKHR fp_vkDestroySurfaceKHR = nullptr;
static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR fp_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
static PFN_vkGetPhysicalDeviceSurfaceFormatsKHR fp_vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
static PFN_vkGetPhysicalDeviceSurfacePresentModesKHR fp_vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;
static PFN_vkGetPhysicalDeviceSurfaceSupportKHR fp_vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
static PFN_vkCreateSwapchainKHR fp_vkCreateSwapchainKHR = nullptr;
static PFN_vkDestroySwapchainKHR fp_vkDestroySwapchainKHR = nullptr;
static PFN_vkGetSwapchainImagesKHR fp_vkGetSwapchainImagesKHR = nullptr;
static PFN_vkAcquireNextImageKHR fp_vkAcquireNextImageKHR = nullptr;
static PFN_vkQueuePresentKHR fp_vkQueuePresentKHR = nullptr;

bool load_instance_ext(VkInstance instance) {
    fp_vkCreateAndroidSurfaceKHR = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
        vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR"));
    fp_vkDestroySurfaceKHR = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
        vkGetInstanceProcAddr(instance, "vkDestroySurfaceKHR"));
    fp_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    fp_vkGetPhysicalDeviceSurfaceFormatsKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
    fp_vkGetPhysicalDeviceSurfacePresentModesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfacePresentModesKHR"));
    fp_vkGetPhysicalDeviceSurfaceSupportKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceSupportKHR"));
    return fp_vkCreateAndroidSurfaceKHR && fp_vkDestroySurfaceKHR &&
           fp_vkGetPhysicalDeviceSurfaceCapabilitiesKHR && fp_vkGetPhysicalDeviceSurfaceFormatsKHR &&
           fp_vkGetPhysicalDeviceSurfacePresentModesKHR && fp_vkGetPhysicalDeviceSurfaceSupportKHR;
}

bool load_device_ext(VkDevice device) {
    fp_vkCreateSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
        vkGetDeviceProcAddr(device, "vkCreateSwapchainKHR"));
    fp_vkDestroySwapchainKHR = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
        vkGetDeviceProcAddr(device, "vkDestroySwapchainKHR"));
    fp_vkGetSwapchainImagesKHR = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
        vkGetDeviceProcAddr(device, "vkGetSwapchainImagesKHR"));
    fp_vkAcquireNextImageKHR = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
        vkGetDeviceProcAddr(device, "vkAcquireNextImageKHR"));
    fp_vkQueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(
        vkGetDeviceProcAddr(device, "vkQueuePresentKHR"));
    return fp_vkCreateSwapchainKHR && fp_vkGetSwapchainImagesKHR &&
           fp_vkAcquireNextImageKHR && fp_vkQueuePresentKHR && fp_vkDestroySwapchainKHR;
}
} // namespace

namespace copper {

VulkanRenderer::VulkanRenderer() : RendererBase() {}
VulkanRenderer::~VulkanRenderer() = default;

bool VulkanRenderer::initialize(const RendererConfig& config) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    
    if (initialized_) {
        return true;
    }
    
    config_ = config;
    // Note: max_frames_in_flight is used in base class
    
    if (!create_instance()) return false;
    if (!load_instance_ext(instance_)) return false;
    if (!setup_debug_messenger()) return false;
    if (!select_physical_device()) return false;
    if (!create_logical_device()) return false;
    if (!create_surface()) return false;
    if (!create_swapchain()) return false;
    if (!create_sync_objects()) return false;
    
    initialized_ = true;
    return true;
}

void VulkanRenderer::shutdown() {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    for (auto fence : in_flight_fences_) {
        vkDestroyFence(device_, fence, nullptr);
    }
    for (auto semaphore : render_finished_semaphores_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    for (auto semaphore : image_available_semaphores_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }

    vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
    vkDestroyDevice(device_, nullptr);

    if (debug_messenger_ != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT_) {
        vkDestroyDebugUtilsMessengerEXT_(instance_, debug_messenger_, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE && fp_vkDestroySurfaceKHR) {
        fp_vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    vkDestroyInstance(instance_, nullptr);

    RendererBase::shutdown();
    initialized_ = false;
}

bool VulkanRenderer::create_instance() {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Copper Oxide";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "Copper Oxide Engine";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_1;

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        "VK_KHR_android_surface",
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME
    };

    std::vector<const char*> layers;
    if (config_.enableValidation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();
    create_info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    create_info.ppEnabledLayerNames = layers.data();

    VkResult result = vkCreateInstance(&create_info, nullptr, &instance_);
    return result == VK_SUCCESS;
}

bool VulkanRenderer::setup_debug_messenger() {
    if (!config_.enableValidation) return true;

    vkCreateDebugUtilsMessengerEXT_ = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    vkDestroyDebugUtilsMessengerEXT_ = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    vkCmdBeginDebugUtilsLabelEXT_ = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(instance_, "vkCmdBeginDebugUtilsLabelEXT"));
    vkCmdEndDebugUtilsLabelEXT_ = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(instance_, "vkCmdEndDebugUtilsLabelEXT"));
    vkCmdInsertDebugUtilsLabelEXT_ = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(instance_, "vkCmdInsertDebugUtilsLabelEXT"));
    vkSetDebugUtilsObjectNameEXT_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetInstanceProcAddr(instance_, "vkSetDebugUtilsObjectNameEXT"));

    if (!vkCreateDebugUtilsMessengerEXT_) return false;

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

    return vkCreateDebugUtilsMessengerEXT_(instance_, &create_info, nullptr, &debug_messenger_) == VK_SUCCESS;
}

bool VulkanRenderer::select_physical_device() {
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    if (device_count == 0) return false;

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());

    for (const auto& device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);

        VkPhysicalDeviceFeatures features;
        vkGetPhysicalDeviceFeatures(device, &features);

        // Check if device supports required features
        bool suitable = true;
        // ... feature checks

        if (suitable) {
            physical_device_ = device;
            query_gpu_info();
            query_limits();
            return true;
        }
    }

    return false;
}

bool VulkanRenderer::create_logical_device() {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queueFamilyCount, nullptr);
    if (queueFamilyCount == 0) return false;

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queueFamilyCount, queueFamilies.data());

    uint32_t graphicsFamily = VK_QUEUE_FAMILY_IGNORED;
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsFamily = i;
            break;
        }
    }
    if (graphicsFamily == VK_QUEUE_FAMILY_IGNORED) return false;

    graphics_queue_family_ = graphicsFamily;
    compute_queue_family_ = graphicsFamily;
    transfer_queue_family_ = graphicsFamily;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = graphicsFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceFeatures features{};
    std::vector<const char*> deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueCreateInfo;
    createInfo.pEnabledFeatures = &features;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    VkResult result = vkCreateDevice(physical_device_, &createInfo, nullptr, &device_);
    if (result != VK_SUCCESS) return false;

    vkGetDeviceQueue(device_, graphicsFamily, 0, &graphics_queue_);
    compute_queue_ = graphics_queue_;
    transfer_queue_ = graphics_queue_;
    return load_device_ext(device_);
}

bool VulkanRenderer::create_surface() {
    if (!native_window_) return false;
    VkAndroidSurfaceCreateInfoKHR surfaceCreateInfo{};
    surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surfaceCreateInfo.window = native_window_;
    VkResult result = fp_vkCreateAndroidSurfaceKHR(instance_, &surfaceCreateInfo, nullptr, &surface_);
    return result == VK_SUCCESS && surface_ != VK_NULL_HANDLE;
}

bool VulkanRenderer::create_swapchain() {
    if (surface_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE) return false;

    VkSurfaceCapabilitiesKHR capabilities;
    if (fp_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities) != VK_SUCCESS) {
        return false;
    }

    swapchain_extent_ = capabilities.currentExtent;
    if (swapchain_extent_.width == 0xFFFFFFFFu) {
        swapchain_extent_.width = capabilities.minImageExtent.width;
        swapchain_extent_.height = capabilities.minImageExtent.height;
        if (native_window_) {
            swapchain_extent_.width = static_cast<uint32_t>(ANativeWindow_getWidth(native_window_));
            swapchain_extent_.height = static_cast<uint32_t>(ANativeWindow_getHeight(native_window_));
        }
    }

    uint32_t formatCount = 0;
    fp_vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &formatCount, nullptr);
    if (formatCount == 0) return false;
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    fp_vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &formatCount, formats.data());

    VkSurfaceFormatKHR surfaceFormat = formats[0];
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = format;
            break;
        }
    }
    swapchain_format_ = surfaceFormat.format;
    swapchain_color_space_ = surfaceFormat.colorSpace;

    uint32_t presentModeCount = 0;
    fp_vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    fp_vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &presentModeCount, presentModes.data());
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (const auto& mode : presentModes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            presentMode = mode;
            break;
        }
    }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = swapchain_extent_;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    if (fp_vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_) != VK_SUCCESS) {
        return false;
    }

    fp_vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapchain_images_.resize(imageCount);
    fp_vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchain_images_.data());
    return true;
}

bool VulkanRenderer::create_sync_objects() {
    uint32_t max_frames = config_.maxFramesInFlight;
    image_available_semaphores_.resize(max_frames);
    render_finished_semaphores_.resize(max_frames);
    in_flight_fences_.resize(max_frames);
    images_in_flight_.resize(max_frames, VK_NULL_HANDLE);

    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < max_frames; i++) {
        if (vkCreateSemaphore(device_, &semaphore_info, nullptr, &image_available_semaphores_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &semaphore_info, nullptr, &render_finished_semaphores_[i]) != VK_SUCCESS ||
            vkCreateFence(device_, &fence_info, nullptr, &in_flight_fences_[i]) != VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

bool VulkanRenderer::onBeginFrame() {
    // Wait for fence
    vkWaitForFences(device_, 1, &in_flight_fences_[current_frame_], VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &in_flight_fences_[current_frame_]);

    // Acquire next image
    uint32_t image_index;
    VkResult result = fp_vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                            image_available_semaphores_[current_frame_],
                                            VK_NULL_HANDLE, &image_index);
    image_index_ = image_index;

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // Recreate swapchain
        return false;
    }

    // Reset command buffer
    // vkResetCommandBuffer(command_buffers_[current_frame_], 0);

    // Begin command buffer
    // VkCommandBufferBeginInfo begin_info{};
    // begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    // vkBeginCommandBuffer(command_buffers_[current_frame_], &begin_info);

    return true;
}

void VulkanRenderer::onEndFrame() {
    // End command buffer
    // vkEndCommandBuffer(command_buffers_[current_frame_]);

    // Submit command buffer
    // VkSubmitInfo submit_info{};
    // submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    // VkSemaphore wait_semaphores[] = {image_available_semaphores_[current_frame_]};
    // VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    // submit_info.waitSemaphoreCount = 1;
    // submit_info.pWaitSemaphores = wait_semaphores;
    // submit_info.pWaitDstStageMask = wait_stages;
    // submit_info.commandBufferCount = 1;
    // submit_info.pCommandBuffers = &command_buffers_[current_frame_];
    // VkSemaphore signal_semaphores[] = {render_finished_semaphores_[current_frame_]};
    // submit_info.signalSemaphoreCount = 1;
    // submit_info.pSignalSemaphores = signal_semaphores;

    // vkQueueSubmit(graphics_queue_, 1, &submit_info, in_flight_fences_[current_frame_]);
}

void VulkanRenderer::onPresent() {
    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_finished_semaphores_[current_frame_];
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain_;
    present_info.pImageIndices = &image_index_;

    fp_vkQueuePresentKHR(graphics_queue_, &present_info);

    current_frame_ = (current_frame_ + 1) % config_.maxFramesInFlight;
}

void VulkanRenderer::onResize(uint32_t width, uint32_t height) {
    // Recreate swapchain
    swapchain_extent_ = {width, height};
}

void VulkanRenderer::onWaitIdle() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
}

RendererBackend VulkanRenderer::getBackendImpl() const {
    return RendererBackend::Vulkan;
}

std::string VulkanRenderer::getGpuRendererStringImpl() const {
    if (physical_device_ != VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_device_, &props);
        return props.deviceName;
    }
    return "Unknown Vulkan Device";
}

std::string VulkanRenderer::getGpuVendorStringImpl() const {
    if (physical_device_ != VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_device_, &props);
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
    if (physical_device_ != VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_device_, &props);
        return std::to_string(VK_VERSION_MAJOR(props.apiVersion)) + "." +
               std::to_string(VK_VERSION_MINOR(props.apiVersion)) + "." +
               std::to_string(VK_VERSION_PATCH(props.apiVersion));
    }
    return "Unknown";
}

void VulkanRenderer::query_gpu_info() {
    if (physical_device_ != VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_device_, &props);
        gpu_info_.renderer_string = props.deviceName;
        gpu_info_.api_version = props.apiVersion;
        gpu_info_.driver_version = props.driverVersion;
        gpu_info_.device_id = props.deviceID;
        
        switch (props.vendorID) {
            case 0x13B5: gpu_info_.vendor = GPUVendor::Mali; break;
            case 0x5143: gpu_info_.vendor = GPUVendor::Adreno; break;
            case 0x1010: gpu_info_.vendor = GPUVendor::PowerVR; break;
            default: gpu_info_.vendor = GPUVendor::Unknown; break;
        }
    }
}

void VulkanRenderer::query_limits() {
    if (physical_device_ != VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_device_, &props);
        
        limits_.max_push_constants_size = props.limits.maxPushConstantsSize;
        limits_.max_sampler_anisotropy = static_cast<uint32_t>(props.limits.maxSamplerAnisotropy);
        limits_.max_viewport_dimensions[0] = props.limits.maxViewportDimensions[0];
        limits_.max_viewport_dimensions[1] = props.limits.maxViewportDimensions[1];
        limits_.max_framebuffer_width = props.limits.maxFramebufferWidth;
        limits_.max_framebuffer_height = props.limits.maxFramebufferHeight;
        limits_.max_framebuffer_layers = props.limits.maxFramebufferLayers;
        // maxFramebufferSamples not in standard Vulkan - use sample count from framebuffer color attachment limits
        limits_.max_framebuffer_samples = 8;
        limits_.max_color_attachments = props.limits.maxColorAttachments;
    }
}

void VulkanRenderer::apply_driver_workarounds() {
    // Apply GPU-specific workarounds based on gpu_info_.vendor and gpu_info_.architecture
    GPUCapabilities capabilities;
    capabilities.applyWorkarounds(config_);
}

void VulkanRenderer::reduceQuality() {
    // Reduce rendering quality for thermal throttling
}

bool VulkanRenderer::detectGPU() {
    return select_physical_device();
}

void VulkanRenderer::onApplyGPUWorkarounds(GPUVendor vendor, GPUArchitecture arch) {
    apply_driver_workarounds();
}

void VulkanRenderer::onOptimizeForGPU(GPUVendor vendor, GPUArchitecture arch) {
    // GPU-specific optimizations
}

bool VulkanRenderer::initializeManagers() {
    // Initialize Vulkan-specific managers
    return true;
}

void VulkanRenderer::onMemoryPressure(int level) {
    if (level <= 0) return;
    if (texture_manager_) { texture_manager_->trimCache(level); }
}

void VulkanRenderer::onThermalThrottling(float temperatureRatio) {
    if (temperatureRatio > 0.9f) {
        reduceQuality();
    }
}

bool VulkanRenderer::supportsFeature(RendererFeature feature) const {
    return (static_cast<uint32_t>(feature) & static_cast<uint32_t>(supported_features_)) != 0;
}

bool VulkanRenderer::isExtensionSupported(const std::string& extension) const {
    for (const auto& ext : gpu_info_.extensions) {
        if (ext == extension) return true;
    }
    return false;
}

void VulkanRenderer::waitIdle() {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
}

void VulkanRenderer::onSurfaceChanged(uint32_t width, uint32_t height) {
    onResize(width, height);
}

void VulkanRenderer::setNativeWindow(void* native_window) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    native_window_ = static_cast<ANativeWindow*>(native_window);
}

void VulkanRenderer::onSurfaceDestroyed() {
    // Clean up surface resources
}

} // namespace copper