#pragma once

#include "renderer_base.h"
#include <string>
#include <memory>

namespace copper {

class RendererConfig {
public:
    RendererConfig();
    ~RendererConfig();

    RendererConfig(const RendererConfig& other) = default;
    RendererConfig& operator=(const RendererConfig& other) = default;
    RendererConfig(RendererConfig&& other) noexcept = default;
    RendererConfig& operator=(RendererConfig&& other) noexcept = default;

    // GPU-specific presets
    static RendererConfig createForGPU(GPUVendor vendor, GPUArchitecture arch);
    static RendererConfig createPerformance();
    static RendererConfig createBatterySaver();
    static RendererConfig createDebug();

    // Validation
    bool validate() const;
    void clampToValidRanges();

    // Apply GPU-specific optimizations
    void applyGPUOptimizations(GPUVendor vendor, GPUArchitecture arch);

    // Presets
    static const RendererConfig Default;
    static const RendererConfig Performance;
    static const RendererConfig BatterySaver;
    static const RendererConfig Debug;

    // Configuration options
    RendererBackend preferredBackend = RendererBackend::Auto;
    bool enableValidation = false;
    bool enableDebugMarkers = true;
    bool enableProfiling = true;
    bool enableMultithreadedRendering = true;
    bool enableAsyncShaderCompilation = true;
    bool enableAsyncResourceLoading = true;
    bool enableResourcePooling = true;
    bool enableCommandBufferReuse = true;
    bool enableStateCaching = true;
    bool enableDrawCallBatching = true;
    bool enablePipelineCaching = true;
    bool enableDescriptorCaching = true;
    bool enableTextureStreaming = true;
    bool enableTextureCompression = true;
    bool enableMipmapGeneration = true;
    uint32_t maxFramesInFlight = 3;
    uint32_t maxCommandBuffersPerFrame = 16;
    uint32_t maxDescriptorSets = 8192;
    uint32_t maxPushConstantsSize = 256;
    uint32_t textureCacheSizeMb = 256;
    uint32_t shaderCacheSizeMb = 64;
    uint32_t bufferPoolSizeMb = 128;
    uint32_t frameTimeoutMs = 5000;
    bool vsyncEnabled = true;
    uint32_t targetFps = 60;
    bool lowLatencyMode = false;
    bool batterySaverMode = false;
    bool thermalThrottlingAware = true;
    float thermalThrottleThreshold = 0.85f;
};

} // namespace copper