#include "vulkan_renderer.h"
#include "renderer_base.h"
#include "gpu_capabilities.h"

#include <vulkan/vulkan.h>
#include <android/log.h>
#include <android/native_window.h>
#include <algorithm>
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

#define LOG_TAG "CopperOxide-VK"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

VulkanRenderer::VulkanRenderer() : RendererBase() {}
VulkanRenderer::~VulkanRenderer() {
    shutdown();
}

bool VulkanRenderer::initialize(const RendererConfig& configIn) {
    RendererConfig config = configIn;
    config.clampToValidRanges();

    if (!create_instance()) {
        return false;
    }
    if (!load_instance_ext(instance_)) {
        LOGE("required Vulkan extensions are unavailable");
        shutdown();
        return false;
    }
    if (!setup_debug_messenger()) {
        shutdown();
        return false;
    }
    if (!select_physical_device()) {
        LOGE("no suitable Vulkan physical device");
        shutdown();
        return false;
    }
    if (!create_surface()) {
        LOGE("vkCreateAndroidSurfaceKHR failed");
        shutdown();
        return false;
    }
    if (!create_logical_device()) {
        LOGE("logical device creation failed");
        shutdown();
        return false;
    }
    if (!create_swapchain()) {
        LOGE("swapchain creation failed");
        shutdown();
        return false;
    }
    if (!create_sync_objects()) {
        shutdown();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        config_ = config;
    }

    // RendererBase::initialize() runs GPU detection and flips pImpl->initialized,
    // which is what gates beginFrame().
    if (!RendererBase::initialize(config)) {
        shutdown();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        initialized_ = true;
    }
    LOGI("Vulkan renderer initialized on %s", getGpuRendererStringImpl().c_str());
    return true;
}

void VulkanRenderer::shutdown() {
    if (!initialized_ && instance_ == VK_NULL_HANDLE) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        initialized_ = false;
    }

    // Must happen while the device is still alive and outside frame_mutex_:
    // RendererBase::shutdown() calls the virtual waitIdle().
    RendererBase::shutdown();

    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        destroy_swapchain();

        for (auto fence : in_flight_fences_) {
            vkDestroyFence(device_, fence, nullptr);
        }
        in_flight_fences_.clear();
        for (auto semaphore : render_finished_semaphores_) {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
        render_finished_semaphores_.clear();
        for (auto semaphore : image_available_semaphores_) {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
        image_available_semaphores_.clear();
        images_in_flight_.clear();

        for (auto command_buffer : command_buffers_) {
            vkFreeCommandBuffers(device_, command_pool_, 1, &command_buffer);
        }
        command_buffers_.clear();
        if (command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, command_pool_, nullptr);
            command_pool_ = VK_NULL_HANDLE;
        }
        if (render_pass_ != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device_, render_pass_, nullptr);
            render_pass_ = VK_NULL_HANDLE;
        }
        if (descriptor_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
            descriptor_pool_ = VK_NULL_HANDLE;
        }
        if (pipeline_cache_ != VK_NULL_HANDLE) {
            vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
            pipeline_cache_ = VK_NULL_HANDLE;
        }
        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }

        if (debug_messenger_ != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT_) {
            vkDestroyDebugUtilsMessengerEXT_(instance_, debug_messenger_, nullptr);
            debug_messenger_ = VK_NULL_HANDLE;
        }
        if (surface_ != VK_NULL_HANDLE && fp_vkDestroySurfaceKHR) {
            fp_vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
    }

    if (native_window_) {
        ANativeWindow_release(native_window_);
        native_window_ = nullptr;
    }
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
    if (swapchain_extent_.width == 0xFFFFFFFFu || swapchain_extent_.width == 0) {
        uint32_t width = capabilities.minImageExtent.width;
        uint32_t height = capabilities.minImageExtent.height;
        if (native_window_) {
            width = static_cast<uint32_t>(ANativeWindow_getWidth(native_window_));
            height = static_cast<uint32_t>(ANativeWindow_getHeight(native_window_));
        }
        // Clamping matters: drivers reject extents outside min/maxImageExtent
        // during a resize.
        swapchain_extent_.width = std::clamp(width, capabilities.minImageExtent.width,
                                            capabilities.maxImageExtent.width);
        swapchain_extent_.height = std::clamp(height, capabilities.minImageExtent.height,
                                              capabilities.maxImageExtent.height);
    }
    if (swapchain_extent_.width == 0 || swapchain_extent_.height == 0) {
        return false;
    }

    uint32_t formatCount = 0;
    fp_vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &formatCount, nullptr);
    if (formatCount == 0) return false;
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    fp_vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &formatCount, formats.data());

    VkSurfaceFormatKHR surfaceFormat = formats[0];
    for (const auto& format : formats) {
        // VK_FORMAT_UNDEFINED must never be selected as a concrete format.
        if (format.format == VK_FORMAT_UNDEFINED) {
            continue;
        }
        surfaceFormat = format;
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            break;
        }
    }
    if (surfaceFormat.format == VK_FORMAT_UNDEFINED) {
        return false;
    }
    swapchain_format_ = surfaceFormat.format;
    swapchain_color_space_ = surfaceFormat.colorSpace;

    uint32_t presentModeCount = 0;
    fp_vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    fp_vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &presentModeCount, presentModes.data());
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (config_.vsyncEnabled) {
        for (const auto& mode : presentModes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                presentMode = mode;
                break;
            }
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
    if (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
        createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;

    const VkSwapchainKHR oldSwapchain = swapchain_;
    createInfo.oldSwapchain = oldSwapchain;

    if (fp_vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_) != VK_SUCCESS) {
        return false;
    }

    fp_vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapchain_images_.resize(imageCount);
    fp_vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchain_images_.data());

    if (!create_render_pass()) {
        return false;
    }

    for (VkImageView view : swapchain_image_views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    swapchain_image_views_.resize(swapchain_images_.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < swapchain_images_.size(); i++) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchain_images_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchain_format_;
        viewInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &viewInfo, nullptr, &swapchain_image_views_[i]) != VK_SUCCESS) {
            return false;
        }
    }

    if (!create_swapchain_framebuffers()) {
        return false;
    }

    if (oldSwapchain != VK_NULL_HANDLE && fp_vkDestroySwapchainKHR) {
        fp_vkDestroySwapchainKHR(device_, oldSwapchain, nullptr);
    }
    return true;
}

bool VulkanRenderer::create_swapchain_framebuffers() {
    for (VkFramebuffer framebuffer : swapchain_framebuffers_) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    swapchain_framebuffers_.resize(swapchain_image_views_.size(), VK_NULL_HANDLE);

    for (size_t i = 0; i < swapchain_image_views_.size(); i++) {
        VkImageView attachment = swapchain_image_views_[i];
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = render_pass_;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &attachment;
        framebufferInfo.width = swapchain_extent_.width;
        framebufferInfo.height = swapchain_extent_.height;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &swapchain_framebuffers_[i]) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::create_render_pass() {
    if (render_pass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }

    VkAttachmentDescription color_attachment{};
    color_attachment.format = swapchain_format_;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = 0;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = 0;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &color_attachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 2;
    renderPassInfo.pDependencies = dependencies;

    return vkCreateRenderPass(device_, &renderPassInfo, nullptr, &render_pass_) == VK_SUCCESS;
}

bool VulkanRenderer::destroy_swapchain() {
    if (device_ == VK_NULL_HANDLE) {
        swapchain_image_views_.clear();
        swapchain_images_.clear();
        swapchain_framebuffers_.clear();
        swapchain_ = VK_NULL_HANDLE;
        return true;
    }
    vkDeviceWaitIdle(device_);
    for (VkFramebuffer framebuffer : swapchain_framebuffers_) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    swapchain_framebuffers_.clear();
    for (VkImageView view : swapchain_image_views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    swapchain_image_views_.clear();
    if (swapchain_ != VK_NULL_HANDLE && fp_vkDestroySwapchainKHR) {
        fp_vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    }
    swapchain_ = VK_NULL_HANDLE;
    swapchain_images_.clear();
    return true;
}

bool VulkanRenderer::recreate_swapchain() {
    if (surface_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE) {
        return false;
    }
    // Every in-flight frame references the old swapchain; wait for them before
    // destroying it.
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    return create_swapchain();
}

bool VulkanRenderer::create_command_pool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphics_queue_family_;
    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &command_pool_) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = command_pool_;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = frames_in_flight_;
    command_buffers_.resize(frames_in_flight_);
    return vkAllocateCommandBuffers(device_, &allocateInfo, command_buffers_.data()) == VK_SUCCESS;
}

bool VulkanRenderer::create_sync_objects() {
    // Frames in flight is bounded: each one owns a command buffer, two
    // semaphores and a fence, and mobile drivers dislike deep swapchain queues.
    // The clamp has to happen before create_command_pool(), which sizes the
    // allocation from frames_in_flight_, and the config is updated to match so
    // RendererBase allocates exactly this many per-frame command buffer slots.
    frames_in_flight_ = std::clamp(config_.maxFramesInFlight, 1u, 3u);
    config_.maxFramesInFlight = frames_in_flight_;
    const uint32_t max_frames = frames_in_flight_;
    if (!create_command_pool()) {
        return false;
    }
    image_available_semaphores_.resize(max_frames);
    render_finished_semaphores_.resize(max_frames);
    in_flight_fences_.resize(max_frames);
    images_in_flight_.assign(swapchain_images_.size(), VK_NULL_HANDLE);

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

void VulkanRenderer::wait_for_fence(VkFence fence, uint64_t timeout_ns) {
    if (fence == VK_NULL_HANDLE) {
        return;
    }
    const uint64_t timeout =
        (timeout_ns == UINT64_MAX) ? UINT64_MAX : timeout_ns;
    const VkResult result = vkWaitForFences(device_, 1, &fence, VK_TRUE, timeout);
    if (result != VK_SUCCESS && result != VK_TIMEOUT) {
        LOGW("vkWaitForFences returned %d", static_cast<int>(result));
    }
}

bool VulkanRenderer::onBeginFrame() {
    if (swapchain_ == VK_NULL_HANDLE || frames_in_flight_ == 0) {
        return false;
    }
    if (current_frame_ >= in_flight_fences_.size()) {
        current_frame_ = 0;
    }

    // Wait for the frame slot we are about to reuse. The fence is only reset
    // after a successful submit: resetting it before the acquire would leave a
    // fence that nothing ever signals if the acquire failed.
    const uint64_t timeout_ns = static_cast<uint64_t>(config_.frameTimeoutMs) * 1000000ULL;
    if (vkWaitForFences(device_, 1, &in_flight_fences_[current_frame_], VK_TRUE, timeout_ns) != VK_SUCCESS) {
        return false;
    }

    uint32_t image_index = 0;
    const VkResult result = fp_vkAcquireNextImageKHR(device_, swapchain_, timeout_ns,
                                                    image_available_semaphores_[current_frame_],
                                                    VK_NULL_HANDLE, &image_index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain();
        return false;
    }
    if (result != VK_SUCCESS) {
        LOGW("vkAcquireNextImageKHR returned %d", static_cast<int>(result));
        return false;
    }

    image_index_ = image_index;
    if (image_index_ < images_in_flight_.size()) {
        images_in_flight_[image_index_] = in_flight_fences_[current_frame_];
    }
    return true;
}

void VulkanRenderer::onEndFrame() {
    if (current_frame_ >= command_buffers_.size() || image_index_ >= swapchain_framebuffers_.size()) {
        return;
    }
    VkCommandBuffer command_buffer = command_buffers_[current_frame_];

    vkResetCommandBuffer(command_buffer, 0);
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command_buffer, &begin_info);

    VkClearAttachment clear_attachment{};
    clear_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear_attachment.clearValue.color = {{0.05f, 0.05f, 0.07f, 1.0f}};

    VkClearRect clear_rect{};
    clear_rect.rect.offset = {0, 0};
    clear_rect.rect.extent = swapchain_extent_;
    clear_rect.baseArrayLayer = 0;
    clear_rect.layerCount = 1;

    VkRenderPassBeginInfo pass_begin{};
    pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass_begin.renderPass = render_pass_;
    pass_begin.framebuffer = swapchain_framebuffers_[image_index_];
    pass_begin.renderArea.offset = {0, 0};
    pass_begin.renderArea.extent = swapchain_extent_;
    pass_begin.clearValueCount = 0;

    vkCmdBeginRenderPass(command_buffer, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdClearAttachments(command_buffer, 1, &clear_attachment, 1, &clear_rect);
    vkCmdEndRenderPass(command_buffer);

    // Replay whatever the caller recorded through RendererBase::getCommandBuffer()
    // into this frame's command buffer. Without this the frame would only ever
    // contain the clear, and a submit would report success for work that never
    // reached the GPU.
    if (command_sink_ != nullptr) {
        command_sink_->setSwapchainFramebuffers(swapchain_framebuffers_);
        command_sink_->setCommandBuffer(command_buffer, current_frame_);
        if (CommandBuffer* recorded = acquireCommandBuffer(current_frame_)) {
            if (!recorded->execute()) {
                LOGW("recorded frame commands were dropped (no sink)");
            }
            recorded->reset();
        }
    }

    vkEndCommandBuffer(command_buffer);

    // Only now is it safe to reset the fence: a submit follows immediately.
    vkResetFences(device_, 1, &in_flight_fences_[current_frame_]);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &image_available_semaphores_[current_frame_];
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &render_finished_semaphores_[current_frame_];

    const VkResult submit_result =
        vkQueueSubmit(graphics_queue_, 1, &submit_info, in_flight_fences_[current_frame_]);
    if (submit_result != VK_SUCCESS) {
        LOGW("vkQueueSubmit returned %d", static_cast<int>(submit_result));
    }
}

void VulkanRenderer::onPresent() {
    if (swapchain_ == VK_NULL_HANDLE || current_frame_ >= render_finished_semaphores_.size()) {
        return;
    }
    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_finished_semaphores_[current_frame_];
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain_;
    present_info.pImageIndices = &image_index_;

    const VkResult result = fp_vkQueuePresentKHR(graphics_queue_, &present_info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain();
    } else if (result != VK_SUCCESS) {
        LOGW("vkQueuePresentKHR returned %d", static_cast<int>(result));
    }

    current_frame_ = (current_frame_ + 1) % frames_in_flight_;
}

void VulkanRenderer::onResize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (swapchain_extent_.width == width && swapchain_extent_.height == height) {
        return;
    }
    swapchain_extent_ = {width, height};
    if (device_ != VK_NULL_HANDLE) {
        recreate_swapchain();
    }
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

std::unique_ptr<Profiler> VulkanRenderer::createProfiler() {
    auto profiler = std::make_unique<VulkanProfiler>(this);
    profiler->probe_support();
    return profiler;
}

std::unique_ptr<SyncManager> VulkanRenderer::createSyncManager() {
    return std::make_unique<VulkanSyncManager>(this);
}

std::unique_ptr<ResourcePool> VulkanRenderer::createResourcePool() {
    return std::make_unique<VulkanResourcePool>(this);
}

std::unique_ptr<CommandBuffer> VulkanRenderer::createCommandBuffer(uint32_t frame_index) {
    auto command_buffer = std::make_unique<VulkanCommandBuffer>(this);

    // Use the VkCommandBuffer the renderer already allocated for this frame slot
    // rather than allocating a second one: only command_buffers_ is tracked by
    // the fence ring, so a second buffer would never be waited on.
    if (frame_index < command_buffers_.size()) {
        command_buffer->set_command_buffer(command_buffers_[frame_index], frame_index);
    }

    // The renderer submits and presents the frame itself, so this object only
    // closes the recording.
    command_buffer->set_renderer_submit(true);

    if (command_sink_ != nullptr) {
        command_buffer->set_sync_manager(dynamic_cast<VulkanSyncManager*>(getSyncManager()));
        command_buffer->set_command_sink(command_sink_.get());
    }
    return command_buffer;
}

bool VulkanRenderer::initializeBackendManagers() {
    auto buffer_manager = std::make_unique<VulkanBufferManager>(this);
    auto texture_manager = std::make_unique<VulkanTextureManager>(this);
    auto shader_manager = std::make_unique<VulkanShaderManager>(this);
    auto framebuffer_manager = std::make_unique<VulkanFramebufferManager>(this);
    auto state_manager = std::make_unique<VulkanStateManager>(this);

    VulkanBufferManager* buffers = buffer_manager.get();
    VulkanTextureManager* textures = texture_manager.get();
    VulkanShaderManager* shaders = shader_manager.get();
    VulkanFramebufferManager* framebuffers = framebuffer_manager.get();
    VulkanStateManager* state = state_manager.get();

    // Handles are opaque manager-local ids, never VkBuffer/VkImageView values,
    // so every backend manager needs a way to resolve one back to the object the
    // API expects. A missing resolver degrades to "unresolvable", which the
    // hooks log rather than silently binding a null handle.
    state->setBufferResolver([buffers](uint64_t handle) { return buffers->vkBuffer(handle); });
    state->setPipelineResolver([shaders](uint64_t handle) {
        VulkanStateManager::VulkanPipelineBinding binding{};
        binding.pipeline = shaders->pipeline(handle);
        binding.layout = shaders->pipelineLayout(handle);
        return binding;
    });
    state->setFramebufferResolver(
        [framebuffers](uint64_t handle) { return framebuffers->vkFramebuffer(handle); });
    framebuffers->setImageViewResolver(
        [textures](uint64_t handle) { return textures->imageView(handle); });
    framebuffers->setAttachmentInfoResolver([textures](uint64_t handle) {
        VulkanFramebufferManager::AttachmentInfo info{};
        textures->imageExtent(handle, &info.width, &info.height, &info.array_layers);
        return info;
    });

    if (!registerBufferManager(std::move(buffer_manager)) ||
        !registerTextureManager(std::move(texture_manager)) ||
        !registerShaderManager(std::move(shader_manager)) ||
        !registerFramebufferManager(std::move(framebuffer_manager))) {
        return false;
    }

    // The sink and the state manager must exist before the frame command
    // buffers are built, because createCommandBuffer() hands them the sink.
    command_sink_ = std::make_unique<VulkanCommandSink>(this, state);
    command_sink_->setProfiler(getProfiler());

    if (!registerStateManager(std::move(state_manager))) {
        return false;
    }

    // Rebuild the per-frame command buffers so each one gets the sink that the
    // base created them before it knew about.
    return refreshCommandBufferSinks();
}

bool VulkanRenderer::refreshCommandBufferSinks() {
    if (command_sink_ == nullptr) {
        return false;
    }
    command_sink_->setSwapchainFramebuffers(swapchain_framebuffers_);
    for (uint32_t frame = 0; frame < config_.maxFramesInFlight; ++frame) {
        if (CommandBuffer* command_buffer = acquireCommandBuffer(frame)) {
            auto* vulkan_command_buffer = dynamic_cast<VulkanCommandBuffer*>(command_buffer);
            if (vulkan_command_buffer != nullptr) {
                vulkan_command_buffer->set_command_sink(command_sink_.get());
            }
        }
    }
    return true;
}

} // namespace copper