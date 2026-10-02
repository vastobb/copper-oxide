#include "gles_renderer.h"
#include "renderer_base.h"
#include "gpu_capabilities.h"

#include <GLES3/gl32.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <vector>
#include <string>
#include <mutex>

namespace copper {

class GLESCRenderer::Impl {
public:
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLConfig config = nullptr;
    bool initialized = false;
    bool debug_markers_enabled = false;
    std::string vendor_string;
    std::string renderer_string;
    std::string version_string;
    std::string extensions_string;
    std::mutex mutex;
    RendererConfig config;
};

GLESCRenderer::GLESCRenderer() : RendererBase(), pImpl(std::make_unique<Impl>()) {}
GLESCRenderer::~GLESCRenderer() = default;

bool GLESCRenderer::initialize(const RendererConfig& config) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);

    if (!RendererBase::initialize(config)) {
        return false;
    }

    pImpl->config = config;
    pImpl->debug_markers_enabled = config.enableDebugMarkers;

    if (!initEGL()) return false;
    if (!createContext()) return false;
    if (!makeCurrent()) return false;
    if (!queryGPUInfo()) return false;
    if (!setupExtensions()) return false;

    pImpl->initialized = true;
    return true;
}

void GLESCRenderer::shutdown() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);

    if (pImpl->context != EGL_NO_CONTEXT) {
        eglMakeCurrent(pImpl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(pImpl->display, pImpl->context);
        pImpl->context = EGL_NO_CONTEXT;
    }

    if (pImpl->surface != EGL_NO_SURFACE) {
        eglDestroySurface(pImpl->display, pImpl->surface);
        pImpl->surface = EGL_NO_SURFACE;
    }

    if (pImpl->display != EGL_NO_DISPLAY) {
        eglTerminate(pImpl->display);
        pImpl->display = EGL_NO_DISPLAY;
    }

    RendererBase::shutdown();
    pImpl->initialized = false;
}

bool GLESCRenderer::initEGL() {
    pImpl->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (pImpl->display == EGL_NO_DISPLAY) {
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(pImpl->display, &major, &minor)) {
        return false;
    }

    // Choose config
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };

    EGLint num_configs;
    if (!eglChooseConfig(pImpl->display, config_attribs, &pImpl->config, 1, &num_configs) || num_configs == 0) {
        return false;
    }

    return true;
}

bool GLESCRenderer::createContext() {
    const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };

    pImpl->context = eglCreateContext(pImpl->display, pImpl->config, EGL_NO_CONTEXT, context_attribs);
    return pImpl->context != EGL_NO_CONTEXT;
}

bool GLESCRenderer::makeCurrent() {
    // Surface will be set later via onSurfaceCreated
    return true;
}

bool GLESCRenderer::queryGPUInfo() {
    pImpl->vendor_string = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    pImpl->renderer_string = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    pImpl->version_string = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    pImpl->extensions_string = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    return true;
}

bool GLESCRenderer::setupExtensions() {
    // Enable debug markers if supported and enabled
    if (pImpl->debug_markers_enabled) {
        // Check for KHR_debug extension
        if (pImpl->extensions_string.find("GL_KHR_debug") != std::string::npos ||
            pImpl->extensions_string.find("GL_EXT_debug_marker") != std::string::npos) {
            // Debug markers available
        }
    }
    return true;
}

bool GLESCRenderer::onBeginFrame() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (!pImpl->initialized) return false;
    return true;
}

void GLESCRenderer::onEndFrame() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (!pImpl->initialized) return;
    glFinish();
}

void GLESCRenderer::onPresent() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (!pImpl->initialized) return;
    eglSwapBuffers(pImpl->display, pImpl->surface);
}

void GLESCRenderer::onResize(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (!pImpl->initialized) return;
    glViewport(0, 0, width, height);
}

void GLESCRenderer::onWaitIdle() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (!pImpl->initialized) return;
    glFinish();
}

RendererBackend GLESCRenderer::getBackendImpl() const {
    return RendererBackend::OpenGLES;
}

std::string GLESCRenderer::getGpuRendererStringImpl() const {
    return pImpl->renderer_string;
}

std::string GLESCRenderer::getGpuVendorStringImpl() const {
    return pImpl->vendor_string;
}

std::string GLESCRenderer::getGpuVersionStringImpl() const {
    return pImpl->version_string;
}

bool GLESCRenderer::setSurface(void* native_window) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (pImpl->surface != EGL_NO_SURFACE) {
        eglDestroySurface(pImpl->display, pImpl->surface);
    }

    pImpl->surface = eglCreateWindowSurface(pImpl->display, pImpl->config, native_window, nullptr);
    if (pImpl->surface == EGL_NO_SURFACE) {
        return false;
    }

    return eglMakeCurrent(pImpl->display, pImpl->surface, pImpl->surface, pImpl->context) == EGL_TRUE;
}

void GLESCRenderer::destroySurface() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (pImpl->surface != EGL_NO_SURFACE) {
        eglDestroySurface(pImpl->display, pImpl->surface);
        pImpl->surface = EGL_NO_SURFACE;
    }
    eglMakeCurrent(pImpl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

} // namespace copper