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

// Backend managers. These own the real Vulkan objects; the renderer only holds
// std::unique_ptr to the base interfaces.
#include "vulkan_buffer_manager.h"
#include "vulkan_texture_manager.h"
#include "vulkan_shader_manager.h"
#include "vulkan_state_manager.h"
#include "vulkan_framebuffer_manager.h"
#include "vulkan_sync_manager.h"
#include "vulkan_resource_pool.h"
#include "vulkan_profiler.h"
#include "vulkan_command_buffer.h"
#include "vulkan_command_sink.h"

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <vk_mem_alloc.h>  // VMA for memory allocation
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
    // Manager construction. The base owns creation, validation, ordering and
    // teardown; these factories and hooks only supply the backend objects.
    std::unique_ptr<Profiler> createProfiler() override;
    std::unique_ptr<SyncManager> createSyncManager() override;
    std::unique_ptr<ResourcePool> createResourcePool() override;
    std::unique_ptr<CommandBuffer> createCommandBuffer(uint32_t frame_index) override;
    bool initializeBackendManagers() override;

    // Re-points every per-frame command buffer at the current sink. Called after
    // registration and after any swapchain recreation that changes the framebuffer
    // list, because the sink caches that list.
    bool refreshCommandBufferSinks();

    // Accessors the backend managers need. Exposing them keeps the managers
    // free of friend declarations and avoids duplicating Vulkan handle state.
    VkDevice device() const { return device_; }
    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physical_device_; }
    VkQueue graphicsQueue() const { return graphics_queue_; }
    uint32_t graphicsQueueFamily() const { return graphics_queue_family_; }
    VkRenderPass renderPass() const { return render_pass_; }
    VkPipelineCache pipelineCache() const { return pipeline_cache_; }
    uint32_t framesInFlight() const { return frames_in_flight_; }
    VkCommandPool commandPool() const { return command_pool_; }
    const std::vector<VkCommandBuffer>& commandBuffers() const { return command_buffers_; }
    const std::vector<VkFence>& inFlightFences() const { return in_flight_fences_; }
    VkExtent2D swapchainExtent() const { return swapchain_extent_; }
    VmaAllocator allocator() const { return allocator_; }
    const std::vector<VkFramebuffer>& swapchainFramebuffers() const { return swapchain_framebuffers_; }

    // The per-frame command sink. Owned here because it borrows the frame's
    // VkCommandBuffer and the render pass, both of which belong to the renderer.
    VulkanCommandSink* commandSink() { return command_sink_.get(); }
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

    // Command recording
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers_;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> swapchain_framebuffers_;
    uint32_t frames_in_flight_ = 2;

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

    std::unique_ptr<BufferManager> buffer_manager_;
    std::unique_ptr<TextureManager> texture_manager_;
    std::unique_ptr<ShaderManager> shader_manager_;
    std::unique_ptr<FramebufferManager> framebuffer_manager_;
    std::unique_ptr<StateManager> state_manager_;
    std::unique_ptr<CommandBuffer> command_buffer_;
    std::unique_ptr<SyncManager> sync_manager_;
    std::unique_ptr<ResourcePool> resource_pool_;

    std::unique_ptr<VulkanCommandSink> command_sink_;

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
    bool create_command_pool();
    bool create_render_pass();
    bool create_swapchain_framebuffers();
    bool recreate_swapchain();
    bool create_instance();
    bool select_physical_device();
    bool create_logical_device();
    bool create_surface();
    bool create_swapchain();
    bool destroy_swapchain();
    bool create_sync_objects();
    bool create_descriptor_pool();
    bool create_pipeline_cache();
    bool create_allocator();
    bool setup_debug_messenger();
    void query_gpu_info();
    void query_limits();
    void apply_driver_workarounds();

    // Frame management
    void wait_for_fence(VkFence fence, uint64_t timeout);

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

} // namespace copper