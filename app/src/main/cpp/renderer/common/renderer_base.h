#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace copper {

// Forward declarations
class BufferManager;
class TextureManager;
class ShaderManager;
class FramebufferManager;
class StateManager;
class CommandBuffer;
class SyncManager;
class ResourcePool;
class Profiler;

enum class RendererBackend : uint8_t {
    Unknown = 0,
    Vulkan = 1,
    OpenGLES = 2,
    Auto = 255
};

enum class RendererFeature : uint32_t {
    None = 0,
    ComputeShaders = 1 << 0,
    GeometryShaders = 1 << 1,
    TessellationShaders = 1 << 2,
    IndirectDraw = 1 << 3,
    MultiDrawIndirect = 1 << 4,
    DrawIndirectCount = 1 << 5,
    BindlessTextures = 1 << 6,
    BindlessSamplers = 1 << 7,
    DescriptorIndexing = 1 << 8,
    ShaderDrawParameters = 1 << 9,
    SubgroupOperations = 1 << 10,
    MeshShaders = 1 << 11,
    TaskShaders = 1 << 12,
    RayTracing = 1 << 13,
    VariableRateShading = 1 << 14,
    ConservativeRasterization = 1 << 15,
    DepthBoundsTest = 1 << 16,
    SampleLocations = 1 << 17,
    FragmentStoresAndAtomics = 1 << 18,
    ImageWriteWithoutFormat = 1 << 19,
    StorageImageExtendedFormats = 1 << 20,
    UniformBufferStandardLayout = 1 << 21,
    ScalarBlockLayout = 1 << 22,
    ImagelessFramebuffer = 1 << 23,
    TimelineSemaphore = 1 << 24,
    BufferDeviceAddress = 1 << 25,
    HostQueryReset = 1 << 26,
    DynamicRendering = 1 << 27,
    Synchronization2 = 1 << 28,
    MaintenanceFeatures = 1 << 29,
    All = 0xFFFFFFFF
};

inline RendererFeature operator|(RendererFeature a, RendererFeature b) {
    return static_cast<RendererFeature>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline RendererFeature operator&(RendererFeature a, RendererFeature b) {
    return static_cast<RendererFeature>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

enum class GPUVendor : uint8_t {
    Unknown = 0,
    Adreno = 1,
    Mali = 2,
    PowerVR = 3,
    Apple = 4,
    NVIDIA = 5,
    AMD = 6,
    Intel = 7,
    Broadcom = 8,
    Vivante = 9,
    VeriSilicon = 10
};

enum class GPUArchitecture : uint8_t {
    Unknown = 0,
    // Adreno
    Adreno_600 = 1,
    Adreno_700 = 2,
    Adreno_800 = 3,
    // Mali
    Mali_Midgard = 10,
    Mali_Bifrost = 11,
    Mali_Valhall = 12,
    Mali_G715 = 13,
    // PowerVR
    PowerVR_Rogue = 20,
    PowerVR_Furian = 21,
    PowerVR_BXM = 22,
};

struct GPUInfo {
    GPUVendor vendor = GPUVendor::Unknown;
    GPUArchitecture architecture = GPUArchitecture::Unknown;
    std::string renderer_string;
    std::string version_string;
    std::string vendor_string;
    uint32_t api_version = 0;
    uint32_t driver_version = 0;
    uint64_t device_id = 0;
    bool is_tiled_renderer = false;
    bool supports_vulkan = false;
    uint32_t vulkan_version = 0;
    bool supports_gles32 = false;
    uint32_t gles_version = 0;
    uint32_t max_compute_work_group_count[3] = {0, 0, 0};
    uint32_t max_compute_work_group_size[3] = {0, 0, 0};
    uint32_t max_compute_shared_memory_size = 0;
    uint32_t max_uniform_buffer_size = 0;
    uint32_t max_storage_buffer_size = 0;
    uint32_t max_vertex_uniform_vectors = 0;
    uint32_t max_fragment_uniform_vectors = 0;
    uint32_t max_texture_size = 0;
    uint32_t max_3d_texture_size = 0;
    uint32_t max_cube_map_texture_size = 0;
    uint32_t max_renderbuffer_size = 0;
    uint32_t max_draw_buffers = 0;
    uint32_t max_color_attachments = 0;
    uint32_t max_samples = 0;
    bool has_performance_counters = false;
    std::vector<std::string> extensions;
    std::vector<std::string> driver_bugs;
};

struct RendererConfig {
    RendererBackend preferred_backend = RendererBackend::Auto;
    bool enable_validation = false;
    bool enable_debug_markers = true;
    bool enable_profiling = true;
    bool enable_multithreaded_rendering = true;
    bool enable_async_shader_compilation = true;
    bool enable_async_resource_loading = true;
    bool enable_resource_pooling = true;
    bool enable_command_buffer_reuse = true;
    bool enable_state_caching = true;
    bool enable_draw_call_batching = true;
    bool enable_pipeline_caching = true;
    bool enable_descriptor_caching = true;
    bool enable_texture_streaming = true;
    bool enable_texture_compression = true;
    bool enable_mipmap_generation = true;
    uint32_t max_frames_in_flight = 3;
    uint32_t max_command_buffers_per_frame = 16;
    uint32_t max_descriptor_sets = 8192;
    uint32_t max_push_constants_size = 256;
    uint32_t texture_cache_size_mb = 256;
    uint32_t shader_cache_size_mb = 64;
    uint32_t buffer_pool_size_mb = 128;
    uint32_t frame_timeout_ms = 5000;
    bool vsync_enabled = true;
    uint32_t target_fps = 0; // 0 = unlimited
    bool low_latency_mode = false;
    bool battery_saver_mode = false;
    bool thermal_throttling_aware = true;
    float thermal_throttle_threshold = 0.85f;
    RendererFeature required_features = RendererFeature::None;
    RendererFeature optional_features = RendererFeature::All;
    std::vector<std::string> disabled_extensions;
    std::vector<std::string> forced_extensions;
};

struct FrameStats {
    uint64_t frame_number = 0;
    double frame_time_ms = 0.0;
    double cpu_time_ms = 0.0;
    double gpu_time_ms = 0.0;
    uint32_t draw_calls = 0;
    uint32_t triangles_rendered = 0;
    uint32_t vertices_processed = 0;
    uint32_t state_changes = 0;
    uint32_t buffer_binds = 0;
    uint32_t texture_binds = 0;
    uint32_t shader_binds = 0;
    uint32_t pipeline_binds = 0;
    uint32_t render_pass_count = 0;
    uint64_t gpu_memory_used = 0;
    uint64_t cpu_memory_used = 0;
    uint32_t active_resources = 0;
    uint32_t shader_compilations = 0;
    uint32_t pipeline_compilations = 0;
};

struct RendererLimits {
    uint32_t max_vertex_attributes = 16;
    uint32_t max_vertex_binding = 16;
    uint32_t max_vertex_attribute_offset = 2047;
    uint32_t max_vertex_attribute_stride = 2048;
    uint32_t max_uniform_buffer_range = 65536;
    uint32_t max_storage_buffer_range = 134217728;
    uint32_t max_push_constants_size = 256;
    uint32_t max_sampler_anisotropy = 16;
    uint32_t max_viewport_dimensions[2] = {16384, 16384};
    uint32_t max_framebuffer_width = 16384;
    uint32_t max_framebuffer_height = 16384;
    uint32_t max_framebuffer_layers = 256;
    uint32_t max_framebuffer_samples = 8;
    uint32_t max_color_attachments = 8;
    uint32_t max_descriptor_set_bindings = 16;
    uint32_t max_per_stage_descriptor_uniform_buffers = 16;
    uint32_t max_per_stage_descriptor_storage_buffers = 16;
    uint32_t max_per_stage_descriptor_sampled_images = 16;
    uint32_t max_per_stage_descriptor_samplers = 16;
    uint32_t max_per_stage_descriptor_input_attachments = 16;
    uint32_t max_per_stage_resources = 64;
    uint32_t max_descriptor_set_uniform_buffers = 16;
    uint32_t max_descriptor_set_storage_buffers = 16;
    uint32_t max_descriptor_set_sampled_images = 16;
    uint32_t max_descriptor_set_samplers = 16;
    uint32_t max_descriptor_set_input_attachments = 16;
};

class RendererBase {
public:
    virtual ~RendererBase() = default;

    // Initialization
    virtual bool initialize(const RendererConfig& config) = 0;
    virtual void shutdown() = 0;

    // Rendering loop
    virtual bool beginFrame() = 0;
    virtual void endFrame() = 0;
    virtual void present() = 0;

    // Resource management
    virtual BufferManager* getBufferManager() = 0;
    virtual TextureManager* getTextureManager() = 0;
    virtual ShaderManager* getShaderManager() = 0;
    virtual FramebufferManager* getFramebufferManager() = 0;
    virtual StateManager* getStateManager() = 0;
    virtual CommandBuffer* getCommandBuffer() = 0;
    virtual SyncManager* getSyncManager() = 0;
    virtual ResourcePool* getResourcePool() = 0;
    virtual Profiler* getProfiler() = 0;

    // Configuration & info
    virtual const RendererConfig& getConfig() const = 0;
    virtual RendererBackend getBackend() const = 0;
    virtual bool isInitialized() const = 0;

    // Frame statistics
    virtual uint64_t getFrameNumber() const = 0;
    virtual double getFrameTimeMs() const = 0;
    virtual double getCpuTimeMs() const = 0;
    virtual double getGpuTimeMs() const = 0;
    virtual uint32_t getDrawCalls() const = 0;
    virtual uint64_t getGpuMemoryUsed() const = 0;
    virtual uint64_t getCpuMemoryUsed() const = 0;

    // GPU info
    virtual std::string getGpuRendererString() const = 0;
    virtual std::string getGpuVendorString() const = 0;
    virtual std::string getGpuVersionString() const = 0;
    virtual GPUVendor getGpuVendor() const = 0;
    virtual GPUArchitecture getGpuArchitecture() const = 0;

    // Surface handling
    virtual void onSurfaceChanged(uint32_t width, uint32_t height) = 0;
    virtual void onSurfaceDestroyed() = 0;

    // Memory pressure
    virtual void onMemoryPressure(int level) = 0;
    virtual void onThermalThrottling(float temperatureRatio) = 0;

    // Feature queries
    virtual bool supportsFeature(RendererFeature feature) const = 0;
    virtual bool isExtensionSupported(const std::string& extension) const = 0;

    // Wait for idle
    virtual void waitIdle() = 0;
};

using RendererFactory = std::function<std::unique_ptr<RendererBase>()>;

struct RendererRegistry {
    static RendererRegistry& instance() {
        static RendererRegistry registry;
        return registry;
    }

    void register_renderer(RendererBackend backend, RendererFactory factory) {
        factories_[static_cast<uint8_t>(backend)] = std::move(factory);
    }

    std::unique_ptr<RendererBase> create_renderer(RendererBackend backend) const {
        auto it = factories_.find(static_cast<uint8_t>(backend));
        if (it != factories_.end()) {
            return it->second();
        }
        return nullptr;
    }

private:
    RendererRegistry() = default;
    std::unordered_map<uint8_t, RendererFactory> factories_;
};

#define COPPER_REGISTER_RENDERER(Backend, Class) \
    namespace { \
        struct Class##_Registrar { \
            Class##_Registrar() { \
                copper::RendererRegistry::instance().register_renderer( \
                    copper::RendererBackend::Backend, \
                    []() { return std::make_unique<Class>(); } \
                ); \
            } \
        }; \
        static Class##_Registrar g_##Class##_registrar; \
    }

} // namespace copper