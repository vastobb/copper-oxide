#include "framebuffer_manager.h"
#include "renderer_base.h"

#include <unordered_map>
#include <mutex>
#include <vector>

namespace copper {

class FramebufferManager::Impl {
public:
    struct Framebuffer {
        uint64_t handle = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint64_t> color_attachments;
        uint64_t depth_attachment = 0;
        uint64_t stencil_attachment = 0;
        std::string debug_name;
    };

    struct SwapchainFramebuffer {
        uint64_t handle = 0;
        uint64_t swapchain_image = 0;
        uint64_t depth_image = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    std::unordered_map<uint64_t, Framebuffer> framebuffers;
    std::vector<SwapchainFramebuffer> swapchain_framebuffers;
    uint64_t next_handle = 1;
    std::mutex mutex;
    RendererBase* renderer = nullptr;
    uint32_t surface_width = 0;
    uint32_t surface_height = 0;
};

FramebufferManager::FramebufferManager() : pImpl(std::make_unique<Impl>()) {}
FramebufferManager::~FramebufferManager() = default;

bool FramebufferManager::initialize(RendererBase* renderer) {
    pImpl->renderer = renderer;
    return true;
}

void FramebufferManager::shutdown() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    for (auto& [handle, fb] : pImpl->framebuffers) {
        onDestroyFramebuffer(handle);
    }
    for (auto& sfb : pImpl->swapchain_framebuffers) {
        onDestroySwapchainFramebuffer(sfb.handle);
    }
    pImpl->framebuffers.clear();
    pImpl->swapchain_framebuffers.clear();
}

uint64_t FramebufferManager::createFramebuffer(uint32_t width, uint32_t height, const std::vector<uint64_t>& color_attachments, uint64_t depth_attachment, uint64_t stencil_attachment) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    uint64_t handle = pImpl->next_handle++;

    Impl::Framebuffer fb;
    fb.handle = handle;
    fb.width = width;
    fb.height = height;
    fb.color_attachments = color_attachments;
    fb.depth_attachment = depth_attachment;
    fb.stencil_attachment = stencil_attachment;

    if (!onCreateFramebuffer(handle, width, height, color_attachments, depth_attachment, stencil_attachment)) {
        return 0;
    }

    pImpl->framebuffers[handle] = std::move(fb);
    return handle;
}

void FramebufferManager::destroyFramebuffer(uint64_t handle) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->framebuffers.find(handle);
    if (it == pImpl->framebuffers.end()) {
        return;
    }

    onDestroyFramebuffer(handle);
    pImpl->framebuffers.erase(it);
}

void FramebufferManager::createSwapchainFramebuffers(uint64_t swapchain, const std::vector<uint64_t>& images, uint64_t depth_image, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->surface_width = width;
    pImpl->surface_height = height;

    for (auto& sfb : pImpl->swapchain_framebuffers) {
        onDestroySwapchainFramebuffer(sfb.handle);
    }
    pImpl->swapchain_framebuffers.clear();

    for (size_t i = 0; i < images.size(); ++i) {
        Impl::SwapchainFramebuffer sfb;
        sfb.handle = pImpl->next_handle++;
        sfb.swapchain_image = images[i];
        sfb.depth_image = depth_image;
        sfb.width = width;
        sfb.height = height;

        // Never record a framebuffer the backend failed to create: callers would
        // submit a null render target and lose the device.
        if (!onCreateSwapchainFramebuffer(sfb.handle, swapchain, images[i], depth_image, width, height)) {
            return;
        }
        pImpl->swapchain_framebuffers.push_back(std::move(sfb));
    }
}

void FramebufferManager::destroySwapchainFramebuffers() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    for (auto& sfb : pImpl->swapchain_framebuffers) {
        onDestroySwapchainFramebuffer(sfb.handle);
    }
    pImpl->swapchain_framebuffers.clear();
}

void FramebufferManager::onSurfaceChanged(uint32_t width, uint32_t height) {
    // Called from the surface callback thread while the render thread reads
    // these: without the lock a torn read yields a wrong viewport.
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->surface_width = width;
    pImpl->surface_height = height;
}

uint32_t FramebufferManager::getSurfaceWidth() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->surface_width;
}

uint32_t FramebufferManager::getSurfaceHeight() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->surface_height;
}

void FramebufferManager::setFramebufferDebugName(uint64_t handle, const std::string& name) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->framebuffers.find(handle);
    if (it != pImpl->framebuffers.end()) {
        it->second.debug_name = name;
        onSetFramebufferDebugName(handle, name);
    }
}

} // namespace copper