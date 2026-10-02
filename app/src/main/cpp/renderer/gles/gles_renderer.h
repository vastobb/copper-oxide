#pragma once

#include "renderer_base.h"
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
    bool initialize(const RendererConfig& config, void* window_handle) override;
    void shutdown() override;

    bool begin_frame() override;
    void end_frame() override;
    void present() override;

    BufferManager* get_buffer_manager() override { return buffer_manager_.get(); }
    TextureManager* get_texture_manager() override { return texture_manager_.get(); }
    ShaderManager* get_shader_manager() override { return shader_manager_.get(); }
    FramebufferManager* get_framebuffer_manager() override { return framebuffer_manager_.get(); }
    StateManager* get_state_manager() override { return state_manager_.get(); }
    CommandBuffer* get_command_buffer() override { return command_buffer_.get(); }
    SyncManager* get_sync_manager() override { return sync_manager_.get(); }
    ResourcePool* get_resource_pool() override { return resource_pool_.get(); }
    Profiler* get_profiler() override { return profiler_.get(); }

    const RendererConfig& get_config() const override { return config_; }
    const GPUInfo& get_gpu_info() const override { return gpu_info_; }
    const RendererLimits& get_limits() const override { return limits_; }
    RendererBackend get_backend() const override { return RendererBackend::OpenGLES; }
    bool is_initialized() const override { return initialized_; }

    const FrameStats& get_frame_stats() const override { return frame_stats_; }
    void reset_frame_stats() override;

    void on_surface_created(void* surface) override;
    void on_surface_changed(int width, int height) override;
    void on_surface_destroyed() override;

    void on_memory_pressure(int level) override;
    void on_thermal_throttling(float temperature_ratio) override;

    void set_debug_name(uint64_t object_handle, const std::string& name) override;
    void insert_debug_marker(const std::string& marker) override;
    void push_debug_group(const std::string& name) override;
    void pop_debug_group() override;

    bool supports_feature(RendererFeature feature) const override;
    bool is_extension_supported(const std::string& extension) const override;

    void wait_idle() override;

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
    std::unique_ptr<class GLESCBufferManager> buffer_manager_;
    std::unique_ptr<class GLESSTextureManager> texture_manager_;
    std::unique_ptr<class GLESShaderManager> shader_manager_;
    std::unique_ptr<class GLESCFramebufferManager> framebuffer_manager_;
    std::unique_ptr<class GLESCStateManager> state_manager_;
    std::unique_ptr<class GLESCCommandBuffer> command_buffer_;
    std::unique_ptr<class GLESCSyncManager> sync_manager_;
    std::unique_ptr<class GLESCResourcePool> resource_pool_;
    std::unique_ptr<class GLESCProfiler> profiler_;

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

    // Private methods
    bool initialize_egl(void* window_handle);
    void terminate_egl();
    bool create_egl_context();
    bool create_egl_surface();
    void destroy_egl_surface();
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

    uint64_t create_buffer(uint64_t size, uint32_t usage, uint32_t memory_flags) override;
    void destroy_buffer(uint64_t handle) override;
    void* map_buffer(uint64_t handle, uint64_t offset = 0, uint64_t size = 0) override;
    void unmap_buffer(uint64_t handle) override;
    void flush_buffer(uint64_t handle, uint64_t offset, uint64_t size) override;
    void invalidate_buffer(uint64_t handle, uint64_t offset, uint64_t size) override;
    void update_buffer(uint64_t handle, uint64_t offset, const void* data, uint64_t size) override;
    void copy_buffer(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset = 0, uint64_t dst_offset = 0) override;

    const Buffer* get_buffer(uint64_t handle) const override;
    uint64_t get_buffer_size(uint64_t handle) const override;
    void set_buffer_debug_name(uint64_t handle, const std::string& name) override;

    uint64_t allocate_from_pool(uint64_t size, uint32_t usage) override;
    void return_to_pool(uint64_t handle) override;
    void trim_pool() override;

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

    uint64_t create_texture_2d(uint32_t width, uint32_t height, uint32_t format, uint32_t usage, uint32_t mip_levels = 1) override;
    uint64_t create_texture_3d(uint32_t width, uint32_t height, uint32_t depth, uint32_t format, uint32_t usage) override;
    uint64_t create_texture_cube(uint32_t size, uint32_t format, uint32_t usage, uint32_t mip_levels = 1) override;
    uint64_t create_texture_array(uint32_t width, uint32_t height, uint32_t layers, uint32_t format, uint32_t usage) override;
    void destroy_texture(uint64_t handle) override;

    void upload_texture_data(uint64_t handle, uint32_t mip_level, uint32_t array_layer, const void* data, uint64_t size) override;
    void upload_compressed_texture_data(uint64_t handle, uint32_t mip_level, uint32_t array_layer, const void* data, uint64_t size, uint32_t format) override;
    void generate_mipmaps(uint64_t handle) override;
    void copy_texture(uint64_t src, uint64_t dst, uint32_t width, uint32_t height, uint32_t depth = 1) override;
    void blit_texture(uint64_t src, uint64_t dst, uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height, uint32_t filter) override;

    const Texture* get_texture(uint64_t handle) const override;
    void set_texture_debug_name(uint64_t handle, const std::string& name) override;

    uint64_t create_streaming_texture(uint32_t width, uint32_t height, uint32_t format) override;
    void update_streaming_texture(uint64_t handle, const void* data, uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;

    bool supports_format(uint32_t format) const override;
    uint32_t get_compressed_format(uint32_t base_format) const override;

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

    uint64_t create_shader_module(uint32_t stage, const uint32_t* spirv, uint32_t word_count, const std::string& entry_point = "main") override;
    void destroy_shader_module(uint64_t handle) override;

    uint64_t create_shader_program(const std::vector<uint64_t>& stages) override;
    void destroy_shader_program(uint64_t handle) override;

    bool compile_shader_from_source(uint32_t stage, const std::string& source, const std::string& entry_point, std::vector<uint32_t>& out_spirv, std::string& out_error) override;
    bool translate_glsl_to_spirv(uint32_t stage, const std::string& glsl, const std::string& entry_point, std::vector<uint32_t>& out_spirv, std::string& out_error) override;

    const ShaderModule* get_shader_module(uint64_t handle) const override;
    const ShaderProgram* get_shader_program(uint64_t handle) const override;

    int get_uniform_location(uint64_t program, const std::string& name) override;
    int get_attribute_location(uint64_t program, const std::string& name) override;
    int get_storage_buffer_binding(uint64_t program, const std::string& name) override;
    int get_texture_binding(uint64_t program, const std::string& name) override;
    int get_sampler_binding(uint64_t program, const std::string& name) override;

    void set_shader_debug_name(uint64_t handle, const std::string& name) override;

    void cache_shader_program(const std::string& key, uint64_t program) override;
    uint64_t get_cached_shader_program(const std::string& key) override;
    void clear_shader_cache() override;

    void precompile_shader_async(uint64_t program) override;
    bool is_shader_compiled(uint64_t program) override;
    void wait_for_shader_compilation(uint64_t program) override;

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