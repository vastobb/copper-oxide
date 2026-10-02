#include "jni_bridge.h"
#include "vulkan_renderer.h"
#include "gles_renderer.h"
#include "renderer_config.h"
#include "gpu_capabilities.h"

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <mutex>
#include <memory>

#define LOG_TAG "CopperOxide-JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

static std::mutex g_renderer_mutex;
static std::unique_ptr<RendererBase> g_renderer;
static JavaVM* g_jvm = nullptr;

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_jvm = vm;
    LOGI("JNI_OnLoad");
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
    std::lock_guard<std::mutex> lock(g_renderer_mutex);
    g_renderer.reset();
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
    std::lock_guard<std::mutex> lock(g_renderer_mutex);
    return g_renderer.get();
}

void setRenderer(std::unique_ptr<RendererBase> renderer) {
    std::lock_guard<std::mutex> lock(g_renderer_mutex);
    g_renderer = std::move(renderer);
}

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
    jfloat thermalThrottleThreshold
) {
    LOGI("nativeInitialize: backend=%d", preferredBackend);

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

    setRenderer(std::move(renderer));
    LOGI("Renderer initialized successfully");
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeShutdown(
    JNIEnv* env, jobject thiz
) {
    LOGI("nativeShutdown");
    setRenderer(nullptr);
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

} // namespace copper