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

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <vma/vk_mem_alloc.h>  // VMA for memory allocation
#include <android/native_window.h>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <thread>
#include <atomic>

namespace copper {

class VulkanRenderer : public RendererBase {
public:
    VulkanRenderer();
    ~VulkanRenderer() override;

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
    RendererBackend getBackend() const override { return RendererBackend::Vulkan; }
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

private:
    // Vulkan instance & device
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue compute_queue_ = VK_NULL_HANDLE;
    VkQueue transfer_queue_ = VK_NULL_HANDLE;
    uint32_t graphics_queue_family_ = UINT32_MAX;
    uint32_t compute_queue_family_ = UINT32_MAX;
    uint32_t transfer_queue_family_ = UINT32_MAX;

    // Surface & swapchain
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    ANativeWindow* native_window_ = nullptr;
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;
    VkFormat swapchain_format_ = VK_FORMAT_B8G8R8A8_SRGB;
    VkColorSpaceKHR swapchain_color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D swapchain_extent_ = {0, 0};
    uint32_t current_frame_ = 0;
    uint32_t image_index_ = 0;

    // Synchronization
    std::vector<VkSemaphore> image_available_semaphores_;
    std::vector<VkSemaphore> render_finished_semaphores_;
    std::vector<VkFence> in_flight_fences_;
    std::vector<VkFence> images_in_flight_;

    // Memory allocator
    VmaAllocator allocator_ = VK_NULL_HANDLE;

    // Descriptor pool
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;

    // Pipeline cache
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;

    // Debug
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;

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
    bool create_instance();
    bool select_physical_device();
    bool create_logical_device();
    bool create_surface();
    bool create_swapchain();
    void destroy_swapchain();
    bool create_sync_objects();
    bool create_descriptor_pool();
    bool create_pipeline_cache();
    bool create_allocator();
    bool setup_debug_messenger();
    void query_gpu_info();
    void query_limits();
    void apply_driver_workarounds();

    // Frame management
    bool acquire_next_image();
    void submit_frame();
    void wait_for_fence(VkFence fence, uint64_t timeout = UINT64_MAX);

    // Resource cleanup
    void cleanup_frame_resources();

    // Debug function pointers
    PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT_ = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT_ = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT vkCmdBeginDebugUtilsLabelEXT_ = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT vkCmdEndDebugUtilsLabelEXT_ = nullptr;
    PFN_vkCmdInsertDebugUtilsLabelEXT vkCmdInsertDebugUtilsLabelEXT_ = nullptr;
    PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT_ = nullptr;
};

// Vulkan Buffer Manager
class VulkanBufferManager : public BufferManager {
public:
    VulkanBufferManager(VulkanRenderer* renderer);
    ~VulkanBufferManager() override;

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
    VulkanRenderer* renderer_;
    struct BufferData {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        Buffer base;
    };
    std::unordered_map<uint64_t, std::unique_ptr<BufferData>> buffers_;
    std::mutex buffers_mutex_;
    uint64_t next_handle_ = 1;
};

// Vulkan Texture Manager
class VulkanTextureManager : public TextureManager {
public:
    VulkanTextureManager(VulkanRenderer* renderer);
    ~VulkanTextureManager() override;

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
    VulkanRenderer* renderer_;
    struct TextureData {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        Texture base;
    };
    std::unordered_map<uint64_t, std::unique_ptr<TextureData>> textures_;
    std::mutex textures_mutex_;
    uint64_t next_handle_ = 1;
};

// Vulkan Shader Manager
class VulkanShaderManager : public ShaderManager {
public:
    VulkanShaderManager(VulkanRenderer* renderer);
    ~VulkanShaderManager() override;

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
    VulkanRenderer* renderer_;
    struct ShaderModuleData {
        VkShaderModule module = VK_NULL_HANDLE;
        ShaderModule base;
    };
    struct ShaderProgramData {
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        std::vector<VkDescriptorSetLayout> descriptor_set_layouts;
        std::vector<VkPipeline> pipelines; // One per render pass variant
        ShaderProgram base;
    };
    std::unordered_map<uint64_t, std::unique_ptr<ShaderModuleData>> modules_;
    std::unordered_map<uint64_t, std::unique_ptr<ShaderProgramData>> programs_;
    std::unordered_map<std::string, uint64_t> program_cache_;
    std::mutex shaders_mutex_;
    uint64_t next_handle_ = 1;
};

// Forward declare other Vulkan managers
class VulkanFramebufferManager : public FramebufferManager { /* ... */ };
class VulkanStateManager : public StateManager { /* ... */ };
class VulkanCommandBuffer : public CommandBuffer { /* ... */ };
class VulkanSyncManager : public SyncManager { /* ... */ };
class VulkanResourcePool : public ResourcePool { /* ... */ };
class VulkanProfiler : public Profiler { /* ... */ };

} // namespace copper