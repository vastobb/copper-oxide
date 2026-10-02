#include "renderer_config.h"
#include "gpu_capabilities.h"

#include <algorithm>

namespace copper {

RendererConfig::RendererConfig() = default;
RendererConfig::~RendererConfig() = default;

// Preset configurations
const RendererConfig RendererConfig::Default = RendererConfig();

const RendererConfig RendererConfig::Performance = [] {
    RendererConfig config;
    config.enableValidation = false;
    config.enableProfiling = false;
    config.targetFps = 120;
    config.lowLatencyMode = true;
    return config;
}();

const RendererConfig RendererConfig::BatterySaver = [] {
    RendererConfig config;
    config.targetFps = 30;
    config.batterySaverMode = true;
    config.maxFramesInFlight = 2;
    config.textureCacheSizeMb = 128;
    config.bufferPoolSizeMb = 64;
    return config;
}();

const RendererConfig RendererConfig::Debug = [] {
    RendererConfig config;
    config.enableValidation = true;
    config.enableDebugMarkers = true;
    config.enableProfiling = true;
    config.targetFps = 60;
    return config;
}();

RendererConfig RendererConfig::createForGPU(GPUVendor vendor, GPUArchitecture arch) {
    RendererConfig config;

    switch (vendor) {
        case GPUVendor::Adreno: {
            config.preferredBackend = RendererBackend::Vulkan;
            config.enableValidation = false;
            config.enableDebugMarkers = true;
            config.enableProfiling = true;
            config.enableMultithreadedRendering = true;
            config.enableAsyncShaderCompilation = true;
            config.enableAsyncResourceLoading = true;
            config.enableResourcePooling = true;
            config.enableCommandBufferReuse = true;
            config.enableStateCaching = true;
            config.enableDrawCallBatching = true;
            config.enablePipelineCaching = true;
            config.enableDescriptorCaching = true;
            config.enableTextureStreaming = true;
            config.enableTextureCompression = true;
            config.enableMipmapGeneration = true;

            switch (arch) {
                case GPUArchitecture::Adreno_800: // Snapdragon 8 Gen 3/4
                    config.maxFramesInFlight = 3;
                    config.maxCommandBuffersPerFrame = 16;
                    config.maxDescriptorSets = 8192;
                    config.maxPushConstantsSize = 256;
                    config.textureCacheSizeMb = 512;
                    config.shaderCacheSizeMb = 128;
                    config.bufferPoolSizeMb = 256;
                    config.targetFps = 120;
                    config.lowLatencyMode = true;
                    config.vsyncEnabled = false;
                    break;
                case GPUArchitecture::Adreno_700: // Snapdragon 8 Gen 1/2, 7 Gen 1/2/3
                    config.maxFramesInFlight = 3;
                    config.maxCommandBuffersPerFrame = 12;
                    config.maxDescriptorSets = 4096;
                    config.maxPushConstantsSize = 256;
                    config.textureCacheSizeMb = 256;
                    config.shaderCacheSizeMb = 64;
                    config.bufferPoolSizeMb = 128;
                    config.targetFps = 90;
                    config.lowLatencyMode = true;
                    config.vsyncEnabled = false;
                    break;
                case GPUArchitecture::Adreno_600:
                    config.maxFramesInFlight = 3;
                    config.maxCommandBuffersPerFrame = 8;
                    config.maxDescriptorSets = 2048;
                    config.maxPushConstantsSize = 128;
                    config.textureCacheSizeMb = 128;
                    config.shaderCacheSizeMb = 32;
                    config.bufferPoolSizeMb = 64;
                    config.targetFps = 60;
                    config.vsyncEnabled = true;
                    break;
                default:
                    config.maxFramesInFlight = 3;
                    config.maxCommandBuffersPerFrame = 8;
                    config.maxDescriptorSets = 2048;
                    config.maxPushConstantsSize = 128;
                    config.textureCacheSizeMb = 128;
                    config.shaderCacheSizeMb = 32;
                    config.bufferPoolSizeMb = 64;
                    config.targetFps = 60;
                    config.vsyncEnabled = true;
                    break;
            }

            config.frameTimeoutMs = 5000;
            config.batterySaverMode = false;
            config.thermalThrottlingAware = true;
            config.thermalThrottleThreshold = 0.85f;
            break;
        }

        case GPUVendor::Mali: {
            config.preferredBackend = RendererBackend::Vulkan;
            config.enableValidation = false;
            config.enableDebugMarkers = true;
            config.enableProfiling = true;
            config.enableMultithreadedRendering = true;
            config.enableAsyncShaderCompilation = true;
            config.enableAsyncResourceLoading = true;
            config.enableResourcePooling = true;
            config.enableCommandBufferReuse = true;
            config.enableStateCaching = true;
            config.enableDrawCallBatching = true;
            config.enablePipelineCaching = true;
            config.enableDescriptorCaching = true;
            config.enableTextureStreaming = true;
            config.enableTextureCompression = true;
            config.enableMipmapGeneration = true;

            switch (arch) {
                case GPUArchitecture::Mali_G715:
                case GPUArchitecture::Mali_Valhall: // Mali-G710, G715, G720, Immortalis
                    config.maxFramesInFlight = 3;
                    config.maxCommandBuffersPerFrame = 16;
                    config.maxDescriptorSets = 8192;
                    config.maxPushConstantsSize = 256;
                    config.textureCacheSizeMb = 512;
                    config.shaderCacheSizeMb = 128;
                    config.bufferPoolSizeMb = 256;
                    config.targetFps = 120;
                    config.lowLatencyMode = true;
                    config.vsyncEnabled = false;
                    break;
                case GPUArchitecture::Mali_Bifrost: // Mali-G76, G77, G78
                    config.maxFramesInFlight = 3;
                    config.maxCommandBuffersPerFrame = 12;
                    config.maxDescriptorSets = 4096;
                    config.maxPushConstantsSize = 256;
                    config.textureCacheSizeMb = 256;
                    config.shaderCacheSizeMb = 64;
                    config.bufferPoolSizeMb = 128;
                    config.targetFps = 90;
                    config.lowLatencyMode = true;
                    config.vsyncEnabled = false;
                    break;
                case GPUArchitecture::Mali_Midgard: // Mali-G71, G72
                    config.maxFramesInFlight = 3;
                    config.maxCommandBuffersPerFrame = 8;
                    config.maxDescriptorSets = 2048;
                    config.maxPushConstantsSize = 128;
                    config.textureCacheSizeMb = 128;
                    config.shaderCacheSizeMb = 32;
                    config.bufferPoolSizeMb = 64;
                    config.targetFps = 60;
                    config.vsyncEnabled = true;
                    break;
                default:
                    config.maxFramesInFlight = 3;
                    config.maxCommandBuffersPerFrame = 8;
                    config.maxDescriptorSets = 2048;
                    config.maxPushConstantsSize = 128;
                    config.textureCacheSizeMb = 128;
                    config.shaderCacheSizeMb = 32;
                    config.bufferPoolSizeMb = 64;
                    config.targetFps = 60;
                    config.vsyncEnabled = true;
                    break;
            }

            config.frameTimeoutMs = 5000;
            config.batterySaverMode = false;
            config.thermalThrottlingAware = true;
            config.thermalThrottleThreshold = 0.85f;
            break;
        }

        case GPUVendor::PowerVR: {
            config.preferredBackend = RendererBackend::Vulkan;
            config.enableValidation = false;
            config.enableDebugMarkers = true;
            config.enableProfiling = true;
            config.enableMultithreadedRendering = true;
            config.enableAsyncShaderCompilation = true;
            config.enableAsyncResourceLoading = true;
            config.enableResourcePooling = true;
            config.enableCommandBufferReuse = true;
            config.enableStateCaching = true;
            config.enableDrawCallBatching = true;
            config.enablePipelineCaching = true;
            config.enableDescriptorCaching = true;
            config.enableTextureStreaming = true;
            config.enableTextureCompression = true;
            config.enableMipmapGeneration = true;

            switch (arch) {
                case GPUArchitecture::PowerVR_BXM:
                case GPUArchitecture::PowerVR_Furian:
                    config.maxFramesInFlight = 3;
                    config.maxCommandBuffersPerFrame = 12;
                    config.maxDescriptorSets = 4096;
                    config.maxPushConstantsSize = 256;
                    config.textureCacheSizeMb = 256;
                    config.shaderCacheSizeMb = 64;
                    config.bufferPoolSizeMb = 128;
                    config.targetFps = 90;
                    config.lowLatencyMode = true;
                    config.vsyncEnabled = false;
                    break;
                case GPUArchitecture::PowerVR_Rogue:
                    config.maxFramesInFlight = 3;
                    config.maxCommandBuffersPerFrame = 8;
                    config.maxDescriptorSets = 2048;
                    config.maxPushConstantsSize = 128;
                    config.textureCacheSizeMb = 128;
                    config.shaderCacheSizeMb = 32;
                    config.bufferPoolSizeMb = 64;
                    config.targetFps = 60;
                    config.vsyncEnabled = true;
                    break;
                default:
                    config.maxFramesInFlight = 3;
                    config.maxCommandBuffersPerFrame = 8;
                    config.maxDescriptorSets = 2048;
                    config.maxPushConstantsSize = 128;
                    config.textureCacheSizeMb = 128;
                    config.shaderCacheSizeMb = 32;
                    config.bufferPoolSizeMb = 64;
                    config.targetFps = 60;
                    config.vsyncEnabled = true;
                    break;
            }

            config.frameTimeoutMs = 5000;
            config.batterySaverMode = false;
            config.thermalThrottlingAware = true;
            config.thermalThrottleThreshold = 0.85f;
            break;
        }

        case GPUVendor::Apple:
        case GPUVendor::NVIDIA:
        case GPUVendor::AMD:
        case GPUVendor::Intel:
        default: {
            config = RendererConfig::Default;
            break;
        }
    }

    return config;
}

RendererConfig RendererConfig::createPerformance() {
    RendererConfig config = Default;
    config.enableValidation = false;
    config.enableProfiling = false;
    config.targetFps = 120;
    config.lowLatencyMode = true;
    config.vsyncEnabled = false;
    return config;
}

RendererConfig RendererConfig::createBatterySaver() {
    RendererConfig config = Default;
    config.targetFps = 30;
    config.batterySaverMode = true;
    config.maxFramesInFlight = 2;
    config.textureCacheSizeMb = 128;
    config.bufferPoolSizeMb = 64;
    config.vsyncEnabled = true;
    return config;
}

RendererConfig RendererConfig::createDebug() {
    RendererConfig config = Default;
    config.enableValidation = true;
    config.enableDebugMarkers = true;
    config.enableProfiling = true;
    config.targetFps = 60;
    return config;
}

bool RendererConfig::validate() const {
    if (maxFramesInFlight < 2 || maxFramesInFlight > 4) return false;
    if (maxCommandBuffersPerFrame < 4 || maxCommandBuffersPerFrame > 32) return false;
    if (maxDescriptorSets < 256 || maxDescriptorSets > 32768) return false;
    if (maxPushConstantsSize < 64 || maxPushConstantsSize > 512) return false;
    if (textureCacheSizeMb < 32 || textureCacheSizeMb > 1024) return false;
    if (shaderCacheSizeMb < 16 || shaderCacheSizeMb > 256) return false;
    if (bufferPoolSizeMb < 32 || bufferPoolSizeMb > 512) return false;
    if (frameTimeoutMs < 1000 || frameTimeoutMs > 30000) return false;
    if (targetFps < 15 || targetFps > 144) return false;
    if (thermalThrottleThreshold < 0.5f || thermalThrottleThreshold > 1.0f) return false;

    return true;
}

void RendererConfig::applyGPUOptimizations(GPUVendor vendor, GPUArchitecture arch) {
    RendererConfig optimized = createForGPU(vendor, arch);

    this->preferredBackend = optimized.preferredBackend;
    this->maxFramesInFlight = optimized.maxFramesInFlight;
    this->maxCommandBuffersPerFrame = optimized.maxCommandBuffersPerFrame;
    this->maxDescriptorSets = optimized.maxDescriptorSets;
    this->maxPushConstantsSize = optimized.maxPushConstantsSize;
    this->textureCacheSizeMb = optimized.textureCacheSizeMb;
    this->shaderCacheSizeMb = optimized.shaderCacheSizeMb;
    this->bufferPoolSizeMb = optimized.bufferPoolSizeMb;
    this->targetFps = optimized.targetFps;
    this->lowLatencyMode = optimized.lowLatencyMode;
    this->vsyncEnabled = optimized.vsyncEnabled;
}

} // namespace copper