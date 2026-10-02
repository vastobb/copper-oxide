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

#include <GLES3/gl32.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
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

    BufferManager* getBufferManager() override { return buffer_manager_.get(); }
    TextureManager* getTextureManager() override { return texture_manager_.get(); }
    ShaderManager* getShaderManager() override { return shader_manager_.get(); }
    FramebufferManager* getFramebufferManager() override { return framebuffer_manager_.get(); }
    StateManager* getStateManager() override { return state_manager_.get(); }
    CommandBuffer* getCommandBuffer() override { return command_buffer_.get(); }
    SyncManager* getSyncManager() override { return sync_manager_.get(); }
    ResourcePool* getResourcePool() override { return resource_pool_.get(); }
    Profiler* getProfiler() override { return profiler_.get(); }

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

    // Backend-specific virtual methods (must implement)
    bool detectGPU() override;
    void onApplyGPUWorkarounds(GPUVendor vendor, GPUArchitecture arch) override;
    void onOptimizeForGPU(GPUVendor vendor, GPUArchitecture arch) override;
    bool initializeManagers() override;
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

    // Framebuffer
    GLuint default_framebuffer_ = 0;
    GLuint default_color_renderbuffer_ = 0;
    GLuint default_depth_renderbuffer_ = 0;

    // Sync
    std::vector<GLsync> fences_;
    uint32_t current_fence_ = 0;

    // Debug
    bool debug_ext_supported_ = false;
    bool khr_debug_supported_ = false;

    // Managers
    std::unique_ptr<BufferManager> buffer_manager_;
    std::unique_ptr<TextureManager> texture_manager_;
    std::unique_ptr<ShaderManager> shader_manager_;
    std::unique_ptr<FramebufferManager> framebuffer_manager_;
    std::unique_ptr<StateManager> state_manager_;
    std::unique_ptr<CommandBuffer> command_buffer_;
    std::unique_ptr<SyncManager> sync_manager_;
    std::unique_ptr<ResourcePool> resource_pool_;
    std::unique_ptr<Profiler> profiler_;

    // Config & state
    RendererConfig config_;
    GPUInfo gpu_info_;
    RendererLimits limits_;
    FrameStats frame_stats_;
    bool initialized_ = false;
    bool surface_created_ = false;

    // Threading
    std::thread shader_compiler_thread_;
    std::atomic<bool> shader_compiler_running_{false};
    std::mutex frame_mutex_;

    // Private methods
    bool init_egl();
    bool create_egl_context();
    void query_gpu_info();
    void query_limits();
    void query_extensions();
    void apply_driver_workarounds();
    void setup_debug_output();

    // Frame management
    bool make_current();
    void swap_buffers();

    // Resource cleanup
    void cleanup_frame_resources();
};

// GLES Buffer Manager
class GLESCBufferManager : public BufferManager {
public:
    GLESCBufferManager(GLESCRenderer* renderer);
    ~GLESCBufferManager() override;

    uint64_t createBuffer(uint64_t size, uint32_t usage, uint32_t memory_flags) override;
    void destroyBuffer(uint64_t handle) override;
    void* mapBuffer(uint64_t handle, uint64_t offset = 0, uint64_t size = 0) override;
    void unmapBuffer(uint64_t handle) override;
    void flushBuffer(uint64_t handle, uint64_t offset, uint64_t size) override;
    void invalidateBuffer(uint64_t handle, uint64_t offset, uint64_t size) override;
    void updateBuffer(uint64_t handle, uint64_t offset, const void* data, uint64_t size) override;
    void copyBuffer(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset = 0, uint64_t dst_offset = 0) override;

    const Buffer* getBuffer(uint64_t handle) const override;
    uint64_t getBufferSize(uint64_t handle) const override;
    void setBufferDebugName(uint64_t handle, const std::string& name) override;

    uint64_t allocateFromPool(uint64_t size, uint32_t usage) override;
    void returnToPool(uint64_t handle) override;
    void trimPool(int level) override;

private:
    GLESCRenderer* renderer_;
    struct BufferData {
        GLuint buffer = 0;
        Buffer base;
    };
    std::unordered_map<uint64_t, std::unique_ptr<BufferData>> buffers_;
    std::mutex buffers_mutex_;
    uint64_t next_handle_ = 1;
};

// GLES Texture Manager
class GLESSTextureManager : public TextureManager {
public:
    GLESSTextureManager(GLESCRenderer* renderer);
    ~GLESSTextureManager() override;

    uint64_t createTexture2D(uint32_t width, uint32_t height, uint32_t format, uint32_t usage, uint32_t mip_levels = 1) override;
    uint64_t createTexture3D(uint32_t width, uint32_t height, uint32_t depth, uint32_t format, uint32_t usage, uint32_t mip_levels = 1) override;
    uint64_t createTextureCube(uint32_t width, uint32_t height, uint32_t format, uint32_t usage, uint32_t mip_levels = 1) override;
    uint64_t createTextureArray(uint32_t width, uint32_t height, uint32_t array_layers, uint32_t format, uint32_t usage, uint32_t mip_levels = 1) override;
    void destroyTexture(uint64_t handle) override;

    void updateTexture(uint64_t handle, uint32_t mip_level, uint32_t array_layer, uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth, const void* data, uint64_t data_size) override;
    void copyTexture(uint64_t src, uint64_t dst, uint32_t src_mip, uint32_t dst_mip, uint32_t src_layer, uint32_t dst_layer) override;
    void generateMipmaps(uint64_t handle) override;

    uint64_t loadTextureFromMemory(const void* data, uint64_t size, uint32_t format, bool generate_mipmaps) override;
    uint64_t getOrCreateTexture(const std::string& key, std::function<uint64_t()> creator) override;

    void setTextureDebugName(uint64_t handle, const std::string& name) override;
    void trimCache(int level) override;
    size_t getCacheSizeMb() const override;
    void setMaxCacheSizeMb(size_t size_mb) override;

private:
    GLESCRenderer* renderer_;
    struct TextureData {
        GLuint texture = 0;
        GLenum target = GL_TEXTURE_2D;
        Texture base;
    };
    std::unordered_map<uint64_t, std::unique_ptr<TextureData>> textures_;
    std::mutex textures_mutex_;
    uint64_t next_handle_ = 1;
};

// GLES Shader Manager
class GLESShaderManager : public ShaderManager {
public:
    GLESShaderManager(GLESCRenderer* renderer);
    ~GLESShaderManager() override;

    uint64_t createShader(ShaderStage stage, const std::vector<uint32_t>& spirv, const std::string& entry_point = "main") override;
    uint64_t createShaderFromGLSL(ShaderStage stage, const std::string& glsl_source, const std::string& entry_point, const std::vector<std::string>& defines) override;
    void destroyShader(uint64_t handle) override;

    uint64_t createGraphicsPipeline(uint64_t vertex_shader, uint64_t fragment_shader, const PipelineLayoutDesc& layout) override;
    uint64_t createComputePipeline(uint64_t compute_shader, const PipelineLayoutDesc& layout) override;
    void destroyPipeline(uint64_t handle) override;

    uint64_t getOrCreateShader(const std::string& key, std::function<uint64_t()> creator) override;
    uint64_t getOrCreatePipeline(const std::string& key, std::function<uint64_t()> creator) override;

    void setShaderDebugName(uint64_t handle, const std::string& name) override;
    void setPipelineDebugName(uint64_t handle, const std::string& name) override;
    void addSpecializationConstant(uint64_t shader_handle, const std::string& name, uint32_t value) override;
    void compileAsync(const std::string& key, ShaderStage stage, const std::string& source, std::function<void(uint64_t)> callback) override;

private:
    GLESCRenderer* renderer_;
    struct ShaderModuleData {
        GLuint shader = 0;
        ShaderModule base;
    };
    struct ShaderProgramData {
        GLuint program = 0;
        ShaderProgram base;
    };
    std::unordered_map<uint64_t, std::unique_ptr<ShaderModuleData>> modules_;
    std::unordered_map<uint64_t, std::unique_ptr<ShaderProgramData>> programs_;
    std::unordered_map<std::string, uint64_t> program_cache_;
    std::mutex shaders_mutex_;
    uint64_t next_handle_ = 1;
};

// Forward declare other GLES managers
class GLESCFramebufferManager : public FramebufferManager { /* ... */ };
class GLESCStateManager : public StateManager { /* ... */ };
class GLESCCommandBuffer : public CommandBuffer { /* ... */ };
class GLESCSyncManager : public SyncManager { /* ... */ };
class GLESCResourcePool : public ResourcePool { /* ... */ };
class GLESCProfiler : public Profiler { /* ... */ };

} // namespace copper