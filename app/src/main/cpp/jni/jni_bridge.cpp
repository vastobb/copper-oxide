#include "jni_bridge.h"
#include "vulkan_renderer.h"
#include "gles_renderer.h"
#include "renderer_config.h"
#include "gpu_capabilities.h"
#include "buffer_manager.h"
#include "texture_manager.h"
#include "shader_manager.h"
#include "shader_translator.h"
#include "state_manager.h"
#include "command_buffer.h"

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <string>
#include <memory>
#include <vector>

#define LOG_TAG "CopperOxide-JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace copper {

static std::mutex g_renderer_mutex;
// One renderer per CopperOxideRenderer instance, keyed by its owner tag.
//
// This started as a single process-global slot. More than one
// CopperOxideRenderer can exist in a process - the activity creates one, and an
// instrumentation test running against that app creates another - and with a
// single slot the second initialize() displaced the first instance's renderer
// mid-frame. The first instance's render loop then kept running against a
// renderer it no longer owned and reported the other's statistics: frameNumber
// 0 with a frame time of 0.0 ms, while the native trace showed frames 1 to 63
// completing normally. A registry removes the interference instead of trying to
// police it: each instance owns its renderer and nothing else can reach it.
static std::unordered_map<uint64_t, std::unique_ptr<RendererBase>> g_renderers;
static JavaVM* g_jvm = nullptr;

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_jvm = vm;
    LOGI("JNI_OnLoad");
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
    std::vector<std::unique_ptr<RendererBase>> renderers;
    {
        std::lock_guard<std::mutex> lock(g_renderer_mutex);
        renderers.reserve(g_renderers.size());
        for (auto& entry : g_renderers) {
            renderers.push_back(std::move(entry.second));
        }
        g_renderers.clear();
    }
    // Outside the lock: shutdown() blocks on fences and the device queue.
    for (auto& renderer : renderers) {
        renderer->shutdown();
    }
    g_jvm = nullptr;
    LOGI("JNI_OnUnload");
}

JNIEnv* getJNIEnv() {
    JNIEnv* env;
    if (g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            return nullptr;
        }
    }
    return env;
}

void setRenderer(std::unique_ptr<RendererBase> renderer, uint64_t owner) {
    std::unique_ptr<RendererBase> replaced;
    {
        std::lock_guard<std::mutex> lock(g_renderer_mutex);
        auto it = g_renderers.find(owner);
        if (it != g_renderers.end()) {
            // Re-initializing the same instance: the old renderer must be shut
            // down rather than dropped, or its device and context leak.
            replaced = std::move(it->second);
            g_renderers.erase(it);
        }
        g_renderers.emplace(owner, std::move(renderer));
    }
    if (replaced) {
        replaced->shutdown();
    }
}


namespace {

// StateManager stores at most this many vertex binding slots; a higher binding
// index would be silently dropped by the array it writes into.
constexpr uint32_t kMaxVertexBindings = 16;

// Holds the renderer alive for the duration of one JNI call. Shutdown() can
// otherwise destroy the renderer between the lookup and the caller's first use,
// which is a use-after-free.
// Returns the renderer this instance installed, or null once it has been shut
// down or was never initialized.
RendererBase* renderer_for_owner(uint64_t owner) {
    std::lock_guard<std::mutex> lock(g_renderer_mutex);
    const auto it = g_renderers.find(owner);
    return it == g_renderers.end() ? nullptr : it->second.get();
}

class RendererGuard {
public:
    explicit RendererGuard(uint64_t owner) {
        g_renderer_mutex.lock();
        const auto it = g_renderers.find(owner);
        renderer_ = it == g_renderers.end() ? nullptr : it->second.get();
    }
    ~RendererGuard() { g_renderer_mutex.unlock(); }

    RendererGuard(const RendererGuard&) = delete;
    RendererGuard& operator=(const RendererGuard&) = delete;

    RendererBase* get() const { return renderer_; }
    explicit operator bool() const { return renderer_ != nullptr; }

private:
    RendererBase* renderer_ = nullptr;
};

} // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeInitialize(
    JNIEnv* env, jobject thiz,
    jobject surface,
    jint preferredBackend,
    jboolean enableValidation,
    jboolean enableDebugMarkers,
    jboolean enableProfiling,
    jboolean enableMultithreaded,
    jboolean enableAsyncShaderCompilation,
    jboolean enableAsyncResourceLoading,
    jboolean enableResourcePooling,
    jboolean enableCommandBufferReuse,
    jboolean enableStateCaching,
    jboolean enableDrawCallBatching,
    jboolean enablePipelineCaching,
    jboolean enableDescriptorCaching,
    jboolean enableTextureStreaming,
    jboolean enableTextureCompression,
    jboolean enableMipmapGeneration,
    jint maxFramesInFlight,
    jint maxCommandBuffersPerFrame,
    jint maxDescriptorSets,
    jint maxPushConstantsSize,
    jint textureCacheSizeMb,
    jint shaderCacheSizeMb,
    jint bufferPoolSizeMb,
    jint frameTimeoutMs,
    jboolean vsyncEnabled,
    jint targetFps,
    jboolean lowLatencyMode,
    jboolean batterySaverMode,
    jboolean thermalThrottlingAware,
    jfloat thermalThrottleThreshold,
    jlong owner
) {
    LOGI("nativeInitialize: backend=%d owner=%lld", preferredBackend,
         static_cast<long long>(owner));

    RendererConfig config;
    config.preferredBackend = static_cast<RendererBackend>(preferredBackend);
    config.enableValidation = enableValidation;
    config.enableDebugMarkers = enableDebugMarkers;
    config.enableProfiling = enableProfiling;
    config.enableMultithreadedRendering = enableMultithreaded;
    config.enableAsyncShaderCompilation = enableAsyncShaderCompilation;
    config.enableAsyncResourceLoading = enableAsyncResourceLoading;
    config.enableResourcePooling = enableResourcePooling;
    config.enableCommandBufferReuse = enableCommandBufferReuse;
    config.enableStateCaching = enableStateCaching;
    config.enableDrawCallBatching = enableDrawCallBatching;
    config.enablePipelineCaching = enablePipelineCaching;
    config.enableDescriptorCaching = enableDescriptorCaching;
    config.enableTextureStreaming = enableTextureStreaming;
    config.enableTextureCompression = enableTextureCompression;
    config.enableMipmapGeneration = enableMipmapGeneration;
    config.maxFramesInFlight = maxFramesInFlight;
    config.maxCommandBuffersPerFrame = maxCommandBuffersPerFrame;
    config.maxDescriptorSets = maxDescriptorSets;
    config.maxPushConstantsSize = maxPushConstantsSize;
    config.textureCacheSizeMb = textureCacheSizeMb;
    config.shaderCacheSizeMb = shaderCacheSizeMb;
    config.bufferPoolSizeMb = bufferPoolSizeMb;
    config.frameTimeoutMs = frameTimeoutMs;
    config.vsyncEnabled = vsyncEnabled;
    config.targetFps = targetFps;
    config.lowLatencyMode = lowLatencyMode;
    config.batterySaverMode = batterySaverMode;
    config.thermalThrottlingAware = thermalThrottlingAware;
    config.thermalThrottleThreshold = thermalThrottleThreshold;

    std::unique_ptr<RendererBase> renderer;

    ANativeWindow* native_window = nullptr;
    if (surface != nullptr) {
        native_window = ANativeWindow_fromSurface(env, surface);
    }

    if (config.preferredBackend == RendererBackend::Vulkan || config.preferredBackend == RendererBackend::Auto) {
        renderer = std::make_unique<VulkanRenderer>();
        if (native_window) {
            renderer->setNativeWindow(native_window);
        }
        if (!renderer->initialize(config)) {
            LOGE("Vulkan initialization failed, falling back to GLES");
            renderer = std::make_unique<GLESCRenderer>();
            if (native_window) {
                renderer->setNativeWindow(native_window);
            }
            if (!renderer->initialize(config)) {
                LOGE("GLES initialization failed");
                if (native_window) {
                    ANativeWindow_release(native_window);
                }
                return JNI_FALSE;
            }
        }
    } else {
        renderer = std::make_unique<GLESCRenderer>();
        if (native_window) {
            renderer->setNativeWindow(native_window);
        }
        if (!renderer->initialize(config)) {
            LOGE("GLES initialization failed");
            if (native_window) {
                ANativeWindow_release(native_window);
            }
            return JNI_FALSE;
        }
    }

    setRenderer(std::move(renderer), static_cast<uint64_t>(owner));
    LOGI("Renderer initialized successfully");
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeShutdown(
    JNIEnv* env, jobject thiz, jlong owner
) {
    LOGI("nativeShutdown: owner=%lld", static_cast<long long>(owner));
    std::unique_ptr<RendererBase> renderer;
    {
        std::lock_guard<std::mutex> lock(g_renderer_mutex);
        const auto it = g_renderers.find(static_cast<uint64_t>(owner));
        if (it == g_renderers.end()) {
            LOGW("nativeShutdown: no renderer is registered for owner %lld; ignoring",
                 static_cast<long long>(owner));
            return;
        }
        renderer = std::move(it->second);
        g_renderers.erase(it);
    }
    if (renderer) {
        // Dropping the unique_ptr without calling shutdown() would destroy the
        // C++ object while its device, swapchain and EGL context are still live.
        renderer->shutdown();
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeBeginFrame(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (!renderer) return JNI_FALSE;
    return renderer->beginFrame() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeEndFrame(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (renderer) renderer->endFrame();
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativePresent(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (renderer) renderer->present();
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnSurfaceCreated(
    JNIEnv* env, jobject thiz, jobject surface, jlong owner
) {
    LOGI("nativeOnSurfaceCreated");
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (renderer) {
        ANativeWindow* native_window = ANativeWindow_fromSurface(env, surface);
        if (native_window) {
            renderer->setNativeWindow(native_window);
            renderer->onSurfaceChanged(ANativeWindow_getWidth(native_window), ANativeWindow_getHeight(native_window));
        }
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnSurfaceChanged(
    JNIEnv* env, jobject thiz, jint width, jint height, jlong owner
) {
    LOGI("nativeOnSurfaceChanged: %dx%d", width, height);
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (renderer) {
        renderer->onSurfaceChanged(width, height);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnSurfaceDestroyed(
    JNIEnv* env, jobject thiz, jlong owner
) {
    LOGI("nativeOnSurfaceDestroyed");
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (renderer) {
        renderer->onSurfaceDestroyed();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnMemoryPressure(
    JNIEnv* env, jobject thiz, jint level, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (renderer) {
        renderer->onMemoryPressure(level);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnThermalThrottling(
    JNIEnv* env, jobject thiz, jfloat temperatureRatio, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (renderer) {
        renderer->onThermalThrottling(temperatureRatio);
    }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetBackend(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (!renderer) return static_cast<jint>(RendererBackend::Unknown);
    return static_cast<jint>(renderer->getBackend());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeIsInitialized(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    return renderer && renderer->isInitialized() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeWaitIdle(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (renderer) renderer->waitIdle();
}

// Frame stats
extern "C" JNIEXPORT jlong JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetFrameNumber(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    return renderer ? renderer->getFrameNumber() : 0;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetFrameTimeMs(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    return renderer ? renderer->getFrameTimeMs() : 0.0;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetCpuTimeMs(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    return renderer ? renderer->getCpuTimeMs() : 0.0;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuTimeMs(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    return renderer ? renderer->getGpuTimeMs() : 0.0;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetDrawCalls(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    return renderer ? renderer->getDrawCalls() : 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuMemoryUsed(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    return renderer ? renderer->getGpuMemoryUsed() : 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetCpuMemoryUsed(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    return renderer ? renderer->getCpuMemoryUsed() : 0;
}

// GPU info
extern "C" JNIEXPORT jstring JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuRendererString(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (!renderer) return env->NewStringUTF("Unknown");
    return env->NewStringUTF(renderer->getGpuRendererString().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuVendorString(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (!renderer) return env->NewStringUTF("Unknown");
    return env->NewStringUTF(renderer->getGpuVendorString().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuVersionString(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (!renderer) return env->NewStringUTF("Unknown");
    return env->NewStringUTF(renderer->getGpuVersionString().c_str());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuVendor(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (!renderer) return static_cast<jint>(GPUVendor::Unknown);
    return static_cast<jint>(renderer->getGpuVendor());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuArchitecture(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (!renderer) return static_cast<jint>(GPUArchitecture::Unknown);
    return static_cast<jint>(renderer->getGpuArchitecture());
}

// Feature queries
extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeSupportsFeature(
    JNIEnv* env, jobject thiz, jint feature, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (!renderer) return JNI_FALSE;
    return renderer->supportsFeature(static_cast<RendererFeature>(feature)) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeIsExtensionSupported(
    JNIEnv* env, jobject thiz, jstring extension, jlong owner
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (!renderer) return JNI_FALSE;
    const char* ext = env->GetStringUTFChars(extension, nullptr);
    bool result = renderer->isExtensionSupported(ext);
    env->ReleaseStringUTFChars(extension, ext);
    return result ? JNI_TRUE : JNI_FALSE;
}
// ---------------------------------------------------------------------------
// Resource and draw path
//
// These entry points form the minimum complete path for a real frame:
// create resources -> record commands -> submit -> present. Each one degrades
// safely when the manager it needs is unavailable (for example before a backend
// finished initializing) instead of dereferencing null.
// ---------------------------------------------------------------------------

extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeAreManagersReady(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard) return JNI_FALSE;
    RendererBase* renderer = guard.get();
    const bool ready = renderer->getBufferManager() != nullptr &&
                       renderer->getTextureManager() != nullptr &&
                       renderer->getShaderManager() != nullptr &&
                       renderer->getStateManager() != nullptr;
    return ready ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeCreateBuffer(
    JNIEnv* env, jobject thiz, jlong size, jint usage, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard || size <= 0) return 0;
    BufferManager* buffers = guard.get()->getBufferManager();
    if (!buffers) return 0;
    return static_cast<jlong>(
        buffers->createBuffer(static_cast<uint64_t>(size), static_cast<uint32_t>(usage), 0));
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeDestroyBuffer(
    JNIEnv* env, jobject thiz, jlong handle, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard) return;
    if (BufferManager* buffers = guard.get()->getBufferManager()) {
        buffers->destroyBuffer(static_cast<uint64_t>(handle));
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeUpdateBuffer(
    JNIEnv* env, jobject thiz, jlong handle, jlong offset, jbyteArray data, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard || data == nullptr) return JNI_FALSE;
    BufferManager* buffers = guard.get()->getBufferManager();
    if (!buffers) return JNI_FALSE;

    const jsize length = env->GetArrayLength(data);
    if (length <= 0) return JNI_FALSE;

    // Critical access avoids a full heap copy for the common upload path.
    void* bytes = env->GetPrimitiveArrayCritical(data, nullptr);
    if (bytes == nullptr) {
        return JNI_FALSE;
    }
    const bool written = buffers->updateBuffer(static_cast<uint64_t>(handle),
                                               static_cast<uint64_t>(offset), bytes,
                                               static_cast<uint64_t>(length));
    env->ReleasePrimitiveArrayCritical(data, bytes, 0);
    if (!written) {
        LOGE("buffer %lld rejected a %d byte write at offset %lld", static_cast<long long>(handle),
             length, static_cast<long long>(offset));
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeCreateTexture2D(
    JNIEnv* env, jobject thiz, jint width, jint height, jint format, jint usage, jint mipLevels, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard) return 0;
    TextureManager* textures = guard.get()->getTextureManager();
    if (!textures) return 0;
    return static_cast<jlong>(textures->createTexture2D(static_cast<uint32_t>(width),
                                                          static_cast<uint32_t>(height),
                                                          static_cast<uint32_t>(format),
                                                          static_cast<uint32_t>(usage),
                                                          static_cast<uint32_t>(mipLevels)));
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeUploadTexture(
    JNIEnv* env, jobject thiz, jlong handle, jint mipLevel, jbyteArray data, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard || data == nullptr) return JNI_FALSE;
    TextureManager* textures = guard.get()->getTextureManager();
    if (!textures) return JNI_FALSE;

    const jsize length = env->GetArrayLength(data);
    if (length <= 0) return JNI_FALSE;

    void* bytes = env->GetPrimitiveArrayCritical(data, nullptr);
    if (bytes == nullptr) {
        return JNI_FALSE;
    }
    // A zero extent means "the whole level"; the texture manager resolves it
    // against the stored dimensions.
    const bool uploaded =
        textures->updateTexture(static_cast<uint64_t>(handle), static_cast<uint32_t>(mipLevel), 0, 0, 0, 0,
                                0, 0, 0, bytes, static_cast<uint64_t>(length));
    env->ReleasePrimitiveArrayCritical(data, bytes, 0);
    if (!uploaded) {
        LOGE("texture %lld rejected a %d byte upload to mip %d", static_cast<long long>(handle), length,
             mipLevel);
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeDestroyTexture(
    JNIEnv* env, jobject thiz, jlong handle, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard) return;
    if (TextureManager* textures = guard.get()->getTextureManager()) {
        textures->destroyTexture(static_cast<uint64_t>(handle));
    }
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeCreateShader(
    JNIEnv* env, jobject thiz, jint stage, jstring source, jobjectArray defines, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard || source == nullptr) return 0;
    ShaderManager* shaders = guard.get()->getShaderManager();
    if (!shaders) return 0;

    const char* glsl = env->GetStringUTFChars(source, nullptr);
    if (glsl == nullptr) {
        return 0;
    }
    std::vector<std::string> define_list;
    if (defines != nullptr) {
        const jsize count = env->GetArrayLength(defines);
        for (jsize i = 0; i < count; ++i) {
            jstring element = static_cast<jstring>(env->GetObjectArrayElement(defines, i));
            if (element == nullptr) {
                continue;
            }
            const char* text = env->GetStringUTFChars(element, nullptr);
            if (text != nullptr) {
                define_list.emplace_back(text);
                env->ReleaseStringUTFChars(element, text);
            }
            env->DeleteLocalRef(element);
        }
    }

    const uint64_t handle =
        shaders->createShaderFromGLSL(static_cast<ShaderStage>(stage), glsl, "main", define_list);
    env->ReleaseStringUTFChars(source, glsl);
    return static_cast<jlong>(handle);
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeDestroyShader(
    JNIEnv* env, jobject thiz, jlong handle, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard) return;
    if (ShaderManager* shaders = guard.get()->getShaderManager()) {
        shaders->destroyShader(static_cast<uint64_t>(handle));
    }
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeCreateGraphicsPipeline(
    JNIEnv* env, jobject thiz, jlong vertexShader, jlong fragmentShader, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard) return 0;
    ShaderManager* shaders = guard.get()->getShaderManager();
    if (!shaders) return 0;
    PipelineLayoutDesc layout;
    return static_cast<jlong>(shaders->createGraphicsPipeline(static_cast<uint64_t>(vertexShader),
                                                                static_cast<uint64_t>(fragmentShader),
                                                                layout));
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeDestroyPipeline(
    JNIEnv* env, jobject thiz, jlong handle, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard) return;
    if (ShaderManager* shaders = guard.get()->getShaderManager()) {
        shaders->destroyPipeline(static_cast<uint64_t>(handle));
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeBindPipeline(
    JNIEnv* env, jobject thiz, jlong pipeline, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard) return;
    if (StateManager* state = guard.get()->getStateManager()) {
        state->bindPipeline(static_cast<uint64_t>(pipeline));
        state->applyState();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeBindVertexBuffers(
    JNIEnv* env, jobject thiz, jint first_binding, jlongArray buffers, jintArray offsets, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard || buffers == nullptr) return;
    StateManager* state = guard.get()->getStateManager();
    if (!state) return;

    const jsize buffer_count = env->GetArrayLength(buffers);
    const jsize offset_count = offsets != nullptr ? env->GetArrayLength(offsets) : 0;
    if (buffer_count <= 0) return;

    jlong* buffer_values = env->GetLongArrayElements(buffers, nullptr);
    jint* offset_values = offset_count > 0 ? env->GetIntArrayElements(offsets, nullptr) : nullptr;
    if (buffer_values == nullptr) return;

    for (jsize i = 0; i < buffer_count; ++i) {
        const uint32_t offset =
            offset_values != nullptr && i < offset_count ? static_cast<uint32_t>(offset_values[i]) : 0u;
        const auto binding = static_cast<uint32_t>(first_binding + i);
        if (binding >= kMaxVertexBindings) {
            LOGE("vertex binding %u is out of range (max %u)", binding, kMaxVertexBindings);
            break;
        }
        state->bindVertexBuffer(binding, static_cast<uint64_t>(buffer_values[i]), offset);
    }

    env->ReleaseLongArrayElements(buffers, buffer_values, 0);
    if (offset_values != nullptr) {
        env->ReleaseIntArrayElements(offsets, offset_values, 0);
    }
    state->applyState();
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeBindIndexBuffer(
    JNIEnv* env, jobject thiz, jlong buffer, jint indexType, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard) return;
    if (StateManager* state = guard.get()->getStateManager()) {
        state->bindIndexBuffer(static_cast<uint64_t>(buffer), static_cast<uint32_t>(indexType));
        state->applyState();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeSetViewport(
    JNIEnv* env, jobject thiz, jfloat x, jfloat y, jfloat width, jfloat height, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard) return;
    if (StateManager* state = guard.get()->getStateManager()) {
        state->setViewport(x, y, width, height, 0.0f, 1.0f);
        state->applyState();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeSetScissor(
    JNIEnv* env, jobject thiz, jint x, jint y, jint width, jint height, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard) return;
    if (StateManager* state = guard.get()->getStateManager()) {
        state->setScissor(x, y, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        state->applyState();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeBindFramebuffer(
    JNIEnv* env, jobject thiz, jlong framebuffer, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard) return;
    if (StateManager* state = guard.get()->getStateManager()) {
        state->setFramebuffer(static_cast<uint64_t>(framebuffer));
        state->applyState();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeDraw(
    JNIEnv* env, jobject thiz, jint vertexCount, jint instanceCount, jint firstVertex, jint firstInstance, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard) return;
    if (CommandBuffer* commands = guard.get()->getCommandBuffer()) {
        commands->draw(static_cast<uint32_t>(vertexCount), static_cast<uint32_t>(instanceCount),
                       static_cast<uint32_t>(firstVertex), static_cast<uint32_t>(firstInstance));
    }
    if (Profiler* profiler = guard.get()->getProfiler()) {
        profiler->recordDrawCall();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeDrawIndexed(
    JNIEnv* env, jobject thiz, jint indexCount, jint instanceCount, jint firstIndex, jint vertexOffset,
    jint firstInstance, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard) return;
    if (CommandBuffer* commands = guard.get()->getCommandBuffer()) {
        commands->drawIndexed(static_cast<uint32_t>(indexCount), static_cast<uint32_t>(instanceCount),
                              static_cast<uint32_t>(firstIndex), static_cast<int32_t>(vertexOffset),
                              static_cast<uint32_t>(firstInstance));
    }
    if (Profiler* profiler = guard.get()->getProfiler()) {
        profiler->recordDrawCall();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeResetFrameStats(
    JNIEnv* env, jobject thiz, jlong owner
) {
    RendererGuard guard(owner);
    if (!guard) return;
    if (Profiler* profiler = guard.get()->getProfiler()) {
        profiler->reset();
    }
}


extern "C" JNIEXPORT jstring JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeLastShaderError(
    JNIEnv* env, jobject thiz, jlong owner
) {
    // Reached through the manager rather than through a RendererBase pass-through:
    // the diagnostics belong to the shader manager, and a second accessor for
    // them would be a second thing to keep in step.
    std::string error;
    if (RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner))) {
        if (ShaderManager* shaders = renderer->getShaderManager()) {
            error = shaders->lastShaderError();
        }
    }
    return env->NewStringUTF(error.c_str());
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeShaderCompileStats(
    JNIEnv* env, jobject thiz, jlong owner
) {
    jlong values[5] = {0, 0, 0, 0, 0};
    if (RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner))) {
        if (ShaderManager* shaders = renderer->getShaderManager()) {
            const ShaderManager::ShaderCompileStats stats = shaders->shaderCompileStats();
            values[0] = static_cast<jlong>(stats.compiled);
            values[1] = static_cast<jlong>(stats.memory_hits);
            values[2] = static_cast<jlong>(stats.disk_hits);
            values[3] = static_cast<jlong>(stats.failures);
            values[4] = static_cast<jlong>(stats.total_ms);
        }
    }
    jlongArray result = env->NewLongArray(5);
    if (result != nullptr) {
        env->SetLongArrayRegion(result, 0, 5, values);
    }
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOpenShaderCache(
    JNIEnv* env, jobject thiz, jlong owner, jstring directory
) {
    RendererBase* renderer = renderer_for_owner(static_cast<uint64_t>(owner));
    if (renderer == nullptr || directory == nullptr) {
        return;
    }
    if (ShaderManager* shaders = renderer->getShaderManager()) {
        const char* path = env->GetStringUTFChars(directory, nullptr);
        if (path != nullptr) {
            shaders->openShaderCache(path);
            env->ReleaseStringUTFChars(directory, path);
        }
    }
}



// Maps an include name to a file inside a directory, rooted at a game install.
//
// Minecraft's own scheme is `#include <minecraft:fog.glsl>`, where `minecraft:`
// is a URI scheme, not a directory. A filesystem cannot answer that, so the
// scheme is stripped and the remainder resolved under the root. `../` is
// rejected: a shader reaching outside its own asset root has no legitimate
// reason, and allowing it would let a resource pack read arbitrary files from
// the device.
//
// Returns false for anything it will not serve, which the caller reports as an
// unresolved include rather than compiling a shader with it silently dropped.
static bool resolve_include_from_root(const std::string& root, const std::string& name,
                                      std::string* text) {
    if (root.empty() || name.empty()) {
        return false;
    }
    std::string relative = name;
    const size_t scheme_end = relative.find(':');
    if (scheme_end != std::string::npos && scheme_end + 1 < relative.size()) {
        // Only a scheme-like prefix is stripped, and only once, so a Windows-style
        // drive letter or a nested path is left alone.
        const std::string scheme = relative.substr(0, scheme_end);
        const bool looks_like_scheme =
            !scheme.empty() &&
            (std::isalpha(static_cast<unsigned char>(scheme.front())) != 0);
        if (looks_like_scheme) {
            relative = relative.substr(scheme_end + 1);
        }
    }
    if (relative.empty() || relative.front() == '/' || relative.find("..") != std::string::npos) {
        return false;
    }

    const std::string path = root + "/" + relative;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    text->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeSetShaderIncludeRoot(
    JNIEnv* env, jobject thiz, jstring root
) {
    if (root == nullptr) {
        setShaderIncludeResolver(ShaderIncludeResolver());
        return;
    }
    const char* path = env->GetStringUTFChars(root, nullptr);
    if (path == nullptr) {
        return;
    }
    // Captured by value: the directory belongs to this app and outlives any
    // compile, but copying it means the resolver never reads a freed jstring.
    const std::string root_path(path);
    env->ReleaseStringUTFChars(root, path);
    if (root_path.empty()) {
        setShaderIncludeResolver(ShaderIncludeResolver());
        return;
    }
    setShaderIncludeResolver(
        [root_path](const std::string& name, std::string* text) {
            return resolve_include_from_root(root_path, name, text);
        });
}


} // namespace copper