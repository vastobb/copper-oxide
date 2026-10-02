#include "gles_renderer.h"
#include "renderer_base.h"
#include "gpu_capabilities.h"

#include <GLES3/gl32.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/log.h>
#include <android/native_window.h>
#include <algorithm>
#include <vector>
#include <string>
#include <mutex>

#define LOG_TAG "CopperOxide-GLES"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

namespace {

// Config attribute sets ordered from most to least demanding. Devices that
// cannot satisfy a strict set fall back to a weaker one instead of failing.
struct ConfigTemplate {
    EGLint alpha_size;
    EGLint depth_size;
    EGLint stencil_size;
    EGLint renderable_type;
};

const ConfigTemplate kConfigTemplates[] = {
    {8, 24, 8, EGL_OPENGL_ES3_BIT},
    {0, 24, 8, EGL_OPENGL_ES3_BIT},
    {8, 16, 0, EGL_OPENGL_ES3_BIT},
    {8, 16, 0, EGL_OPENGL_ES2_BIT},
};

} // namespace

GLESCRenderer::GLESCRenderer() : RendererBase() {}
GLESCRenderer::~GLESCRenderer() {
    shutdown();
}

bool GLESCRenderer::initialize(const RendererConfig& config) {
    if (initialized_) {
        return true;
    }

    config_ = config;

    if (!init_egl()) {
        return false;
    }
    if (!create_egl_context()) {
        destroy_egl();
        return false;
    }
    if (egl_surface_ == EGL_NO_SURFACE) {
        const bool ok = native_window_ ? create_window_surface() : create_info_surface();
        if (!ok) {
            LOGE("surface creation failed (EGL error 0x%x)", eglGetError());
            destroy_egl();
            return false;
        }
    }

    // RendererBase::initialize() runs GPU detection and flips pImpl->initialized,
    // which is what gates beginFrame(). Without it no frame is ever produced.
    if (!RendererBase::initialize(config)) {
        LOGE("renderer base initialization failed");
        destroy_egl();
        return false;
    }
    if (!initializeManagers()) {
        destroy_egl();
        RendererBase::shutdown();
        return false;
    }
    if (!apply_driver_workarounds()) {
        destroy_egl();
        RendererBase::shutdown();
        return false;
    }

    eglSwapInterval(egl_display_, config_.vsyncEnabled ? 1 : 0);

    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        initialized_ = true;
    }
    LOGI("GLES renderer initialized (EGL %d.%d)", egl_major_version_, egl_minor_version_);
    return true;
}

void GLESCRenderer::setNativeWindow(void* native_window) {
    ANativeWindow* next = static_cast<ANativeWindow*>(native_window);
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (next == native_window_) {
        return;
    }

    if (initialized_ && egl_display_ != EGL_NO_DISPLAY) {
        // Unbind before destroying so the surface is not current.
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(egl_display_, egl_surface_);
            egl_surface_ = EGL_NO_SURFACE;
        }
    }

    if (native_window_) {
        ANativeWindow_release(native_window_);
    }
    if (next) {
        ANativeWindow_acquire(next);
    }
    native_window_ = next;

    if (initialized_ && native_window_ && egl_display_ != EGL_NO_DISPLAY) {
        if (!create_window_surface()) {
            LOGE("create_window_surface failed (EGL error 0x%x)", eglGetError());
        } else {
            const int32_t width = ANativeWindow_getWidth(native_window_);
            const int32_t height = ANativeWindow_getHeight(native_window_);
            onResize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        }
    }
}

void GLESCRenderer::shutdown() {
    bool was_initialized = false;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (!initialized_ && egl_display_ == EGL_NO_DISPLAY) {
            return;
        }
        was_initialized = initialized_;
        initialized_ = false;
        destroy_egl();
    }

    if (native_window_) {
        ANativeWindow_release(native_window_);
        native_window_ = nullptr;
    }

    if (was_initialized) {
        // Must run outside frame_mutex_: RendererBase::shutdown() calls
        // waitIdle(), which takes frame_mutex_ again.
        RendererBase::shutdown();
    }
}

void GLESCRenderer::destroy_egl() {
    if (egl_display_ == EGL_NO_DISPLAY) {
        return;
    }
    if (egl_context_ != EGL_NO_CONTEXT) {
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(egl_display_, egl_context_);
        egl_context_ = EGL_NO_CONTEXT;
    }
    if (egl_surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(egl_display_, egl_surface_);
        egl_surface_ = EGL_NO_SURFACE;
    }
    eglTerminate(egl_display_);
    egl_display_ = EGL_NO_DISPLAY;
    egl_config_ = nullptr;
}

bool GLESCRenderer::init_egl() {
    egl_display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_display_ == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(egl_display_, &major, &minor)) {
        LOGE("eglInitialize failed (EGL error 0x%x)", eglGetError());
        egl_display_ = EGL_NO_DISPLAY;
        return false;
    }
    egl_major_version_ = major;
    egl_minor_version_ = minor;
    eglBindAPI(EGL_OPENGL_ES_API);

    // Both window and pbuffer surfaces must be supported: a window surface is
    // used when a Surface is available, otherwise a 1x1 pbuffer is used for
    // capability probing.
    for (const ConfigTemplate& tmpl : kConfigTemplates) {
        const EGLint config_attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, tmpl.alpha_size,
            EGL_DEPTH_SIZE, tmpl.depth_size,
            EGL_STENCIL_SIZE, tmpl.stencil_size,
            EGL_RENDERABLE_TYPE, tmpl.renderable_type,
            EGL_NONE
        };

        EGLint num_configs = 0;
        EGLConfig candidate = nullptr;
        if (eglChooseConfig(egl_display_, config_attribs, &candidate, 1, &num_configs) &&
            num_configs > 0) {
            egl_config_ = candidate;
            return true;
        }
    }

    LOGE("no suitable EGL config found (EGL error 0x%x)", eglGetError());
    eglTerminate(egl_display_);
    egl_display_ = EGL_NO_DISPLAY;
    return false;
}

bool GLESCRenderer::create_egl_context() {
    const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };

    egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, context_attribs);
    if (egl_context_ == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed (EGL error 0x%x)", eglGetError());
        return false;
    }
    return true;
}

bool GLESCRenderer::create_window_surface() {
    if (!native_window_ || egl_display_ == EGL_NO_DISPLAY) {
        return false;
    }
    egl_surface_ = eglCreateWindowSurface(egl_display_, egl_config_, native_window_, nullptr);
    return egl_surface_ != EGL_NO_SURFACE;
}

bool GLESCRenderer::create_info_surface() {
    const EGLint surface_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    egl_surface_ = eglCreatePbufferSurface(egl_display_, egl_config_, surface_attribs);
    return egl_surface_ != EGL_NO_SURFACE;
}

bool GLESCRenderer::query_gpu_info() {
    if (!make_current()) {
        return false;
    }

    const GLubyte* vendor = glGetString(GL_VENDOR);
    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* version = glGetString(GL_VERSION);
    if (!vendor || !renderer || !version) {
        LOGE("glGetString returned null (GL error 0x%x)", glGetError());
        return false;
    }

    gpu_info_.vendor_string = reinterpret_cast<const char*>(vendor);
    gpu_info_.renderer_string = reinterpret_cast<const char*>(renderer);
    gpu_info_.version_string = reinterpret_cast<const char*>(version);

    GLint gles_major = 0;
    GLint gles_minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &gles_major);
    glGetIntegerv(GL_MINOR_VERSION, &gles_minor);
    gles_major_version_ = gles_major;
    gles_minor_version_ = gles_minor;
    gpu_info_.gles_version = static_cast<uint32_t>(gles_major) * 10 + static_cast<uint32_t>(gles_minor);
    gpu_info_.supports_gles32 = (gles_major > 3) || (gles_major == 3 && gles_minor >= 2);

    query_extensions();
    query_limits();

    // Feature bits derived from the actually reported GL version.
    if (gpu_info_.supports_gles32) {
        supported_features_ = static_cast<RendererFeature>(
            static_cast<uint32_t>(supported_features_) |
            static_cast<uint32_t>(RendererFeature::ComputeShaders) |
            static_cast<uint32_t>(RendererFeature::GeometryShaders) |
            static_cast<uint32_t>(RendererFeature::TessellationShaders) |
            static_cast<uint32_t>(RendererFeature::ImageWriteWithoutFormat) |
            static_cast<uint32_t>(RendererFeature::SampleLocations) |
            static_cast<uint32_t>(RendererFeature::FragmentStoresAndAtomics));
    }
    if (isExtensionSupported("GL_NV_mesh_shader") || isExtensionSupported("GL_EXT_mesh_shader")) {
        supported_features_ = static_cast<RendererFeature>(
            static_cast<uint32_t>(supported_features_) | static_cast<uint32_t>(RendererFeature::MeshShaders));
    }
    if (isExtensionSupported("GL_QCOM_image_processing") || isExtensionSupported("GL_EXT_multisampled_render_to_texture")) {
        supported_features_ = static_cast<RendererFeature>(
            static_cast<uint32_t>(supported_features_) | static_cast<uint32_t>(RendererFeature::StorageImageExtendedFormats));
    }

    const std::string vendor_str = gpu_info_.vendor_string + " " + gpu_info_.renderer_string;
    if (vendor_str.find("ARM") != std::string::npos || vendor_str.find("Mali") != std::string::npos) {
        gpu_info_.vendor = GPUVendor::Mali;
    } else if (vendor_str.find("Qualcomm") != std::string::npos ||
               vendor_str.find("Adreno") != std::string::npos) {
        gpu_info_.vendor = GPUVendor::Adreno;
    } else if (vendor_str.find("Imagination") != std::string::npos ||
               vendor_str.find("PowerVR") != std::string::npos) {
        gpu_info_.vendor = GPUVendor::PowerVR;
    } else if (vendor_str.find("NVIDIA") != std::string::npos) {
        gpu_info_.vendor = GPUVendor::NVIDIA;
    } else if (vendor_str.find("AMD") != std::string::npos) {
        gpu_info_.vendor = GPUVendor::AMD;
    } else if (vendor_str.find("Intel") != std::string::npos) {
        gpu_info_.vendor = GPUVendor::Intel;
    } else if (vendor_str.find("Broadcom") != std::string::npos) {
        gpu_info_.vendor = GPUVendor::Broadcom;
    } else if (vendor_str.find("Vivante") != std::string::npos) {
        gpu_info_.vendor = GPUVendor::Vivante;
    } else if (vendor_str.find("VeriSilicon") != std::string::npos) {
        gpu_info_.vendor = GPUVendor::VeriSilicon;
    } else if (vendor_str.find("Apple") != std::string::npos) {
        gpu_info_.vendor = GPUVendor::Apple;
    } else {
        gpu_info_.vendor = GPUVendor::Unknown;
    }

    const std::string name = gpu_info_.renderer_string;
    switch (gpu_info_.vendor) {
        case GPUVendor::Adreno:
            gpu_info_.architecture = (name.find("830") != std::string::npos)
                                         ? GPUArchitecture::Adreno_800
                                         : ((name.find("7") != std::string::npos)
                                                ? GPUArchitecture::Adreno_700
                                                : GPUArchitecture::Adreno_600);
            break;
        case GPUVendor::Mali:
            gpu_info_.architecture = (name.find("G715") != std::string::npos ||
                                      name.find("G720") != std::string::npos)
                                         ? GPUArchitecture::Mali_G715
                                         : ((name.find("Valhall") != std::string::npos ||
                                             name.find("G7") != std::string::npos)
                                                ? GPUArchitecture::Mali_Valhall
                                                : ((name.find("Bifrost") != std::string::npos)
                                                       ? GPUArchitecture::Mali_Bifrost
                                                       : GPUArchitecture::Mali_Midgard));
            break;
        case GPUVendor::PowerVR:
            gpu_info_.architecture = (name.find("Furian") != std::string::npos)
                                         ? GPUArchitecture::PowerVR_Furian
                                         : ((name.find("BXM") != std::string::npos)
                                                ? GPUArchitecture::PowerVR_BXM
                                                : GPUArchitecture::PowerVR_Rogue);
            break;
        default:
            gpu_info_.architecture = GPUArchitecture::Unknown;
            break;
    }

    return true;
}

void GLESCRenderer::query_extensions() {
    gpu_info_.extensions.clear();
    const GLubyte* extension_string = glGetString(GL_EXTENSIONS);
    if (!extension_string) {
        return;
    }
    const std::string all(reinterpret_cast<const char*>(extension_string));
    size_t start = 0;
    while (start < all.size()) {
        const size_t end = all.find(' ', start);
        const std::string ext = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!ext.empty()) {
            gpu_info_.extensions.push_back(ext);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
}

void GLESCRenderer::query_limits() {
    GLint value = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &value);
    gpu_info_.max_texture_size = static_cast<uint32_t>(value);

    glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &value);
    gpu_info_.max_cube_map_texture_size = static_cast<uint32_t>(value);

    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &value);
    limits_.max_framebuffer_width = static_cast<uint32_t>(value);
    limits_.max_framebuffer_height = static_cast<uint32_t>(value);
    gpu_info_.max_renderbuffer_size = static_cast<uint32_t>(value);

    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &value);
    gpu_info_.max_color_attachments = static_cast<uint32_t>(value);
    if (static_cast<uint32_t>(value) < limits_.max_color_attachments) {
        limits_.max_color_attachments = static_cast<uint32_t>(value);
    }

    glGetIntegerv(GL_MAX_SAMPLES, &value);
    limits_.max_framebuffer_samples = value > 0 ? static_cast<uint32_t>(value) : 0u;
    gpu_info_.max_samples = limits_.max_framebuffer_samples;

    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &value);
    if (static_cast<uint32_t>(value) < limits_.max_vertex_attributes) {
        limits_.max_vertex_attributes = static_cast<uint32_t>(value);
    }

    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &value);
    gpu_info_.max_uniform_buffer_size = static_cast<uint32_t>(value);
}

bool GLESCRenderer::apply_driver_workarounds() {
    // Workarounds are applied through the shared capability layer so that the
    // GPU/driver specific behaviour lives in one place instead of being
    // scattered across backends.
    GPUCapabilities capabilities;
    capabilities.setDetectedInfo(gpu_info_.vendor, gpu_info_.architecture, gpu_info_.extensions,
                                supported_features_);
    capabilities.applyWorkarounds(config_);
    const GPUCapabilities::GPUOptimizationConfig& opt = capabilities.getOptimizationConfig();
    supported_features_ = static_cast<RendererFeature>(
        static_cast<uint32_t>(supported_features_) |
        static_cast<uint32_t>(opt.use_descriptor_indexing
                                  ? RendererFeature::DescriptorIndexing
                                  : RendererFeature::None) |
        static_cast<uint32_t>(opt.use_timeline_semaphores
                                  ? RendererFeature::TimelineSemaphore
                                  : RendererFeature::None));
    onOptimizeForGPU(gpu_info_.vendor, gpu_info_.architecture);
    return true;
}

bool GLESCRenderer::make_current() {
    if (egl_surface_ == EGL_NO_SURFACE) {
        return false;
    }
    return eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_) == EGL_TRUE;
}

bool GLESCRenderer::onBeginFrame() {
    if (!initialized_) {
        return false;
    }
    // The render thread comes from a coroutine pool and may differ between
    // frames, so the context must be bound explicitly.
    if (!make_current()) {
        LOGW("eglMakeCurrent failed (EGL error 0x%x)", eglGetError());
        return false;
    }
    return true;
}

void GLESCRenderer::onEndFrame() {
    // Deliberately no glFinish(): eglSwapBuffers already orders the work and a
    // full stall would destroy CPU/GPU overlap and frame pacing.
    glFlush();
}

void GLESCRenderer::onPresent() {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!initialized_ || egl_surface_ == EGL_NO_SURFACE) {
        return;
    }
    if (!make_current()) {
        return;
    }
    if (eglSwapBuffers(egl_display_, egl_surface_) != EGL_TRUE) {
        const EGLint error = eglGetError();
        if (error == EGL_CONTEXT_LOST) {
            LOGW("EGL context lost; signalling surface recreation");
            context_lost_ = true;
        } else {
            LOGW("eglSwapBuffers failed (EGL error 0x%x)", error);
        }
    }
}

void GLESCRenderer::onResize(uint32_t width, uint32_t height) {
    if (!initialized_ || width == 0 || height == 0) {
        return;
    }
    if (!make_current()) {
        return;
    }
    surface_width_ = static_cast<int>(width);
    surface_height_ = static_cast<int>(height);
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
}

void GLESCRenderer::onWaitIdle() {
    if (initialized_ && make_current()) {
        glFinish();
    }
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

void GLESCRenderer::onMemoryPressure(int level) {
    // Delegated to the base: it trims the resource pool, texture cache and
    // buffer pool. Resetting a manager here would hand out dangling pointers
    // to callers that already cached getBufferManager().
    RendererBase::onMemoryPressure(level);
}

void GLESCRenderer::onThermalThrottling(float temperatureRatio) {
    RendererBase::onThermalThrottling(temperatureRatio);
}

bool GLESCRenderer::supportsFeature(RendererFeature feature) const {
    return (static_cast<uint32_t>(feature) & static_cast<uint32_t>(supported_features_)) != 0;
}

bool GLESCRenderer::isExtensionSupported(const std::string& extension) const {
    for (const auto& ext : gpu_info_.extensions) {
        if (ext == extension) {
            return true;
        }
    }
    return false;
}

void GLESCRenderer::waitIdle() {
    if (!initialized_) {
        return;
    }
    if (make_current()) {
        glFinish();
    }
}

void GLESCRenderer::reduceQuality() {
    // Quality scaling is driven by the resource managers; nothing to change
    // for a baseline backend that does not manage its own render scale yet.
}

void GLESCRenderer::onSurfaceChanged(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }
    // Route through the base so the framebuffer manager also learns the new size.
    RendererBase::onSurfaceChanged(width, height);
}

void GLESCRenderer::onSurfaceDestroyed() {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (egl_display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(egl_display_, egl_surface_);
            egl_surface_ = EGL_NO_SURFACE;
        }
    }
}

bool GLESCRenderer::detectGPU() {
    return query_gpu_info();
}

void GLESCRenderer::onApplyGPUWorkarounds(GPUVendor vendor, GPUArchitecture arch) {
    apply_driver_workarounds();
}

void GLESCRenderer::onOptimizeForGPU(GPUVendor vendor, GPUArchitecture arch) {
    (void)vendor;
    (void)arch;
}

bool GLESCRenderer::initializeManagers() {
    // Manager creation is wired up by the backend-specific manager factories.
    // Until those exist the base-class managers stay null, which the render
    // loop tolerates because no draw commands can be submitted without a
    // surface-owned context anyway.
    return true;
}

} // namespace copper