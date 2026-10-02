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
#include <vector>

#include "framebuffer_manager.h"

namespace copper {

class VulkanRenderer;

// INTEGRATOR NOTE -- vulkan_renderer.h currently ends with the stubs
//     class VulkanFramebufferManager : public FramebufferManager { /* ... */ };
//     class VulkanStateManager : public StateManager { /* ... */ };
// and it does NOT include this header. Those two stubs have to be deleted and
// replaced with `#include "vulkan_framebuffer_manager.h"` (+ the state manager
// header), otherwise `copper::VulkanFramebufferManager` is defined twice and
// this translation unit does not compile. The same note applies to
// vulkan_shader_manager.h / VulkanShaderManager, whose .cpp has the identical
// include order (own header first, vulkan_renderer.h second).
//
// INTEGRATOR NOTE -- public VulkanRenderer accessors this class calls. They do
// not exist yet and vulkan_renderer.h is off limits to this file, so add them to
// VulkanRenderer's public section (each is a one-line inline getter over the
// existing private member):
//
//     VkDevice     device() const;       // device_
//     VkRenderPass renderPass() const;   // render_pass_
//     VkInstance   instance() const;     // instance_  (VK_EXT_debug_utils naming)
//
// (`device()` and `renderPass()` are the same two vulkan_shader_manager.cpp
// already expects, so all three managers share one accessor set.)
//
// Vulkan backend for FramebufferManager.
//
// The base owns handle allocation, bookkeeping and the debug-name string; this
// class owns the VkFramebuffer objects. Attachment image views live in the
// backend TextureManager, which the cross-backend API only exposes as opaque
// uint64_t handles, so the views are resolved through an injected callback.
//
// DEADLOCK RULE: the base calls the on* hooks while holding ITS OWN mutex. These
// hooks never call a public method of FramebufferManager / this class, and the
// resolvers must be pure handle -> VkObject lookups. The backend mutex is never
// held across vkQueueSubmit / vkDeviceWaitIdle (this backend never submits).
class VulkanFramebufferManager : public FramebufferManager {
public:
    // TextureManager handle -> the VkImageView to attach. Return VK_NULL_HANDLE
    // for an unknown / destroyed texture.
    using ImageViewResolver = std::function<VkImageView(uint64_t texture_handle)>;

    // Optional companion to the resolver above: the metadata vkCreateFramebuffer
    // has to agree on. TextureManager has no getTexture() in the cross-backend
    // API, so the backend TextureManager is the only place that knows it.
    //
    //   width/height    - used to check that every attachment matches the
    //                     framebuffer extent.
    //   array_layers    - decides VK_IMAGE_VIEW_TYPE_2D_ARRAY vs _2D and the
    //                     VkFramebufferCreateInfo::layers value. Without this
    //                     resolver the manager assumes one layer, i.e. plain 2D.
    struct AttachmentInfo {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t array_layers = 1;
    };
    using AttachmentInfoResolver = std::function<AttachmentInfo(uint64_t texture_handle)>;

    explicit VulkanFramebufferManager(VulkanRenderer* renderer);
    ~VulkanFramebufferManager() override;

    VulkanFramebufferManager(const VulkanFramebufferManager&) = delete;
    VulkanFramebufferManager& operator=(const VulkanFramebufferManager&) = delete;

    // Optional wiring, install before the first framebuffer is created. Passing
    // an empty std::function clears the resolver and restores the documented
    // fallback (creation is refused, because a framebuffer without views cannot
    // be built).
    void setImageViewResolver(ImageViewResolver resolver);
    void setAttachmentInfoResolver(AttachmentInfoResolver resolver);

    // FramebufferManager handle -> VkFramebuffer, for VulkanStateManager's
    // framebuffer resolver (see vulkan_state_manager.h).
    VkFramebuffer vkFramebuffer(uint64_t handle) const;

protected:
    bool onCreateFramebuffer(uint64_t handle, uint32_t width, uint32_t height,
                             const std::vector<uint64_t>& color_attachments,
                             uint64_t depth_attachment, uint64_t stencil_attachment) override;
    void onDestroyFramebuffer(uint64_t handle) override;
    bool onCreateSwapchainFramebuffer(uint64_t handle, uint64_t swapchain, uint64_t image,
                                      uint64_t depth_image, uint32_t width,
                                      uint32_t height) override;
    void onDestroySwapchainFramebuffer(uint64_t handle) override;
    void onSetFramebufferDebugName(uint64_t handle, const std::string& name) override;

private:
    // VK_EXT_debug_utils object naming, resolved lazily from the instance; every
    // helper degrades to a debug log when the extension is unreachable.
    PFN_vkSetDebugUtilsObjectNameEXT debug_object_name_fn() const;
    void set_object_name(VkObjectType type, uint64_t object_handle, const std::string& name) const;

    VulkanRenderer* renderer_ = nullptr;
    ImageViewResolver image_view_resolver_;
    AttachmentInfoResolver attachment_info_resolver_;

    // Backend handle -> VkFramebuffer. Keyed by the base's handle so the base's
    // own map and this one stay in step through the on* hooks.
    std::unordered_map<uint64_t, VkFramebuffer> framebuffers_;

    mutable std::mutex mutex_;
    mutable PFN_vkSetDebugUtilsObjectNameEXT debug_object_name_fn_ = nullptr;
    mutable bool debug_object_name_fn_loaded_ = false;
    mutable bool no_image_view_resolver_logged_ = false;
};

} // namespace copper