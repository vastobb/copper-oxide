#include "jni_bridge.h"
#include "vulkan_renderer.h"
#include "gles_renderer.h"
#include "renderer_config.h"
#include "gpu_capabilities.h"
#include "buffer_manager.h"
#include "texture_manager.h"
#include "shader_manager.h"
#include "state_manager.h"
#include "command_buffer.h"

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <array>
#include <mutex>
#include <string>
#include <memory>
#include <vector>

#define LOG_TAG "CopperOxide-JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

static std::mutex g_renderer_mutex;
static std::unique_ptr<RendererBase> g_renderer;
// Which CopperOxideRenderer instance owns g_renderer.
//
// There is one process-global renderer slot, but there can be more than one
// CopperOxideRenderer object in a process - the activity and an instrumentation
// test both create one. Without ownership, the second initialize() replaced the
// first instance's renderer and the first instance's shutdown() destroyed it out
// from under its own running render loop, which is how frame statistics came
// back as all zeroes while the native side was visibly rendering.
static uint64_t g_renderer_owner = 0;
static JavaVM* g_jvm = nullptr;

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_jvm = vm;
    LOGI("JNI_OnLoad");
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
    std::unique_ptr<RendererBase> renderer;
    {
        std::lock_guard<std::mutex> lock(g_renderer_mutex);
        renderer = std::move(g_renderer);
        g_renderer_owner = 0;
    }
    if (renderer) {
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

RendererBase* getRenderer() {
    // Kept for source compatibility with existing call sites. Callers that
    // then use the returned pointer for more than one operation should
    // prefer RendererGuard, which keeps the renderer alive for the call.
    std::lock_guard<std::mutex> lock(g_renderer_mutex);
    return g_renderer.get();
}

void setRenderer(std::unique_ptr<RendererBase> renderer, uint64_t owner) {
    std::unique_ptr<RendererBase> displaced;
    {
        std::lock_guard<std::mutex> lock(g_renderer_mutex);
        // Replacing the slot must not leak or leave a previous owner's renderer
        // alive with nothing pointing at it, so the old one is taken out here
        // and shut down outside the lock.
        displaced = std::move(g_renderer);
        g_renderer = std::move(renderer);
        g_renderer_owner = owner;
    }
    if (displaced) {
        LOGE("a renderer was already installed; shutting the previous one down");
        displaced->shutdown();
    }
}


namespace {

// StateManager stores at most this many vertex binding slots; a higher binding
// index would be silently dropped by the array it writes into.
constexpr uint32_t kMaxVertexBindings = 16;

// Holds the renderer alive for the duration of one JNI call. Shutdown() can
// otherwise destroy the renderer between getRenderer() and the caller's
// first use, which is a use-after-free.
class RendererGuard {
public:
    RendererGuard() {
        g_renderer_mutex.lock();
        renderer_ = g_renderer.get();
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
        // Only the instance that installed the renderer may tear it down.
        // Another CopperOxideRenderer in the same process - the activity, or an
        // instrumentation test - owns it now, and destroying it here would pull
        // the device and the surface out from under that owner's render thread.
        if (g_renderer_owner != static_cast<uint64_t>(owner)) {
            LOGW("nativeShutdown from owner %lld, but owner %llu holds the renderer; ignoring",
                 static_cast<long long>(owner), static_cast<unsigned long long>(g_renderer_owner));
            return;
        }
        renderer = std::move(g_renderer);
        g_renderer_owner = 0;
    }
    if (renderer) {
        // Dropping the unique_ptr without calling shutdown() would destroy the
        // C++ object while its device, swapchain and EGL context are still live.
        renderer->shutdown();
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeBeginFrame(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    if (!renderer) return JNI_FALSE;
    return renderer->beginFrame() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeEndFrame(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    if (renderer) renderer->endFrame();
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativePresent(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    if (renderer) renderer->present();
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnSurfaceCreated(
    JNIEnv* env, jobject thiz, jobject surface
) {
    LOGI("nativeOnSurfaceCreated");
    RendererBase* renderer = getRenderer();
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
    JNIEnv* env, jobject thiz, jint width, jint height
) {
    LOGI("nativeOnSurfaceChanged: %dx%d", width, height);
    RendererBase* renderer = getRenderer();
    if (renderer) {
        renderer->onSurfaceChanged(width, height);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnSurfaceDestroyed(
    JNIEnv* env, jobject thiz
) {
    LOGI("nativeOnSurfaceDestroyed");
    RendererBase* renderer = getRenderer();
    if (renderer) {
        renderer->onSurfaceDestroyed();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnMemoryPressure(
    JNIEnv* env, jobject thiz, jint level
) {
    RendererBase* renderer = getRenderer();
    if (renderer) {
        renderer->onMemoryPressure(level);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnThermalThrottling(
    JNIEnv* env, jobject thiz, jfloat temperatureRatio
) {
    RendererBase* renderer = getRenderer();
    if (renderer) {
        renderer->onThermalThrottling(temperatureRatio);
    }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetBackend(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    if (!renderer) return static_cast<jint>(RendererBackend::Unknown);
    return static_cast<jint>(renderer->getBackend());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeIsInitialized(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    return renderer && renderer->isInitialized() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeWaitIdle(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    if (renderer) renderer->waitIdle();
}

// Frame stats
extern "C" JNIEXPORT jlong JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetFrameNumber(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    return renderer ? renderer->getFrameNumber() : 0;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetFrameTimeMs(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    return renderer ? renderer->getFrameTimeMs() : 0.0;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetCpuTimeMs(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    return renderer ? renderer->getCpuTimeMs() : 0.0;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuTimeMs(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    return renderer ? renderer->getGpuTimeMs() : 0.0;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetDrawCalls(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    return renderer ? renderer->getDrawCalls() : 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuMemoryUsed(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    return renderer ? renderer->getGpuMemoryUsed() : 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetCpuMemoryUsed(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    return renderer ? renderer->getCpuMemoryUsed() : 0;
}

// GPU info
extern "C" JNIEXPORT jstring JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuRendererString(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    if (!renderer) return env->NewStringUTF("Unknown");
    return env->NewStringUTF(renderer->getGpuRendererString().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuVendorString(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    if (!renderer) return env->NewStringUTF("Unknown");
    return env->NewStringUTF(renderer->getGpuVendorString().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuVersionString(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    if (!renderer) return env->NewStringUTF("Unknown");
    return env->NewStringUTF(renderer->getGpuVersionString().c_str());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuVendor(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    if (!renderer) return static_cast<jint>(GPUVendor::Unknown);
    return static_cast<jint>(renderer->getGpuVendor());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuArchitecture(
    JNIEnv* env, jobject thiz
) {
    RendererBase* renderer = getRenderer();
    if (!renderer) return static_cast<jint>(GPUArchitecture::Unknown);
    return static_cast<jint>(renderer->getGpuArchitecture());
}

// Feature queries
extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeSupportsFeature(
    JNIEnv* env, jobject thiz, jint feature
) {
    RendererBase* renderer = getRenderer();
    if (!renderer) return JNI_FALSE;
    return renderer->supportsFeature(static_cast<RendererFeature>(feature)) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeIsExtensionSupported(
    JNIEnv* env, jobject thiz, jstring extension
) {
    RendererBase* renderer = getRenderer();
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
    JNIEnv* env, jobject thiz
) {
    RendererGuard guard;
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
    JNIEnv* env, jobject thiz, jlong size, jint usage
) {
    RendererGuard guard;
    if (!guard || size <= 0) return 0;
    BufferManager* buffers = guard.get()->getBufferManager();
    if (!buffers) return 0;
    return static_cast<jlong>(
        buffers->createBuffer(static_cast<uint64_t>(size), static_cast<uint32_t>(usage), 0));
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeDestroyBuffer(
    JNIEnv* env, jobject thiz, jlong handle
) {
    RendererGuard guard;
    if (!guard) return;
    if (BufferManager* buffers = guard.get()->getBufferManager()) {
        buffers->destroyBuffer(static_cast<uint64_t>(handle));
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeUpdateBuffer(
    JNIEnv* env, jobject thiz, jlong handle, jlong offset, jbyteArray data
) {
    RendererGuard guard;
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
    JNIEnv* env, jobject thiz, jint width, jint height, jint format, jint usage, jint mipLevels
) {
    RendererGuard guard;
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
    JNIEnv* env, jobject thiz, jlong handle, jint mipLevel, jbyteArray data
) {
    RendererGuard guard;
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
    JNIEnv* env, jobject thiz, jlong handle
) {
    RendererGuard guard;
    if (!guard) return;
    if (TextureManager* textures = guard.get()->getTextureManager()) {
        textures->destroyTexture(static_cast<uint64_t>(handle));
    }
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeCreateShader(
    JNIEnv* env, jobject thiz, jint stage, jstring source, jobjectArray defines
) {
    RendererGuard guard;
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
    JNIEnv* env, jobject thiz, jlong handle
) {
    RendererGuard guard;
    if (!guard) return;
    if (ShaderManager* shaders = guard.get()->getShaderManager()) {
        shaders->destroyShader(static_cast<uint64_t>(handle));
    }
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeCreateGraphicsPipeline(
    JNIEnv* env, jobject thiz, jlong vertexShader, jlong fragmentShader
) {
    RendererGuard guard;
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
    JNIEnv* env, jobject thiz, jlong handle
) {
    RendererGuard guard;
    if (!guard) return;
    if (ShaderManager* shaders = guard.get()->getShaderManager()) {
        shaders->destroyPipeline(static_cast<uint64_t>(handle));
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeBindPipeline(
    JNIEnv* env, jobject thiz, jlong pipeline
) {
    RendererGuard guard;
    if (!guard) return;
    if (StateManager* state = guard.get()->getStateManager()) {
        state->bindPipeline(static_cast<uint64_t>(pipeline));
        state->applyState();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeBindVertexBuffers(
    JNIEnv* env, jobject thiz, jint first_binding, jlongArray buffers, jintArray offsets
) {
    RendererGuard guard;
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
    JNIEnv* env, jobject thiz, jlong buffer, jint indexType
) {
    RendererGuard guard;
    if (!guard) return;
    if (StateManager* state = guard.get()->getStateManager()) {
        state->bindIndexBuffer(static_cast<uint64_t>(buffer), static_cast<uint32_t>(indexType));
        state->applyState();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeSetViewport(
    JNIEnv* env, jobject thiz, jfloat x, jfloat y, jfloat width, jfloat height
) {
    RendererGuard guard;
    if (!guard) return;
    if (StateManager* state = guard.get()->getStateManager()) {
        state->setViewport(x, y, width, height, 0.0f, 1.0f);
        state->applyState();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeSetScissor(
    JNIEnv* env, jobject thiz, jint x, jint y, jint width, jint height
) {
    RendererGuard guard;
    if (!guard) return;
    if (StateManager* state = guard.get()->getStateManager()) {
        state->setScissor(x, y, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        state->applyState();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeBindFramebuffer(
    JNIEnv* env, jobject thiz, jlong framebuffer
) {
    RendererGuard guard;
    if (!guard) return;
    if (StateManager* state = guard.get()->getStateManager()) {
        state->setFramebuffer(static_cast<uint64_t>(framebuffer));
        state->applyState();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeDraw(
    JNIEnv* env, jobject thiz, jint vertexCount, jint instanceCount, jint firstVertex, jint firstInstance
) {
    RendererGuard guard;
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
    jint firstInstance
) {
    RendererGuard guard;
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
    JNIEnv* env, jobject thiz
) {
    RendererGuard guard;
    if (!guard) return;
    if (Profiler* profiler = guard.get()->getProfiler()) {
        profiler->reset();
    }
}

} // namespace copper