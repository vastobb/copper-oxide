#include "jni_bridge.h"
#include "renderer_base.h"
#include "vulkan/vulkan_renderer.h"
#include "gles/gles_renderer.h"
#include "android_platform.h"
#include <mutex>
#include <memory>

namespace copper {

static std::unique_ptr<RendererBase> g_renderer;
static std::mutex g_renderer_mutex;
static RendererBackend g_current_backend = RendererBackend::Auto;
static bool g_renderer_initialized = false;

extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeInitialize(
    JNIEnv* env, jobject thiz,
    jobject surface,
    jint preferred_backend,
    jboolean enable_validation,
    jboolean enable_debug_markers,
    jboolean enable_profiling,
    jboolean enable_multithreaded,
    jboolean enable_async_shader_compilation,
    jboolean enable_async_resource_loading,
    jboolean enable_resource_pooling,
    jboolean enable_command_buffer_reuse,
    jboolean enable_state_caching,
    jboolean enable_draw_call_batching,
    jboolean enable_pipeline_caching,
    jboolean enable_descriptor_caching,
    jboolean enable_texture_streaming,
    jboolean enable_texture_compression,
    jboolean enable_mipmap_generation,
    jint max_frames_in_flight,
    jint max_command_buffers_per_frame,
    jint max_descriptor_sets,
    jint max_push_constants_size,
    jint texture_cache_size_mb,
    jint shader_cache_size_mb,
    jint buffer_pool_size_mb,
    jint frame_timeout_ms,
    jboolean vsync_enabled,
    jint target_fps,
    jboolean low_latency_mode,
    jboolean battery_saver_mode,
    jboolean thermal_throttling_aware,
    jfloat thermal_throttle_threshold) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (g_renderer_initialized) {
        COPPER_LOGW("Renderer already initialized");
        return JNI_TRUE;
    }

    // Get native window from surface
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        COPPER_LOGE("Failed to get native window from surface");
        return JNI_FALSE;
    }

    // Create renderer config
    RendererConfig config;
    config.preferred_backend = static_cast<RendererBackend>(preferred_backend);
    config.enable_validation = enable_validation;
    config.enable_debug_markers = enable_debug_markers;
    config.enable_profiling = enable_profiling;
    config.enable_multithreaded_rendering = enable_multithreaded;
    config.enable_async_shader_compilation = enable_async_shader_compilation;
    config.enable_async_resource_loading = enable_async_resource_loading;
    config.enable_resource_pooling = enable_resource_pooling;
    config.enable_command_buffer_reuse = enable_command_buffer_reuse;
    config.enable_state_caching = enable_state_caching;
    config.enable_draw_call_batching = enable_draw_call_batching;
    config.enable_pipeline_caching = enable_pipeline_caching;
    config.enable_descriptor_caching = enable_descriptor_caching;
    config.enable_texture_streaming = enable_texture_streaming;
    config.enable_texture_compression = enable_texture_compression;
    config.enable_mipmap_generation = enable_mipmap_generation;
    config.max_frames_in_flight = max_frames_in_flight;
    config.max_command_buffers_per_frame = max_command_buffers_per_frame;
    config.max_descriptor_sets = max_descriptor_sets;
    config.max_push_constants_size = max_push_constants_size;
    config.texture_cache_size_mb = texture_cache_size_mb;
    config.shader_cache_size_mb = shader_cache_size_mb;
    config.buffer_pool_size_mb = buffer_pool_size_mb;
    config.frame_timeout_ms = frame_timeout_ms;
    config.vsync_enabled = vsync_enabled;
    config.target_fps = target_fps;
    config.low_latency_mode = low_latency_mode;
    config.battery_saver_mode = battery_saver_mode;
    config.thermal_throttling_aware = thermal_throttling_aware;
    config.thermal_throttle_threshold = thermal_throttle_threshold;

    // Set up Android platform
    AndroidPlatform& platform = AndroidPlatform::instance();
    platform.set_native_window(window);

    // Create renderer based on preferred backend
    std::unique_ptr<RendererBase> renderer;
    RendererBackend backend_to_use = config.preferred_backend;

    if (backend_to_use == RendererBackend::Auto) {
        // Auto-detect: prefer Vulkan if available
        // We'll try Vulkan first, fall back to GLES
        renderer = RendererRegistry::instance().create_renderer(RendererBackend::Vulkan);
        if (renderer) {
            if (!renderer->initialize(config, window)) {
                COPPER_LOGW("Vulkan initialization failed, falling back to OpenGL ES");
                renderer.reset();
            } else {
                backend_to_use = RendererBackend::Vulkan;
            }
        }

        if (!renderer) {
            renderer = RendererRegistry::instance().create_renderer(RendererBackend::OpenGLES);
            if (renderer && renderer->initialize(config, window)) {
                backend_to_use = RendererBackend::OpenGLES;
            }
        }
    } else {
        renderer = RendererRegistry::instance().create_renderer(backend_to_use);
        if (renderer && !renderer->initialize(config, window)) {
            COPPER_LOGE("Failed to initialize requested renderer backend");
            renderer.reset();
        }
    }

    if (!renderer) {
        COPPER_LOGE("Failed to create any renderer");
        ANativeWindow_release(window);
        return JNI_FALSE;
    }

    g_renderer = std::move(renderer);
    g_current_backend = backend_to_use;
    g_renderer_initialized = true;

    COPPER_LOGI("Renderer initialized: backend=%d", static_cast<int>(g_current_backend));
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeShutdown(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return;
    }

    g_renderer->shutdown();
    g_renderer.reset();
    g_renderer_initialized = false;
    g_current_backend = RendererBackend::Auto;

    AndroidPlatform::instance().set_native_window(nullptr);

    COPPER_LOGI("Renderer shutdown");
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeBeginFrame(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return JNI_FALSE;
    }

    return g_renderer->begin_frame() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeEndFrame(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return;
    }

    g_renderer->end_frame();
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativePresent(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return;
    }

    g_renderer->present();
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnSurfaceCreated(
    JNIEnv* env, jobject thiz, jobject surface) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return;
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window) {
        g_renderer->on_surface_created(window);
        AndroidPlatform::instance().set_native_window(window);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnSurfaceChanged(
    JNIEnv* env, jobject thiz, jint width, jint height) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return;
    }

    g_renderer->on_surface_changed(width, height);
    AndroidPlatform::instance().set_screen_info(width, height, 1.0f);
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnSurfaceDestroyed(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return;
    }

    g_renderer->on_surface_destroyed();
    AndroidPlatform::instance().set_native_window(nullptr);
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnMemoryPressure(
    JNIEnv* env, jobject thiz, jint level) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return;
    }

    g_renderer->on_memory_pressure(level);
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnThermalThrottling(
    JNIEnv* env, jobject thiz, jfloat temperature_ratio) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return;
    }

    g_renderer->on_thermal_throttling(temperature_ratio);
    AndroidPlatform::instance().set_thermal_level(temperature_ratio);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetBackend(
    JNIEnv* env, jobject thiz) {

    return static_cast<jint>(g_current_backend);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeIsInitialized(
    JNIEnv* env, jobject thiz) {

    return g_renderer_initialized ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeWaitIdle(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return;
    }

    g_renderer->wait_idle();
}

// Frame stats
extern "C" JNIEXPORT jlong JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetFrameNumber(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return 0;
    }

    return static_cast<jlong>(g_renderer->get_frame_stats().frame_number);
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetFrameTimeMs(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return 0.0;
    }

    return g_renderer->get_frame_stats().frame_time_ms;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetCpuTimeMs(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return 0.0;
    }

    return g_renderer->get_frame_stats().cpu_time_ms;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuTimeMs(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return 0.0;
    }

    return g_renderer->get_frame_stats().gpu_time_ms;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetDrawCalls(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return 0;
    }

    return static_cast<jint>(g_renderer->get_frame_stats().draw_calls);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuMemoryUsed(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return 0;
    }

    return static_cast<jlong>(g_renderer->get_frame_stats().gpu_memory_used);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetCpuMemoryUsed(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return 0;
    }

    return static_cast<jlong>(g_renderer->get_frame_stats().cpu_memory_used);
}

extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeResetFrameStats(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return;
    }

    g_renderer->reset_frame_stats();
}

// GPU info
extern "C" JNIEXPORT jstring JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuRendererString(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return env->NewStringUTF("Unknown");
    }

    return env->NewStringUTF(g_renderer->get_gpu_info().renderer_string.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuVendorString(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return env->NewStringUTF("Unknown");
    }

    return env->NewStringUTF(g_renderer->get_gpu_info().vendor_string.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuVersionString(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return env->NewStringUTF("Unknown");
    }

    return env->NewStringUTF(g_renderer->get_gpu_info().version_string.c_str());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuVendor(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return 0; // Unknown
    }

    return static_cast<jint>(g_renderer->get_gpu_info().vendor);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuArchitecture(
    JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return 0; // Unknown
    }

    return static_cast<jint>(g_renderer->get_gpu_info().architecture);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeSupportsFeature(
    JNIEnv* env, jobject thiz, jint feature) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer) {
        return JNI_FALSE;
    }

    return g_renderer->supports_feature(static_cast<RendererFeature>(feature)) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeIsExtensionSupported(
    JNIEnv* env, jobject thiz, jstring extension) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer || !extension) {
        return JNI_FALSE;
    }

    const char* ext_str = env->GetStringUTFChars(extension, nullptr);
    bool result = g_renderer->is_extension_supported(ext_str);
    env->ReleaseStringUTFChars(extension, ext_str);

    return result ? JNI_TRUE : JNI_FALSE;
}

// Set debug name
extern "C" JNIEXPORT void JNICALL
Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeSetDebugName(
    JNIEnv* env, jobject thiz, jlong handle, jstring name) {

    std::lock_guard<std::mutex> lock(g_renderer_mutex);

    if (!g_renderer_initialized || !g_renderer || !name) {
        return;
    }

    const char* name_str = env->GetStringUTFChars(name, nullptr);
    g_renderer->set_debug_name(static_cast<uint64_t>(handle), name_str);
    env->ReleaseStringUTFChars(name, name_str);
}

} // namespace copper