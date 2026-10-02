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

GLESCRenderer::GLESCRenderer() : RendererBase() {}
GLESCRenderer::~GLESCRenderer() = default;

bool GLESCRenderer::initialize(const RendererConfig& config) {
    std::lock_guard<std::mutex> lock(frame_mutex_);

    if (initialized_) {
        return true;
    }

    config_ = config;

    if (!init_egl()) return false;
    if (!create_egl_context()) return false;
    if (egl_surface_ == EGL_NO_SURFACE) {
        if (native_window_) {
            create_window_surface();
        } else {
            create_info_surface();
        }
    }
    if (!query_gpu_info()) return false;
    if (!apply_driver_workarounds()) return false;

    initialized_ = true;
    return true;
}

void GLESCRenderer::setNativeWindow(void* native_window) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    native_window_ = static_cast<ANativeWindow*>(native_window);
    if (initialized_ && native_window_ && egl_display_ != EGL_NO_DISPLAY) {
        if (egl_surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(egl_display_, egl_surface_);
            egl_surface_ = EGL_NO_SURFACE;
        }
        create_window_surface();
    }
}

void GLESCRenderer::shutdown() {
    std::lock_guard<std::mutex> lock(frame_mutex_);

    if (egl_context_ != EGL_NO_CONTEXT) {
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(egl_display_, egl_context_);
        egl_context_ = EGL_NO_CONTEXT;
    }

    if (egl_surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(egl_display_, egl_surface_);
        egl_surface_ = EGL_NO_SURFACE;
    }

    if (egl_display_ != EGL_NO_DISPLAY) {
        eglTerminate(egl_display_);
        egl_display_ = EGL_NO_DISPLAY;
    }

    RendererBase::shutdown();
    initialized_ = false;
}

bool GLESCRenderer::init_egl() {
    egl_display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_display_ == EGL_NO_DISPLAY) {
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(egl_display_, &major, &minor)) {
        return false;
    }
    egl_major_version_ = major;
    egl_minor_version_ = minor;

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
    if (!eglChooseConfig(egl_display_, config_attribs, &egl_config_, 1, &num_configs) || num_configs == 0) {
        return false;
    }

    return true;
}

bool GLESCRenderer::create_egl_context() {
    const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };

    egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, context_attribs);
    return egl_context_ != EGL_NO_CONTEXT;
}

bool GLESCRenderer::create_window_surface() {
    if (!native_window_) return false;
    egl_surface_ = eglCreateWindowSurface(egl_display_, egl_config_, native_window_, nullptr);
    return egl_surface_ != EGL_NO_SURFACE;
}

bool GLESCRenderer::create_info_surface() {
    const EGLint surface_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    egl_surface_ = eglCreatePbufferSurface(egl_display_, egl_config_, surface_attribs);
    return egl_surface_ != EGL_NO_SURFACE;
}

bool GLESCRenderer::query_gpu_info() {
    if (!make_current()) return false;

    const GLubyte* vendor = glGetString(GL_VENDOR);
    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* version = glGetString(GL_VERSION);
    if (!vendor || !renderer || !version) return false;

    gpu_info_.vendor_string = reinterpret_cast<const char*>(vendor);
    gpu_info_.renderer_string = reinterpret_cast<const char*>(renderer);
    gpu_info_.version_string = reinterpret_cast<const char*>(version);

    std::string vendor_str = gpu_info_.vendor_string + " " + gpu_info_.renderer_string;
    if (vendor_str.find("ARM") != std::string::npos || vendor_str.find("Mali") != std::string::npos) {
        gpu_info_.vendor = GPUVendor::Mali;
    } else if (vendor_str.find("Qualcomm") != std::string::npos || vendor_str.find("Adreno") != std::string::npos) {
        gpu_info_.vendor = GPUVendor::Adreno;
    } else if (vendor_str.find("Imagination") != std::string::npos || vendor_str.find("PowerVR") != std::string::npos ||
               vendor_str.find("PowerVR") != std::string::npos) {
        gpu_info_.vendor = GPUVendor::PowerVR;
    } else {
        gpu_info_.vendor = GPUVendor::Unknown;
    }

    return true;
}

bool GLESCRenderer::apply_driver_workarounds() {
    GPUCapabilities capabilities;
    capabilities.applyWorkarounds(config_);
    return true;
}

bool GLESCRenderer::make_current() {
    if (egl_surface_ == EGL_NO_SURFACE) return false;
    return eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_) == EGL_TRUE;
}

bool GLESCRenderer::onBeginFrame() {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!initialized_) return false;
    return true;
}

void GLESCRenderer::onEndFrame() {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!initialized_) return;
    glFinish();
}

void GLESCRenderer::onPresent() {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!initialized_) return;
    eglSwapBuffers(egl_display_, egl_surface_);
}

void GLESCRenderer::onResize(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!initialized_) return;
    surface_width_ = width;
    surface_height_ = height;
    glViewport(0, 0, width, height);
}

void GLESCRenderer::onWaitIdle() {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!initialized_) return;
    glFinish();
}

RendererBackend GLESCRenderer::getBackendImpl() const {
    return RendererBackend::OpenGLES;
}

std::string GLESCRenderer::getGpuRendererStringImpl() const {
    return gpu_info_.renderer_string;
}

std::string GLESCRenderer::getGpuVendorStringImpl() const {
    return gpu_info_.vendor_string;
}

std::string GLESCRenderer::getGpuVersionStringImpl() const {
    return gpu_info_.version_string;
}

void GLESCRenderer::reduceQuality() {
    // Reduce rendering quality for thermal throttling
}

void GLESCRenderer::onSurfaceChanged(uint32_t width, uint32_t height) {
    onResize(width, height);
}

void GLESCRenderer::onSurfaceDestroyed() {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (egl_surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(egl_display_, egl_surface_);
        egl_surface_ = EGL_NO_SURFACE;
    }
    eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

bool GLESCRenderer::detectGPU() {
    return query_gpu_info();
}

void GLESCRenderer::onApplyGPUWorkarounds(GPUVendor vendor, GPUArchitecture arch) {
    apply_driver_workarounds();
}

void GLESCRenderer::onOptimizeForGPU(GPUVendor vendor, GPUArchitecture arch) {
    // GPU-specific optimizations
}

bool GLESCRenderer::initializeManagers() {
    // Initialize GLES-specific managers
    return true;
}

} // namespace copper