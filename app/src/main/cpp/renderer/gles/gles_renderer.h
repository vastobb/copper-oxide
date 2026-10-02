#pragma once

#include "renderer_base.h"
#include "buffer_manager.h"
#include "texture_manager.h"
#include "shader_manager.h"
#include "framebuffer_manager.h"
#include "state_manager.h"
#include "command_buffer.h"
#include "sync_manager.h"
#include "resource_pool.h"
#include "profiler.h"

// Backend managers. These own the real GL objects; the renderer only holds
// std::unique_ptr to the base interfaces.
#include "gles_buffer_manager.h"
#include "gles_texture_manager.h"
#include "gles_shader_manager.h"
#include "gles_state_manager.h"
#include "gles_framebuffer_manager.h"
#include "gles_sync_manager.h"
#include "gles_resource_pool.h"
#include "gles_profiler.h"
#include "gles_command_buffer.h"  // also declares GLESCommandSink

#include <GLES3/gl32.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/native_window.h>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <thread>
#include <atomic>

namespace copper {

class GLESCRenderer : public RendererBase {
public:
    GLESCRenderer();
    ~GLESCRenderer() override;

    // RendererBase interface
    bool initialize(const RendererConfig& config) override;
    void shutdown() override;


    const RendererConfig& getConfig() const override { return config_; }
    RendererBackend getBackend() const override { return RendererBackend::OpenGLES; }
    bool isInitialized() const override { return initialized_; }

    uint64_t getFrameNumber() const override { return frame_stats_.frame_number; }
    double getFrameTimeMs() const override { return frame_stats_.frame_time_ms; }
    double getCpuTimeMs() const override { return frame_stats_.cpu_time_ms; }
    double getGpuTimeMs() const override { return frame_stats_.gpu_time_ms; }
    uint32_t getDrawCalls() const override { return frame_stats_.draw_calls; }
    uint64_t getGpuMemoryUsed() const override { return frame_stats_.gpu_memory_used; }
    uint64_t getCpuMemoryUsed() const override { return frame_stats_.cpu_memory_used; }

    std::string getGpuRendererString() const override { return gpu_info_.renderer_string; }
    std::string getGpuVendorString() const override { return gpu_info_.vendor_string; }
    std::string getGpuVersionString() const override { return gpu_info_.version_string; }
    GPUVendor getGpuVendor() const override { return gpu_info_.vendor; }
    GPUArchitecture getGpuArchitecture() const override { return gpu_info_.architecture; }

    void onSurfaceChanged(uint32_t width, uint32_t height) override;
    void onSurfaceDestroyed() override;

    void onMemoryPressure(int level) override;
    void onThermalThrottling(float temperatureRatio) override;

    bool supportsFeature(RendererFeature feature) const override;
    bool isExtensionSupported(const std::string& extension) const override;

    void waitIdle() override;
    void setNativeWindow(void* native_window) override;

    // Backend-specific virtual methods (must implement)
    bool detectGPU() override;
    void onApplyGPUWorkarounds(GPUVendor vendor, GPUArchitecture arch) override;
    void onOptimizeForGPU(GPUVendor vendor, GPUArchitecture arch) override;
    // Manager construction. The base owns creation, validation, ordering and
    // teardown; these factories and hooks only supply the backend objects.
    std::unique_ptr<Profiler> createProfiler() override;
    std::unique_ptr<SyncManager> createSyncManager() override;
    std::unique_ptr<ResourcePool> createResourcePool() override;
    std::unique_ptr<CommandBuffer> createCommandBuffer(uint32_t frame_index) override;
    bool initializeBackendManagers() override;

    // Re-points every per-frame command buffer at the sink. Called after
    // registration and whenever the EGL context is created or lost, because the
    // sink refuses to issue GL calls without a current context.
    bool refreshCommandBufferSinks();

    GLESCommandSink* commandSink() { return command_sink_.get(); }
    bool onBeginFrame() override;
    void onEndFrame() override;
    void onPresent() override;
    void onResize(uint32_t width, uint32_t height) override;
    void onWaitIdle() override;
    RendererBackend getBackendImpl() const override;
    std::string getGpuRendererStringImpl() const override;
    std::string getGpuVendorStringImpl() const override;
    std::string getGpuVersionStringImpl() const override;
    void reduceQuality() override;

    // EGL access
    EGLDisplay get_egl_display() const { return egl_display_; }
    EGLContext get_egl_context() const { return egl_context_; }
    EGLSurface get_egl_surface() const { return egl_surface_; }

private:
    // EGL
    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLContext egl_context_ = EGL_NO_CONTEXT;
    EGLSurface egl_surface_ = EGL_NO_SURFACE;
    EGLConfig egl_config_ = nullptr;
    int egl_major_version_ = 0;
    int egl_minor_version_ = 0;

    // OpenGL ES
    int gles_major_version_ = 0;
    int gles_minor_version_ = 0;

    // Surface
    ANativeWindow* native_window_ = nullptr;
    int surface_width_ = 0;
    int surface_height_ = 0;

    // Context health
    std::atomic<bool> context_lost_{false};

    // Managers are owned by RendererBase after initialization; this renderer
    // only supplies factories and wires the backend resolvers.

    std::unique_ptr<GLESCommandSink> command_sink_;

    // Config & state
    RendererConfig config_;
    GPUInfo gpu_info_;
    RendererLimits limits_;
    FrameStats frame_stats_;
    RendererFeature supported_features_ = RendererFeature::None;
    bool initialized_ = false;
    bool surface_created_ = false;

    // Threading
    std::mutex frame_mutex_;

    // Private methods
    bool init_egl();
    bool create_egl_context();
    bool create_window_surface();
    bool create_info_surface();
    void destroy_egl();
    bool query_gpu_info();
    void query_limits();
    void query_extensions();
    bool apply_driver_workarounds();
    void setup_debug_output();

    // Frame management
    bool make_current();
    void swap_buffers();

    // Resource cleanup
    void cleanup_frame_resources();
};

} // namespace copper