#include "gles_framebuffer_manager.h"

#include "gles_texture_manager.h"
#include "renderer_base.h"
#include "renderer_config.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <android/log.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define LOG_TAG "CopperOxide-Framebuffer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

namespace {

const char* framebuffer_status_name(GLenum status) {
    switch (status) {
        case GL_FRAMEBUFFER_COMPLETE:
            return "GL_FRAMEBUFFER_COMPLETE";
        case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
            return "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
        case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
            return "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
        case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS:
            return "GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS";
        case GL_FRAMEBUFFER_UNSUPPORTED:
            return "GL_FRAMEBUFFER_UNSUPPORTED";
        case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:
            return "GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE";
        default:
            return "unknown status";
    }
}

} // namespace

GLESCFramebufferManager::GLESCFramebufferManager(RendererBase* renderer) : renderer_(renderer) {
    // initialize() only caches the renderer pointer, so doing it here makes the
    // manager usable immediately; initializeManagers() calling it again is
    // harmless.
    FramebufferManager::initialize(renderer);
}

GLESCFramebufferManager::~GLESCFramebufferManager() {
    // RendererBase::shutdown() destroys the managers through unique_ptr::reset()
    // without calling manager->shutdown(), so the GL framebuffers are released
    // here. Without a current context the hooks log and skip; GLESCRenderer has
    // already torn the context down by then and the objects die with it, so
    // nothing actually leaks.
    FramebufferManager::shutdown();
}

GLuint GLESCFramebufferManager::get_gl_framebuffer(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = framebuffers_.find(handle);
    return it == framebuffers_.end() ? 0u : it->second.gl_framebuffer;
}

bool GLESCFramebufferManager::is_swapchain_framebuffer(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = framebuffers_.find(handle);
    return it != framebuffers_.end() && it->second.is_swapchain;
}

size_t GLESCFramebufferManager::get_live_framebuffer_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return framebuffers_.size();
}

bool GLESCFramebufferManager::onCreateFramebuffer(uint64_t handle, uint32_t width, uint32_t height,
                                                  const std::vector<uint64_t>& color_attachments,
                                                  uint64_t depth_attachment,
                                                  uint64_t stencil_attachment) {
    // A framebuffer with no attachment can never be complete, and creating one
    // would hand the caller a handle that fails at draw time with no diagnostic.
    if (color_attachments.empty() && depth_attachment == 0 && stencil_attachment == 0) {
        LOGE("framebuffer %llu: no color, depth or stencil attachment",
             static_cast<unsigned long long>(handle));
        return false;
    }
    if (width == 0 || height == 0) {
        LOGE("framebuffer %llu: %ux%u is empty, the result would always be "
             "incomplete",
             static_cast<unsigned long long>(handle), static_cast<unsigned>(width),
             static_cast<unsigned>(height));
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (framebuffers_.find(handle) != framebuffers_.end()) {
            LOGE("framebuffer %llu: handle already in use",
                 static_cast<unsigned long long>(handle));
            return false;
        }
    }
    if (!requireContext("onCreateFramebuffer")) {
        return false;
    }

    GLint max_color_attachments = 0;
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &max_color_attachments);
    if (max_color_attachments > 0 &&
        static_cast<GLsizei>(color_attachments.size()) > max_color_attachments) {
        LOGE("framebuffer %llu: %zu color attachments exceed the device limit of %d",
             static_cast<unsigned long long>(handle), color_attachments.size(),
             static_cast<int>(max_color_attachments));
        return false;
    }

    // GL work runs unlocked so no lock is held across the driver calls. The base
    // FramebufferManager serialises create/destroy with its own mutex, so no
    // concurrent destroyFramebuffer() can race this creation.
    GLint previous_binding = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_binding);

    GLuint gl_framebuffer = 0;
    glGenFramebuffers(1, &gl_framebuffer);
    if (gl_framebuffer == 0) {
        LOGE("framebuffer %llu: glGenFramebuffers failed (GL error 0x%x)",
             static_cast<unsigned long long>(handle), glGetError());
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_binding));
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, gl_framebuffer);

    for (size_t i = 0; i < color_attachments.size(); ++i) {
        if (color_attachments[i] == 0) {
            continue;
        }
        // A failure here does not abort the creation: glCheckFramebufferStatus()
        // below is the authority on whether the framebuffer ended up usable, and
        // its verdict is what the caller needs to see.
        attach_texture(color_attachments[i],
                       static_cast<GLenum>(GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i)));
    }

    // One texture used for depth and stencil must be attached to
    // GL_DEPTH_STENCIL_ATTACHMENT; attaching the same texture to both separate
    // points is what produces INCOMPLETE_ATTACHMENT on combined formats.
    if (depth_attachment != 0 && depth_attachment == stencil_attachment) {
        attach_texture(depth_attachment, GL_DEPTH_STENCIL_ATTACHMENT);
    } else {
        if (depth_attachment != 0) {
            attach_texture(depth_attachment, GL_DEPTH_ATTACHMENT);
        }
        if (stencil_attachment != 0) {
            attach_texture(stencil_attachment, GL_STENCIL_ATTACHMENT);
        }
    }

    // The attachments have no format information at this level, so a
    // colour-only texture in a depth slot is only reported by the completeness
    // check below. That is the honest place for it: the driver knows the formats.
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("framebuffer %llu: %s (0x%04x), %ux%u, %zu color attachment(s)",
             static_cast<unsigned long long>(handle), framebuffer_status_name(status),
             static_cast<unsigned>(status), static_cast<unsigned>(width),
             static_cast<unsigned>(height), color_attachments.size());
        glDeleteFramebuffers(1, &gl_framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_binding));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        FramebufferObject object;
        object.gl_framebuffer = gl_framebuffer;
        object.width = width;
        object.height = height;
        object.color_attachments = color_attachments;
        object.depth_attachment = depth_attachment;
        object.stencil_attachment = stencil_attachment;
        framebuffers_[handle] = std::move(object);
    }

    // Leave the caller's binding exactly as it was: this manager is not the one
    // that owns the render pass, so quietly switching the target out from under
    // it would be a nasty surprise.
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_binding));
    return true;
}

void GLESCFramebufferManager::onDestroyFramebuffer(uint64_t handle) {
    GLuint gl_framebuffer = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = framebuffers_.find(handle);
        if (it == framebuffers_.end()) {
            return;
        }
        gl_framebuffer = it->second.gl_framebuffer;
        framebuffers_.erase(it);
    }

    if (gl_framebuffer == 0) {
        // Swapchain entry: nothing was created on the GL side. Returning quietly
        // is what makes shutdown() and an explicit destroyFramebuffer() both
        // safe.
        return;
    }
    if (!requireContext("onDestroyFramebuffer")) {
        return;
    }

    // Deleting the bound framebuffer is legal in GL; the binding reverts to 0,
    // so no unbind dance is needed here.
    glDeleteFramebuffers(1, &gl_framebuffer);
}

bool GLESCFramebufferManager::onCreateSwapchainFramebuffer(uint64_t handle, uint64_t swapchain,
                                                           uint64_t image, uint64_t depth_image,
                                                           uint32_t width, uint32_t height) {
    // There is deliberately nothing to create on the GL side:
    //   - The swapchain "images" of a GLES renderer are EGL window surface
    //     contents. The render target is the default framebuffer (name 0), which
    //     EGL owns and re-binds across eglSwapBuffers(); wrapping it in a
    //     glGenFramebuffers() object would break that contract.
    //   - depth_image has nowhere to be attached: the default framebuffer's depth
    //     and stencil come from the EGL config (depth_size/stencil_size chosen in
    //     init_egl()). Rendering to an offscreen FBO and blitting is the way to
    //     get custom depth on GLES, and that is a caller-level decision.
    //
    // Returning true is required: the base now consumes this result and aborts
    // the whole loop on false, which would leave the remaining swapchain images
    // unrecorded and drop the renderer's render targets. A recorded no-op is the
    // truthful answer - "this backend owns no object here".
    std::lock_guard<std::mutex> lock(mutex_);
    FramebufferObject object;
    object.gl_framebuffer = 0;
    object.is_swapchain = true;
    object.width = width;
    object.height = height;
    object.swapchain = swapchain;
    object.image = image;
    object.depth_image = depth_image;
    framebuffers_[handle] = std::move(object);
    return true;
}

void GLESCFramebufferManager::onDestroySwapchainFramebuffer(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = framebuffers_.find(handle);
    if (it == framebuffers_.end()) {
        return;
    }
    if (!it->second.is_swapchain) {
        // Guard: an offscreen framebuffer reached the swapchain teardown path.
        // Leaving it alone keeps onDestroyFramebuffer() authoritative.
        LOGW("framebuffer %llu is not a swapchain entry, left untouched",
             static_cast<unsigned long long>(handle));
        return;
    }
    // Nothing to delete: the surface belongs to EGL and survives the swapchain.
    framebuffers_.erase(it);
}

void GLESCFramebufferManager::onSetFramebufferDebugName(uint64_t handle, const std::string& name) {
    bool known = false;
    GLuint gl_framebuffer = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = framebuffers_.find(handle);
        if (it != framebuffers_.end()) {
            it->second.debug_name = name;
            gl_framebuffer = it->second.gl_framebuffer;
            known = true;
        }
    }

    if (!known) {
        return;
    }

    // Stored and logged, but not pushed to the driver: object naming is
    // GL_KHR_debug (ES 3.2), and doing it only where the extension happens to
    // exist would make debug tooling device dependent. The name stays available
    // for logs and for a future KHR_debug path.
    if (debug_names_enabled()) {
        LOGI("framebuffer %llu (GL name %u) named '%s'", static_cast<unsigned long long>(handle),
             static_cast<unsigned>(gl_framebuffer), name.c_str());
    }
}

bool GLESCFramebufferManager::hasContext() const {
    if (renderer_ == nullptr) {
        return false;
    }
    if (!renderer_->isInitialized()) {
        return false;
    }
    // Per-thread EGL state: only the thread the context is bound to may issue GL
    // calls, and the render thread changes between frames.
    return eglGetCurrentContext() != EGL_NO_CONTEXT;
}

bool GLESCFramebufferManager::requireContext(const char* operation) const {
    if (hasContext()) {
        return true;
    }
    LOGW("%s skipped: no GL context is current on this thread (renderer %s)", operation,
         renderer_ == nullptr ? "missing" : "up");
    return false;
}

bool GLESCFramebufferManager::attach_texture(uint64_t handle, GLenum attachment_point) const {
    GLuint gl_texture = 0;
    GLenum target = 0;
    if (!resolve_texture(handle, gl_texture, target)) {
        return false;
    }

    // The attach call has to match the texture's sampling target, and the base API
    // carries no per-attachment layer or mip, so level 0 (layer 0 for arrays) is
    // what gets attached. Anything richer needs a view on the Vulkan side and has
    // no analogue here.
    switch (target) {
        case GL_TEXTURE_2D:
        case GL_TEXTURE_2D_MULTISAMPLE:
            glFramebufferTexture2D(GL_FRAMEBUFFER, attachment_point, target, gl_texture, 0);
            return true;
        case GL_TEXTURE_2D_ARRAY:
            glFramebufferTextureLayer(GL_FRAMEBUFFER, attachment_point, gl_texture, 0, 0);
            return true;
        case GL_TEXTURE_3D:
            glFramebufferTexture2D(GL_FRAMEBUFFER, attachment_point, GL_TEXTURE_3D, gl_texture, 0);
            return true;
        default:
            // Cube maps are sampled through the GL_TEXTURE_CUBE_MAP_* targets and
            // are never valid framebuffer attachments, so this is reported instead
            // of producing a silent GL_INVALID_OPERATION.
            LOGW("texture %llu has sampling target 0x%04x, which cannot be a "
                 "framebuffer attachment",
                 static_cast<unsigned long long>(handle), static_cast<unsigned>(target));
            return false;
    }
}

GLESCTextureManager* GLESCFramebufferManager::texture_manager() const {
    if (texture_manager_ != nullptr) {
        return texture_manager_;
    }
    if (renderer_ == nullptr) {
        return nullptr;
    }
    // dynamic_cast rather than static_cast: RendererBase::getTextureManager()
    // returns whatever backend was installed, and a foreign one must degrade to
    // "unresolvable handle" instead of reinterpreting an id as a GL name. Racing
    // threads compute the same pointer and the renderer owns the manager for its
    // whole lifetime.
    texture_manager_ = dynamic_cast<GLESCTextureManager*>(renderer_->getTextureManager());
    return texture_manager_;
}

bool GLESCFramebufferManager::resolve_texture(uint64_t handle, GLuint& out_texture,
                                              GLenum& out_target) const {
    out_texture = 0;
    out_target = 0;

    GLESCTextureManager* textures = texture_manager();
    const GLuint gl_texture = textures != nullptr ? textures->glObject(handle) : 0;
    if (gl_texture == 0) {
        bool first_failure = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            first_failure = !warned_texture_resolution_;
            warned_texture_resolution_ = true;
        }
        if (first_failure) {
            if (textures != nullptr) {
                LOGW("texture %llu is unknown to the GLES texture manager; its "
                     "attachment slot stays empty",
                     static_cast<unsigned long long>(handle));
            } else {
                LOGW("cannot resolve texture %llu: the renderer has no GLES texture "
                     "manager",
                     static_cast<unsigned long long>(handle));
            }
        }
        return false;
    }

    out_texture = gl_texture;
    out_target = textures->glTarget(handle);
    return true;
}

bool GLESCFramebufferManager::debug_names_enabled() const {
    // RendererConfig::enableDebugMarkers rather than NDEBUG: the Android build
    // always defines NDEBUG, so a compile-time check would silence this forever.
    return renderer_ != nullptr && renderer_->getConfig().enableDebugMarkers;
}

} // namespace copper