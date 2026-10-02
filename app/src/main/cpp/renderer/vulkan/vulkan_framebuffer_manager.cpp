#include "vulkan_framebuffer_manager.h"

// The accessors used below (device(), renderPass(), instance()) are the ones the
// integrator must add to VulkanRenderer; see the INTEGRATOR NOTE in the header.
#include "vulkan_renderer.h"

#include <android/log.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#define LOG_TAG "CopperOxide-VK"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

namespace copper {

namespace {

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
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        default: return "VkResult(unmapped)";
    }
}

} // namespace

VulkanFramebufferManager::VulkanFramebufferManager(VulkanRenderer* renderer)
    : renderer_(renderer) {}

VulkanFramebufferManager::~VulkanFramebufferManager() {
    // FramebufferManager::shutdown() destroys every framebuffer and then clears
    // its own map; this only guards against a manager that was never shut down.
    // Nothing is destroyed once the device is gone (the device itself frees the
    // child objects), it is only dropped from the map.
    std::lock_guard<std::mutex> lock(mutex_);
    if (renderer_ == nullptr || renderer_->device() == VK_NULL_HANDLE) {
        framebuffers_.clear();
        return;
    }
    for (auto& [handle, framebuffer] : framebuffers_) {
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(renderer_->device(), framebuffer, nullptr);
        }
    }
    framebuffers_.clear();
}

void VulkanFramebufferManager::setImageViewResolver(ImageViewResolver resolver) {
    std::lock_guard<std::mutex> lock(mutex_);
    image_view_resolver_ = std::move(resolver);
}

void VulkanFramebufferManager::setAttachmentInfoResolver(AttachmentInfoResolver resolver) {
    std::lock_guard<std::mutex> lock(mutex_);
    attachment_info_resolver_ = std::move(resolver);
}

VkFramebuffer VulkanFramebufferManager::vkFramebuffer(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = framebuffers_.find(handle);
    if (it == framebuffers_.end()) {
        return VK_NULL_HANDLE;
    }
    return it->second;
}

PFN_vkSetDebugUtilsObjectNameEXT VulkanFramebufferManager::debug_object_name_fn() const {
    if (debug_object_name_fn_loaded_) {
        return debug_object_name_fn_;
    }
    debug_object_name_fn_loaded_ = true;
    if (renderer_ != nullptr && renderer_->instance() != VK_NULL_HANDLE) {
        // WHY resolved here and not taken from the renderer: the renderer's own
        // debug pointers are only populated when validation is enabled, while
        // VK_EXT_debug_utils is always requested at instance creation.
        debug_object_name_fn_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
                vkGetInstanceProcAddr(renderer_->instance(), "vkSetDebugUtilsObjectNameEXT"));
    }
    if (debug_object_name_fn_ == nullptr) {
        LOGD("VK_EXT_debug_utils unavailable, framebuffer names fall back to logcat");
    }
    return debug_object_name_fn_;
}

void VulkanFramebufferManager::set_object_name(VkObjectType type, uint64_t object_handle,
                                              const std::string& name) const {
    if (object_handle == 0) {
        return;
    }
    PFN_vkSetDebugUtilsObjectNameEXT set_name = debug_object_name_fn();
    if (set_name == nullptr || renderer_ == nullptr || renderer_->instance() == VK_NULL_HANDLE) {
        LOGD("framebuffer %llu named '%s' in logcat only (VK_EXT_debug_utils unavailable)",
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

bool VulkanFramebufferManager::onCreateFramebuffer(uint64_t handle, uint32_t width, uint32_t height,
                                                   const std::vector<uint64_t>& color_attachments,
                                                   uint64_t depth_attachment,
                                                   uint64_t stencil_attachment) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (renderer_ == nullptr || renderer_->device() == VK_NULL_HANDLE) {
        LOGE("onCreateFramebuffer: Vulkan device is not ready");
        return false;
    }
    const VkRenderPass render_pass = renderer_->renderPass();
    if (render_pass == VK_NULL_HANDLE) {
        // WHY this is fatal rather than deferred: a framebuffer without a render
        // pass is not a Vulkan object, and returning true would hand the caller a
        // handle that can only ever be rejected at beginRenderPass time.
        LOGE("onCreateFramebuffer: renderer has no render pass yet");
        return false;
    }
    if (width == 0 || height == 0) {
        LOGE("onCreateFramebuffer: %ux%u framebuffer for handle %llu has a zero extent",
             width, height, static_cast<unsigned long long>(handle));
        return false;
    }
    // A framebuffer with no attachment is never usable: the render pass needs at
    // least its colour (or depth) attachment covered.
    if (color_attachments.empty() && depth_attachment == 0 && stencil_attachment == 0) {
        LOGE("onCreateFramebuffer: framebuffer %llu has no colour, depth or stencil "
             "attachment",
             static_cast<unsigned long long>(handle));
        return false;
    }
    if (!image_view_resolver_) {
        if (!no_image_view_resolver_logged_) {
            no_image_view_resolver_logged_ = true;
            LOGW("no image view resolver installed: framebuffers cannot be created");
        }
        return false;
    }

    // Attachment order matters: the array is matched against the render pass's
    // attachment descriptions position by position, so it must be
    // colour..., depth, stencil -- exactly what the base API passes in.
    std::vector<uint64_t> texture_handles = color_attachments;
    if (depth_attachment != 0) {
        texture_handles.push_back(depth_attachment);
    }
    if (stencil_attachment != 0) {
        // A depth+stencil image referenced twice would appear twice in the
        // attachment array and Vulkan would reject the framebuffer, so a shared
        // handle is only appended once (it then covers both aspects).
        if (depth_attachment == 0 || stencil_attachment != depth_attachment) {
            texture_handles.push_back(stencil_attachment);
        }
    }

    std::vector<VkImageView> attachment_views;
    attachment_views.reserve(texture_handles.size());
    uint32_t array_layers = 1;
    for (const uint64_t texture : texture_handles) {
        const VkImageView view = image_view_resolver_(texture);
        if (view == VK_NULL_HANDLE) {
            LOGE("onCreateFramebuffer: texture %llu did not resolve to an image view",
                 static_cast<unsigned long long>(texture));
            return false;
        }
        attachment_views.push_back(view);

        if (attachment_info_resolver_) {
            const AttachmentInfo info = attachment_info_resolver_(texture);
            // Extents have to match: an attachment smaller than the framebuffer
            // is a render pass that reads outside the image.
            if (info.width != 0 && info.height != 0 &&
                (info.width != width || info.height != height)) {
                LOGE("onCreateFramebuffer: attachment %llu is %ux%u but the framebuffer is "
                     "%ux%u",
                     static_cast<unsigned long long>(texture), info.width, info.height, width,
                     height);
                return false;
            }
            // The widest attachment decides the view type and the layer count;
            // a framebuffer over a 2D array must be VK_IMAGE_VIEW_TYPE_2D_ARRAY.
            array_layers = std::max(array_layers, info.array_layers);
        }
    }
    if (array_layers == 0) {
        array_layers = 1;
    }

    VkFramebufferCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    create_info.renderPass = render_pass;
    create_info.attachmentCount = static_cast<uint32_t>(attachment_views.size());
    create_info.pAttachments = attachment_views.data();
    create_info.width = width;
    create_info.height = height;
    create_info.layers = array_layers;
    // WHY there is no VkImageViewType here: VkFramebufferCreateInfo has no such
    // field -- it references image VIEWS, and the view type was fixed by the
    // TextureManager when it created them. The rule this manager enforces with
    // `layers` is the matching constraint: an attachment view must be
    // VK_IMAGE_VIEW_TYPE_2D when layers == 1 and VK_IMAGE_VIEW_TYPE_2D_ARRAY
    // when layers > 1, otherwise the framebuffer is invalid.
    if (array_layers > 1) {
        LOGD("framebuffer %llu spans %u layers: its attachment views must be "
             "VK_IMAGE_VIEW_TYPE_2D_ARRAY",
             static_cast<unsigned long long>(handle), array_layers);
    }
    // WHY no VkFramebufferCreateInfo::flags: the extension that would set
    // VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT is not part of this build.

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    const VkResult result =
            vkCreateFramebuffer(renderer_->device(), &create_info, nullptr, &framebuffer);
    if (result != VK_SUCCESS) {
        // VK_ERROR_OUT_OF_DATE_KHR here means "attachment list does not match the
        // render pass": VulkanRenderer::create_render_pass() currently declares
        // exactly one colour attachment and no depth/stencil, so a depth
        // attachment fails until the render pass grows one.
        LOGE("vkCreateFramebuffer(%llu, %u attachments, %ux%u, %u layers) failed: %s",
             static_cast<unsigned long long>(handle),
             static_cast<uint32_t>(attachment_views.size()), width, height, array_layers,
             result_name(result));
        return false;
    }

    framebuffers_[handle] = framebuffer;
    LOGD("framebuffer %llu created (%u attachment(s), %ux%u, %u layer(s))",
         static_cast<unsigned long long>(handle),
         static_cast<uint32_t>(attachment_views.size()), width, height, array_layers);
    return true;
}

void VulkanFramebufferManager::onDestroyFramebuffer(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Handle 0 is "no framebuffer" in the base's handle space, and
    // vkDestroyFramebuffer would dereference it.
    if (handle == 0) {
        LOGW("onDestroyFramebuffer: refusing to destroy handle 0");
        return;
    }
    const auto it = framebuffers_.find(handle);
    if (it == framebuffers_.end()) {
        return;
    }
    if (renderer_ == nullptr || renderer_->device() == VK_NULL_HANDLE) {
        LOGW("onDestroyFramebuffer: device already gone, leaking framebuffer %llu",
             static_cast<unsigned long long>(handle));
        framebuffers_.erase(it);
        return;
    }
    if (it->second != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(renderer_->device(), it->second, nullptr);
    }
    framebuffers_.erase(it);
}

bool VulkanFramebufferManager::onCreateSwapchainFramebuffer(uint64_t /*handle*/,
                                                           uint64_t /*swapchain*/,
                                                           uint64_t /*image*/,
                                                           uint64_t /*depth_image*/,
                                                           uint32_t /*width*/,
                                                           uint32_t /*height*/) {
    // Recorded no-op that reports SUCCESS.
    //
    // WHY: VulkanRenderer builds the swapchain VkFramebuffers itself in
    // create_swapchain() / create_swapchain_framebuffers() because it needs them
    // for its own clear path, which begins and ends the render pass without ever
    // consulting the framebuffer manager. Creating a second set here would
    // allocate duplicate objects and the renderer's swapchain_framebuffers_ would
    // still be the ones bound.
    //
    // WHY true and not false: FramebufferManager::createSwapchainFramebuffers()
    // now honours this return value and abandons the loop (leaving the previous
    // list empty) as soon as it is false, so reporting a failure here would
    // break every swapchain rebuild for no benefit.
    std::lock_guard<std::mutex> lock(mutex_);
    return true;
}

void VulkanFramebufferManager::onDestroySwapchainFramebuffer(uint64_t /*handle*/) {
    // Recorded no-op, matching onCreateSwapchainFramebuffer(): there is nothing
    // of ours to destroy. The renderer destroys its swapchain framebuffers in
    // destroy_swapchain() / create_swapchain_framebuffers().
    std::lock_guard<std::mutex> lock(mutex_);
}

void VulkanFramebufferManager::onSetFramebufferDebugName(uint64_t handle,
                                                        const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = framebuffers_.find(handle);
    if (it == framebuffers_.end()) {
        // The base only calls this for handles it knows, so this is a no-op that
        // keeps a stale handle from naming somebody else's object.
        return;
    }
    set_object_name(VK_OBJECT_TYPE_FRAMEBUFFER, reinterpret_cast<uint64_t>(it->second), name);
}

} // namespace copper