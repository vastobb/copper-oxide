#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace copper {

class RendererBase;

class FramebufferManager {
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

    FramebufferManager();
    virtual ~FramebufferManager();

    FramebufferManager(const FramebufferManager&) = delete;
    FramebufferManager& operator=(const FramebufferManager&) = delete;
    FramebufferManager(FramebufferManager&&) noexcept = default;
    FramebufferManager& operator=(FramebufferManager&&) noexcept = default;

    bool initialize(RendererBase* renderer);
    void shutdown();

    virtual uint64_t createFramebuffer(uint32_t width, uint32_t height, const std::vector<uint64_t>& color_attachments, uint64_t depth_attachment = 0, uint64_t stencil_attachment = 0);
    virtual void destroyFramebuffer(uint64_t handle);

    virtual void createSwapchainFramebuffers(uint64_t swapchain, const std::vector<uint64_t>& images, uint64_t depth_image, uint32_t width, uint32_t height);
    virtual void destroySwapchainFramebuffers();
    virtual void onSurfaceChanged(uint32_t width, uint32_t height);

    virtual uint32_t getSurfaceWidth() const;
    virtual uint32_t getSurfaceHeight() const;

    virtual void setFramebufferDebugName(uint64_t handle, const std::string& name);

protected:
    virtual bool onCreateFramebuffer(uint64_t handle, uint32_t width, uint32_t height, const std::vector<uint64_t>& color_attachments, uint64_t depth_attachment, uint64_t stencil_attachment) = 0;
    virtual void onDestroyFramebuffer(uint64_t handle) = 0;
    virtual bool onCreateSwapchainFramebuffer(uint64_t handle, uint64_t swapchain, uint64_t image, uint64_t depth_image, uint32_t width, uint32_t height) = 0;
    virtual void onDestroySwapchainFramebuffer(uint64_t handle) = 0;
    virtual void onSetFramebufferDebugName(uint64_t handle, const std::string& name) = 0;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace copper