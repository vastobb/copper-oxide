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

#include <android/log.h>

#include <algorithm>
#include <mutex>
#include <atomic>
#include <chrono>

// Logging stays local to this translation unit: the header must not grow a
// dependency on the platform log.
#define LOG_TAG "CopperOxide-Base"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace copper {

namespace {

// Shared helper for the managers the base owns: a factory that hands back
// null, or an initialize() that fails, is an initialization failure and is
// reported with the manager name.
template <typename Manager>
bool createSharedManager(RendererBase* owner,
                         std::unique_ptr<Manager> manager,
                         const char* name,
                         std::unique_ptr<Manager>& out) {
    if (!manager) {
        LOGE("%s: factory returned nullptr - the backend must override create%s()", name, name);
        return false;
    }
    if (!manager->initialize(owner)) {
        LOGE("%s: initialize(renderer) failed", name);
        return false;
    }
    out = std::move(manager);
    return true;
}

} // namespace

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
    // One command buffer per frame slot, owned by the base; the entries are
    // filled by createCommandBuffer().
    std::vector<std::unique_ptr<CommandBuffer>> commandBuffers;
    bool managersCreated = false;
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

template <typename Manager>
bool RendererBase::registerManager(std::unique_ptr<Manager>& slot,
                                   std::unique_ptr<Manager> manager,
                                   const char* name) {
    if (!manager) {
        LOGE("%s: registerManager() called with a nullptr", name);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        if (slot) {
            LOGE("%s: a manager is already registered", name);
            return false;
        }
    }

    // initialize() reads back through the renderer (getConfig(),
    // supportsFeature()), and a backend may have overridden those with
    // implementations that take the backend frame mutex - so it must not run
    // under pImpl->mutex (lock-order inversion).
    if (!manager->initialize(this)) {
        LOGE("%s: initialize(renderer) failed", name);
        return false;
    }

    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (slot) {
        // Registration is an initialization-time, single-threaded operation;
        // reaching this means a caller violated that.
        LOGE("%s: registered concurrently", name);
        return false;
    }
    slot = std::move(manager);
    return true;
}

bool RendererBase::registerBufferManager(std::unique_ptr<BufferManager> manager) {
    return registerManager(pImpl->bufferManager, std::move(manager), "BufferManager");
}

bool RendererBase::registerTextureManager(std::unique_ptr<TextureManager> manager) {
    return registerManager(pImpl->textureManager, std::move(manager), "TextureManager");
}

bool RendererBase::registerShaderManager(std::unique_ptr<ShaderManager> manager) {
    return registerManager(pImpl->shaderManager, std::move(manager), "ShaderManager");
}

bool RendererBase::registerFramebufferManager(std::unique_ptr<FramebufferManager> manager) {
    return registerManager(pImpl->framebufferManager, std::move(manager), "FramebufferManager");
}

bool RendererBase::registerStateManager(std::unique_ptr<StateManager> manager) {
    return registerManager(pImpl->stateManager, std::move(manager), "StateManager");
}

std::unique_ptr<Profiler> RendererBase::createProfiler() {
    return nullptr;
}

std::unique_ptr<SyncManager> RendererBase::createSyncManager() {
    return nullptr;
}

std::unique_ptr<ResourcePool> RendererBase::createResourcePool() {
    return nullptr;
}

std::unique_ptr<CommandBuffer> RendererBase::createCommandBuffer(uint32_t /*frame_index*/) {
    return nullptr;
}

bool RendererBase::initializeBackendManagers() {
    return true;
}

bool RendererBase::failManagerCreation(const char* reason) {
    LOGE("manager creation failed: %s", reason);
    destroyManagers();
    return false;
}

bool RendererBase::validateManagers() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);

    // A successful initialize() must leave every accessor usable; a manager
    // that is still missing here is the bug this validation exists to catch.
    const struct {
        const void* manager;
        const char* name;
    } required[] = {
        {pImpl->bufferManager.get(), "BufferManager"},
        {pImpl->textureManager.get(), "TextureManager"},
        {pImpl->shaderManager.get(), "ShaderManager"},
        {pImpl->framebufferManager.get(), "FramebufferManager"},
        {pImpl->stateManager.get(), "StateManager"},
        {pImpl->syncManager.get(), "SyncManager"},
        {pImpl->resourcePool.get(), "ResourcePool"},
        {pImpl->profiler.get(), "Profiler"},
    };
    for (const auto& entry : required) {
        if (!entry.manager) {
            LOGE("missing %s after initializeBackendManagers()", entry.name);
            return false;
        }
    }

    if (pImpl->commandBuffers.empty()) {
        LOGE("no command buffer slots were created");
        return false;
    }
    for (size_t slot = 0; slot < pImpl->commandBuffers.size(); ++slot) {
        if (!pImpl->commandBuffers[slot]) {
            LOGE("command buffer slot %zu is empty", slot);
            return false;
        }
    }
    return true;
}

bool RendererBase::createManagers() {
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        if (pImpl->managersCreated) {
            // Idempotent: initialize() may be called again after a successful
            // one, but the managers must not be rebuilt.
            return true;
        }
    }

    uint32_t frames_in_flight = 1;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        // Guard against a zero divisor: the frame index is taken modulo this.
        frames_in_flight = pImpl->maxFramesInFlight > 0 ? pImpl->maxFramesInFlight : 1;
    }

    // --- 1. shared managers -------------------------------------------------
    // Order: Profiler (pure CPU bookkeeping, no GPU ownership) -> SyncManager
    // (fences/semaphores guarding submissions) -> ResourcePool (allocates
    // through the backend hooks of the buffer/texture managers).
    std::unique_ptr<Profiler> profiler;
    std::unique_ptr<SyncManager> syncManager;
    std::unique_ptr<ResourcePool> resourcePool;
    std::vector<std::unique_ptr<CommandBuffer>> commandBuffers;

    if (!createSharedManager(this, createProfiler(), "Profiler", profiler)) {
        return failManagerCreation("Profiler unavailable");
    }
    if (!createSharedManager(this, createSyncManager(), "SyncManager", syncManager)) {
        return failManagerCreation("SyncManager unavailable");
    }
    if (!createSharedManager(this, createResourcePool(), "ResourcePool", resourcePool)) {
        return failManagerCreation("ResourcePool unavailable");
    }

    // One command buffer per frame in flight, owned by the base. The factory is
    // virtual and may take backend locks, so it runs without pImpl->mutex.
    commandBuffers.resize(frames_in_flight);
    for (uint32_t slot = 0; slot < frames_in_flight; ++slot) {
        std::unique_ptr<CommandBuffer> commandBuffer = createCommandBuffer(slot);
        if (!commandBuffer) {
            return failManagerCreation("createCommandBuffer() returned nullptr");
        }
        if (!commandBuffer->initialize(this, slot)) {
            return failManagerCreation("CommandBuffer::initialize() failed");
        }
        commandBuffers[slot] = std::move(commandBuffer);
    }

    {
        // Publish before the backend hook: initializeBackendManagers() may use
        // getSyncManager() and friends while it builds its own managers.
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->profiler = std::move(profiler);
        pImpl->syncManager = std::move(syncManager);
        pImpl->resourcePool = std::move(resourcePool);
        pImpl->commandBuffers = std::move(commandBuffers);
    }

    // --- 2. validate the shared set ----------------------------------------
    bool shared_complete = false;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        shared_complete = pImpl->profiler && pImpl->syncManager && pImpl->resourcePool &&
                          pImpl->commandBuffers.size() == frames_in_flight;
    }
    // Checked outside the lock: failManagerCreation() takes pImpl->mutex.
    if (!shared_complete) {
        return failManagerCreation("shared manager set is incomplete");
    }

    // --- 3. backend hook ----------------------------------------------------
    if (!initializeBackendManagers()) {
        return failManagerCreation("initializeBackendManagers() failed");
    }

    // --- 4. validate the complete set --------------------------------------
    if (!validateManagers()) {
        return failManagerCreation("incomplete manager set");
    }

    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->managersCreated = true;
    return true;
}

bool RendererBase::destroyManagers() {
    // The slots are emptied under pImpl->mutex and the managers are destroyed
    // outside of it: a destructor runs backend hooks (ResourcePool::onFree,
    // ShaderManager teardown, ...) that may call back into the accessors, and
    // those take pImpl->mutex. Destroying under the lock would self-deadlock.
    std::vector<std::unique_ptr<CommandBuffer>> commandBuffers;
    std::unique_ptr<StateManager> stateManager;
    std::unique_ptr<FramebufferManager> framebufferManager;
    std::unique_ptr<ResourcePool> resourcePool;
    std::unique_ptr<ShaderManager> shaderManager;
    std::unique_ptr<TextureManager> textureManager;
    std::unique_ptr<BufferManager> bufferManager;
    std::unique_ptr<SyncManager> syncManager;
    std::unique_ptr<Profiler> profiler;

    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        // Close the window for a racing beginFrame()/acquireCommandBuffer():
        // they now see a shut-down renderer instead of a half-destroyed one.
        pImpl->frameActive = false;
        pImpl->initialized = false;
        pImpl->managersCreated = false;

        commandBuffers = std::move(pImpl->commandBuffers);
        stateManager = std::move(pImpl->stateManager);
        framebufferManager = std::move(pImpl->framebufferManager);
        resourcePool = std::move(pImpl->resourcePool);
        shaderManager = std::move(pImpl->shaderManager);
        textureManager = std::move(pImpl->textureManager);
        bufferManager = std::move(pImpl->bufferManager);
        syncManager = std::move(pImpl->syncManager);
        profiler = std::move(pImpl->profiler);
    }

    // Explicit reverse-dependency destruction order (see the header): it must
    // not depend on the declaration order above.
    commandBuffers.clear();    // recorded commands reference everything below
    stateManager.reset();      // holds the bound pipeline/buffer/framebuffer
    framebufferManager.reset();// framebuffers reference texture image views
    resourcePool.reset();      // frees pooled buffers/textures through hooks
    shaderManager.reset();
    textureManager.reset();
    bufferManager.reset();
    syncManager.reset();       // fences/semaphores of the submissions above
    profiler.reset();          // no GPU ownership
    return true;
}

RendererBase::RendererBase() : pImpl(std::make_unique<Impl>()) {}

RendererBase::~RendererBase() {
    // Managers are intentionally NOT destroyed here: by the time the base
    // destructor runs the derived object is already gone, so its device and
    // context are gone too and a manager destructor would touch dead GPU
    // objects. shutdown() is where the ordered teardown happens (both backends
    // already call it from their destructors); this is only diagnostics.
    if (pImpl->managersCreated) {
        LOGE("RendererBase destroyed without shutdown(): managers leaked");
    }
}

bool RendererBase::initialize(const RendererConfig& configIn) {
    // Clamp rather than reject: callers integrate from Java where a single bad
    // value must not make the renderer unusable.
    RendererConfig config = configIn;
    config.clampToValidRanges();
    if (!config.validate()) {
        return false;
    }

    // detectGPU() is virtual and ends up taking the backend frame mutex, so it
    // must not be called while pImpl->mutex is held (lock-order inversion).
    if (!detectGPU()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        if (pImpl->initialized) {
            return true;
        }

        pImpl->config = config;
        // Guard against a zero divisor: the frame index is taken modulo this.
        pImpl->maxFramesInFlight = config.maxFramesInFlight > 0 ? config.maxFramesInFlight : 1;
        pImpl->currentFrameIndex = 0;
    }

    // The backends create their instance/device/context before calling this, so
    // the device is alive here - which is what the manager destructors on the
    // failure path below need. Manager creation is not a backend override
    // any more: the base drives createManagers()/initializeBackendManagers().
    if (!createManagers()) {
        // createManagers() has already destroyed everything it built.
        return false;
    }

    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (pImpl->profiler) {
        pImpl->profiler->beginFrame(pImpl->frameNumber);
    }

    pImpl->initialized = true;
    return true;
}

void RendererBase::shutdown() {
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        if (!pImpl->initialized) {
            return;
        }
    }

    // Ordering rule for every renderer:
    //   1. waitIdle() first - the managers own GPU resources and their
    //      destructors must not race the submissions still in flight.
    //   2. destroy the managers in reverse dependency order, still before the
    //      backend tears down its device/context.
    // Both backends already call RendererBase::shutdown() first for exactly
    // this reason; keep it that way (see also the class comment in
    // renderer_base.h).
    //
    // waitIdle() is virtual and acquires the backend frame mutex; calling it
    // under pImpl->mutex inverts the lock order and can deadlock.
    waitIdle();

    destroyManagers();

    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->frameActive = false;
    pImpl->initialized = false;
}

bool RendererBase::beginFrame() {
    if (!pImpl->initialized || pImpl->frameActive) {
        return false;
    }

    const uint64_t frameStart = getCurrentTimeNs();
    // Through the accessor (no lock is held here): a racing shutdown() can reset
    // the slot, and this keeps the read from tearing.
    if (Profiler* profiler = getProfiler()) {
        profiler->beginFrame(pImpl->frameNumber);
    }

    // The backend hook runs before frameActive is latched: a failed acquire
    // (for example VK_ERROR_OUT_OF_DATE_KHR during a resize) must not leave the
    // renderer permanently unable to start the next frame.
    if (!onBeginFrame()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->frameStartTime = frameStart;
    pImpl->drawCalls = 0;
    pImpl->frameActive = true;
    return true;
}

// The frame counter is the only thing a caller can observe about the frame
// lifecycle, so a mismatch between it and the callbacks the caller receives is
// otherwise invisible. These two lines are what make that mismatch diagnosable
// from a device log instead of requiring a debugger.
#define COPPER_FRAME_TRACE 1

void RendererBase::endFrame() {
    if (!pImpl->frameActive) {
#ifdef COPPER_FRAME_TRACE
        LOGW("endFrame() ignored: no frame is active");
#endif
        return;
    }

    onEndFrame();

    const uint64_t frameEndTime = getCurrentTimeNs();
    const double frameTimeMs = (frameEndTime - pImpl->frameStartTime) / 1'000'000.0;

    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->lastFrameTimeMs = frameTimeMs;
    // Until backend timestamp queries are wired up, the wall-clock frame time
    // is the only honest number we have; CPU time is measured up to submit.
    pImpl->lastCpuTimeMs = frameTimeMs;
    pImpl->lastGpuTimeMs = 0.0;
    pImpl->frameActive = false;
    pImpl->frameNumber++;

    if (pImpl->profiler) {
        pImpl->profiler->endFrame(pImpl->frameNumber, frameTimeMs, frameTimeMs, 0.0, pImpl->drawCalls);
    }
#ifdef COPPER_FRAME_TRACE
    LOGI("frame %llu complete in %.3f ms, %u draw calls", (unsigned long long)pImpl->frameNumber,
         frameTimeMs, pImpl->drawCalls);
#endif
}

void RendererBase::present() {
    onPresent();
}

void RendererBase::onSurfaceChanged(uint32_t width, uint32_t height) {
    // The virtual onResize() takes the backend frame mutex, so it must run
    // before pImpl->mutex is acquired.
    onResize(width, height);

    // Grab the manager, then call it with pImpl->mutex released: onSurfaceChanged
    // is virtual and a backend override may run hooks that call back into the
    // accessors, which take pImpl->mutex.
    FramebufferManager* framebufferManager = getFramebufferManager();
    if (framebufferManager) {
        framebufferManager->onSurfaceChanged(width, height);
    }
}

void RendererBase::onSurfaceDestroyed() {
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->frameActive = false;
    }

    // Outside the lock for the same reason: releaseAll() runs the backend's
    // onFree hook, which may call back into the accessors.
    if (ResourcePool* resourcePool = getResourcePool()) {
        resourcePool->releaseAll();
    }
}

void RendererBase::setNativeWindow(void* /*native_window*/) {
    // Backends that own a native window should override this.
}

void RendererBase::onMemoryPressure(int level) {
    // Through the accessors: they take pImpl->mutex, which is not held here.
    if (ResourcePool* resourcePool = getResourcePool()) {
        resourcePool->trim(level);
    }
    if (TextureManager* textureManager = getTextureManager()) {
        textureManager->trimCache(level);
    }
    if (BufferManager* bufferManager = getBufferManager()) {
        bufferManager->trimPool(level);
    }
}

void RendererBase::onThermalThrottling(float temperatureRatio) {
    RendererConfig config;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        config = pImpl->config;
    }

    const bool throttling_aware = config.thermalThrottlingAware || config.batterySaverMode;
    if (!throttling_aware) {
        return;
    }
    if (temperatureRatio >= config.thermalThrottleThreshold) {
        reduceQuality();
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
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->bufferManager.get();
}

TextureManager* RendererBase::getTextureManager() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->textureManager.get();
}

ShaderManager* RendererBase::getShaderManager() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->shaderManager.get();
}

FramebufferManager* RendererBase::getFramebufferManager() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->framebufferManager.get();
}

StateManager* RendererBase::getStateManager() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->stateManager.get();
}

SyncManager* RendererBase::getSyncManager() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->syncManager.get();
}

CommandBuffer* RendererBase::getCommandBuffer() {
    // Render-thread only: resolves to the command buffer of the current frame
    // slot. Null before a successful initialize() and after shutdown().
    if (!pImpl->initialized.load()) {
        return nullptr;
    }
    return acquireCommandBuffer(frameIndex());
}

void RendererBase::setFrameIndex(uint32_t frame_index) {
    // Render-thread only - see the header.
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->currentFrameIndex = frame_index;
}

uint32_t RendererBase::frameIndex() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->currentFrameIndex;
}

CommandBuffer* RendererBase::acquireCommandBuffer(uint32_t frame_index) {
    uint32_t slot = 0;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        // Guard against a zero divisor: the index is taken modulo this.
        const uint32_t frames_in_flight = pImpl->maxFramesInFlight > 0 ? pImpl->maxFramesInFlight : 1;
        slot = frame_index % frames_in_flight;
        if (slot < pImpl->commandBuffers.size()) {
            if (CommandBuffer* existing = pImpl->commandBuffers[slot].get()) {
                return existing;
            }
        }
    }

    // The factory is virtual and may take backend locks, and
    // CommandBuffer::initialize() reads the renderer back: neither runs under
    // pImpl->mutex. The slot is claimed when publishing.
    std::unique_ptr<CommandBuffer> created = createCommandBuffer(slot);
    if (!created) {
        LOGE("createCommandBuffer(%u) returned nullptr", slot);
        return nullptr;
    }
    if (!created->initialize(this, slot)) {
        LOGE("CommandBuffer::initialize() failed for frame slot %u", slot);
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (pImpl->commandBuffers.size() <= slot) {
        pImpl->commandBuffers.resize(slot + 1);
    }
    if (!pImpl->commandBuffers[slot]) {
        pImpl->commandBuffers[slot] = std::move(created);
    }
    return pImpl->commandBuffers[slot].get();
}

ResourcePool* RendererBase::getResourcePool() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->resourcePool.get();
}

uint64_t RendererBase::getCurrentTimeNs() const {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

Profiler* RendererBase::getProfiler() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
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