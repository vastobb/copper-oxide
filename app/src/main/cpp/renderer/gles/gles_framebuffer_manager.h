#pragma once

// OpenGL ES backend implementation of FramebufferManager.
//
// Render passes work the way they do on Vulkan: glGenFramebuffers(),
// glFramebufferTexture2D() per attachment and glCheckFramebufferStatus().
//
// The attachment handles are opaque TextureManager ids, not GL names: they are
// resolved through GLESCTextureManager::glObject()/glTarget() so the attach call
// matches the texture's real sampling target.
//
// The swapchain is the exception. On GLES the window surface's default
// framebuffer is owned by EGL and has no image objects to wrap, so
// onCreateSwapchainFramebuffer() records the swapchain image and returns true -
// see the comment on that hook for why returning false would be worse.

#include <GLES3/gl32.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "framebuffer_manager.h"

namespace copper {

class RendererBase;
class GLESCTextureManager;

class GLESCFramebufferManager : public FramebufferManager {
public:
    explicit GLESCFramebufferManager(RendererBase* renderer);
    ~GLESCFramebufferManager() override;

    // The manager owns a mutex and the renderer pointer, so it is neither
    // copyable nor movable.
    GLESCFramebufferManager(const GLESCFramebufferManager&) = delete;
    GLESCFramebufferManager& operator=(const GLESCFramebufferManager&) = delete;
    GLESCFramebufferManager(GLESCFramebufferManager&&) = delete;
    GLESCFramebufferManager& operator=(GLESCFramebufferManager&&) = delete;

    // GL framebuffer object name for a handle. 0 means either "the default
    // framebuffer" (swapchain entries) or "unknown handle" - use
    // is_swapchain_framebuffer() to tell those apart.
    GLuint get_gl_framebuffer(uint64_t handle) const;
    bool is_swapchain_framebuffer(uint64_t handle) const;
    size_t get_live_framebuffer_count() const;

protected:
    bool onCreateFramebuffer(uint64_t handle, uint32_t width, uint32_t height,
                             const std::vector<uint64_t>& color_attachments,
                             uint64_t depth_attachment,
                             uint64_t stencil_attachment) override;
    void onDestroyFramebuffer(uint64_t handle) override;
    bool onCreateSwapchainFramebuffer(uint64_t handle, uint64_t swapchain, uint64_t image,
                                      uint64_t depth_image, uint32_t width,
                                      uint32_t height) override;
    void onDestroySwapchainFramebuffer(uint64_t handle) override;
    void onSetFramebufferDebugName(uint64_t handle, const std::string& name) override;

private:
    struct FramebufferObject {
        // 0 for swapchain entries: on GLES there is no object to own.
        GLuint gl_framebuffer = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        bool is_swapchain = false;
        std::vector<uint64_t> color_attachments;
        uint64_t depth_attachment = 0;
        uint64_t stencil_attachment = 0;
        uint64_t swapchain = 0;
        uint64_t image = 0;
        uint64_t depth_image = 0;
        std::string debug_name;
    };

    // True when this thread can issue GL calls at all.
    bool hasContext() const;
    // hasContext() plus the logged early-out every hook needs.
    bool requireContext(const char* operation) const;
    bool debug_names_enabled() const;

    // TextureManager handles are opaque ids owned by the texture manager, so the
    // GL names come from GLESCTextureManager::glObject(). The handle is never
    // reinterpret_cast into a GL name: doing so would attach a random texture.
    GLESCTextureManager* texture_manager() const;
    bool resolve_texture(uint64_t handle, GLuint& out_texture, GLenum& out_target) const;
    // Attaches one resolved texture to an attachment point, picking the attach
    // call the sampling target requires. Returns false when the texture cannot be
    // attached, so the caller can report which slot went missing.
    bool attach_texture(uint64_t handle, GLenum attachment_point) const;

    RendererBase* renderer_ = nullptr;

    // Lock order is always base FramebufferManager mutex -> this mutex. GL calls
    // run unlocked, and no hook ever calls back into a base public method.
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, FramebufferObject> framebuffers_;
    mutable GLESCTextureManager* texture_manager_ = nullptr;
    mutable bool warned_texture_resolution_ = false;
};

} // namespace copper