#include "renderer_base.h"
#include "renderer_config.h"
#include "gpu_capabilities.h"
#include "buffer_manager.h"
#include "texture_manager.h"
#include "shader_manager.h"
#include "framebuffer_manager.h"
#include "state_manager.h"
#include "command_buffer.h"
#include "sync_manager.h"
#include "resource_pool.h"
#include "profiler.h"

#include <mutex>
#include <atomic>
#include <chrono>

namespace copper {

struct RendererBase::Impl {
    RendererConfig config;
    GPUVendor vendor = GPUVendor::Unknown;
    GPUArchitecture architecture = GPUArchitecture::Unknown;
    RendererFeature supportedFeatures = RendererFeature::None;
    std::vector<std::string> supportedExtensions;
    std::unique_ptr<BufferManager> bufferManager;
    std::unique_ptr<TextureManager> textureManager;
    std::unique_ptr<ShaderManager> shaderManager;
    std::unique_ptr<FramebufferManager> framebufferManager;
    std::unique_ptr<StateManager> stateManager;
    std::unique_ptr<SyncManager> syncManager;
    std::unique_ptr<ResourcePool> resourcePool;
    std::unique_ptr<Profiler> profiler;
    std::atomic<bool> initialized{false};
    std::atomic<bool> frameActive{false};
    uint32_t frameNumber = 0;
    uint32_t currentFrameIndex = 0;
    uint32_t maxFramesInFlight = 3;
    uint64_t frameStartTime = 0;
    double lastFrameTimeMs = 0.0;
    double lastCpuTimeMs = 0.0;
    double lastGpuTimeMs = 0.0;
    uint32_t drawCalls = 0;
    uint64_t gpuMemoryUsed = 0;
    uint64_t cpuMemoryUsed = 0;
    std::mutex mutex;
};

RendererBase::RendererBase() : pImpl(std::make_unique<Impl>()) {}
RendererBase::~RendererBase() = default;

bool RendererBase::initialize(const RendererConfig& config) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (pImpl->initialized) {
        return true;
    }

    pImpl->config = config;
    pImpl->maxFramesInFlight = config.maxFramesInFlight;

    if (!detectGPU()) {
        return false;
    }

    pImpl->initialized = true;
    return true;
}

void RendererBase::shutdown() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (!pImpl->initialized) {
        return;
    }

    waitIdle();

    pImpl->resourcePool.reset();
    pImpl->syncManager.reset();
    pImpl->stateManager.reset();
    pImpl->framebufferManager.reset();
    pImpl->shaderManager.reset();
    pImpl->textureManager.reset();
    pImpl->bufferManager.reset();
    pImpl->profiler.reset();

    pImpl->initialized = false;
}

bool RendererBase::beginFrame() {
    if (!pImpl->initialized || pImpl->frameActive) {
        return false;
    }

    pImpl->frameStartTime = getCurrentTimeNs();
    pImpl->frameActive = true;
    pImpl->drawCalls = 0;

    if (pImpl->profiler) {
        pImpl->profiler->beginFrame(pImpl->frameNumber);
    }

    return onBeginFrame();
}

void RendererBase::endFrame() {
    if (!pImpl->frameActive) {
        return;
    }

    onEndFrame();

    uint64_t frameEndTime = getCurrentTimeNs();
    pImpl->lastFrameTimeMs = (frameEndTime - pImpl->frameStartTime) / 1'000'000.0;

    if (pImpl->profiler) {
        pImpl->profiler->endFrame(pImpl->frameNumber, pImpl->lastFrameTimeMs, pImpl->lastCpuTimeMs, pImpl->lastGpuTimeMs, pImpl->drawCalls);
    }

    pImpl->frameActive = false;
    pImpl->frameNumber++;
    pImpl->currentFrameIndex = (pImpl->currentFrameIndex + 1) % pImpl->maxFramesInFlight;
}

void RendererBase::present() {
    onPresent();
}

void RendererBase::onSurfaceChanged(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    onResize(width, height);
    
    if (pImpl->framebufferManager) {
        pImpl->framebufferManager->onSurfaceChanged(width, height);
    }
}

void RendererBase::onSurfaceDestroyed() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->frameActive = false;

    if (pImpl->resourcePool) {
        pImpl->resourcePool->releaseAll();
    }
}

void RendererBase::setNativeWindow(void* /*native_window*/) {
    // Backends that own a native window should override this.
}

void RendererBase::onMemoryPressure(int level) {
    if (pImpl->resourcePool) {
        pImpl->resourcePool->trim(level);
    }
    if (pImpl->textureManager) {
        pImpl->textureManager->trimCache(level);
    }
    if (pImpl->bufferManager) {
        pImpl->bufferManager->trimPool(level);
    }
}

void RendererBase::onThermalThrottling(float temperatureRatio) {
    if (temperatureRatio >= pImpl->config.thermalThrottleThreshold) {
        if (pImpl->config.batterySaverMode) {
            reduceQuality();
        }
    }
}

void RendererBase::waitIdle() {
    onWaitIdle();
}

RendererBackend RendererBase::getBackend() const {
    return getBackendImpl();
}

bool RendererBase::isInitialized() const {
    return pImpl->initialized.load();
}

uint64_t RendererBase::getFrameNumber() const {
    return pImpl->frameNumber;
}

double RendererBase::getFrameTimeMs() const {
    return pImpl->lastFrameTimeMs;
}

double RendererBase::getCpuTimeMs() const {
    return pImpl->lastCpuTimeMs;
}

double RendererBase::getGpuTimeMs() const {
    return pImpl->lastGpuTimeMs;
}

uint32_t RendererBase::getDrawCalls() const {
    return pImpl->drawCalls;
}

uint64_t RendererBase::getGpuMemoryUsed() const {
    return pImpl->gpuMemoryUsed;
}

uint64_t RendererBase::getCpuMemoryUsed() const {
    return pImpl->cpuMemoryUsed;
}

std::string RendererBase::getGpuRendererString() const {
    return getGpuRendererStringImpl();
}

std::string RendererBase::getGpuVendorString() const {
    return getGpuVendorStringImpl();
}

std::string RendererBase::getGpuVersionString() const {
    return getGpuVersionStringImpl();
}

GPUVendor RendererBase::getGpuVendor() const {
    return pImpl->vendor;
}

GPUArchitecture RendererBase::getGpuArchitecture() const {
    return pImpl->architecture;
}

bool RendererBase::supportsFeature(RendererFeature feature) const {
    return (pImpl->supportedFeatures & feature) != RendererFeature::None;
}

bool RendererBase::isExtensionSupported(const std::string& extension) const {
    return std::find(pImpl->supportedExtensions.begin(), pImpl->supportedExtensions.end(), extension) != pImpl->supportedExtensions.end();
}

BufferManager* RendererBase::getBufferManager() {
    return pImpl->bufferManager.get();
}

TextureManager* RendererBase::getTextureManager() {
    return pImpl->textureManager.get();
}

ShaderManager* RendererBase::getShaderManager() {
    return pImpl->shaderManager.get();
}

FramebufferManager* RendererBase::getFramebufferManager() {
    return pImpl->framebufferManager.get();
}

StateManager* RendererBase::getStateManager() {
    return pImpl->stateManager.get();
}

SyncManager* RendererBase::getSyncManager() {
    return pImpl->syncManager.get();
}

CommandBuffer* RendererBase::getCommandBuffer() {
    return nullptr;
}

ResourcePool* RendererBase::getResourcePool() {
    return pImpl->resourcePool.get();
}

uint64_t RendererBase::getCurrentTimeNs() const {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

Profiler* RendererBase::getProfiler() {
    return pImpl->profiler.get();
}

const RendererConfig& RendererBase::getConfig() const {
    return pImpl->config;
}

void RendererBase::incrementDrawCalls(uint32_t count) {
    pImpl->drawCalls += count;
}

void RendererBase::updateMemoryStats(uint64_t gpuMem, uint64_t cpuMem) {
    pImpl->gpuMemoryUsed = gpuMem;
    pImpl->cpuMemoryUsed = cpuMem;
}

bool RendererBase::detectGPU() {
    GPUCapabilities caps;
    if (!caps.detect()) {
        return false;
    }

    pImpl->vendor = caps.getVendor();
    pImpl->architecture = caps.getArchitecture();
    pImpl->supportedFeatures = caps.getSupportedFeatures();
    pImpl->supportedExtensions = caps.getSupportedExtensions();

    applyGPUWorkarounds(pImpl->vendor, pImpl->architecture);
    optimizeForGPU(pImpl->vendor, pImpl->architecture);

    return true;
}

void RendererBase::applyGPUWorkarounds(GPUVendor vendor, GPUArchitecture arch) {
    onApplyGPUWorkarounds(vendor, arch);
}

void RendererBase::optimizeForGPU(GPUVendor vendor, GPUArchitecture arch) {
    onOptimizeForGPU(vendor, arch);
}

} // namespace copper