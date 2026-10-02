#pragma once

#include <jni.h>

namespace copper {

// JNI Bridge declarations
extern "C" {
    // Renderer lifecycle
    JNIEXPORT jboolean JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeInitialize(
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
        jfloat thermal_throttle_threshold);

    JNIEXPORT void JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeShutdown(
        JNIEnv* env, jobject thiz);

    // Frame rendering
    JNIEXPORT jboolean JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeBeginFrame(
        JNIEnv* env, jobject thiz);

    JNIEXPORT void JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeEndFrame(
        JNIEnv* env, jobject thiz);

    JNIEXPORT void JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativePresent(
        JNIEnv* env, jobject thiz);

    // Surface lifecycle
    JNIEXPORT void JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnSurfaceCreated(
        JNIEnv* env, jobject thiz, jobject surface);

    JNIEXPORT void JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnSurfaceChanged(
        JNIEnv* env, jobject thiz, jint width, jint height);

    JNIEXPORT void JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnSurfaceDestroyed(
        JNIEnv* env, jobject thiz);

    // System events
    JNIEXPORT void JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnMemoryPressure(
        JNIEnv* env, jobject thiz, jint level);

    JNIEXPORT void JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeOnThermalThrottling(
        JNIEnv* env, jobject thiz, jfloat temperature_ratio);

    // Info queries
    JNIEXPORT jint JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetBackend(
        JNIEnv* env, jobject thiz);

    JNIEXPORT jboolean JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeIsInitialized(
        JNIEnv* env, jobject thiz);

    JNIEXPORT void JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeWaitIdle(
        JNIEnv* env, jobject thiz);

    // Frame stats
    JNIEXPORT jlong JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetFrameNumber(
        JNIEnv* env, jobject thiz);

    JNIEXPORT jdouble JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetFrameTimeMs(
        JNIEnv* env, jobject thiz);

    JNIEXPORT jdouble JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetCpuTimeMs(
        JNIEnv* env, jobject thiz);

    JNIEXPORT jdouble JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuTimeMs(
        JNIEnv* env, jobject thiz);

    JNIEXPORT jint JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetDrawCalls(
        JNIEnv* env, jobject thiz);

    JNIEXPORT jlong JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuMemoryUsed(
        JNIEnv* env, jobject thiz);

    JNIEXPORT jlong JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetCpuMemoryUsed(
        JNIEnv* env, jobject thiz);

    JNIEXPORT void JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeResetFrameStats(
        JNIEnv* env, jobject thiz);

    // GPU info
    JNIEXPORT jstring JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuRendererString(
        JNIEnv* env, jobject thiz);

    JNIEXPORT jstring JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuVendorString(
        JNIEnv* env, jobject thiz);

    JNIEXPORT jstring JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuVersionString(
        JNIEnv* env, jobject thiz);

    JNIEXPORT jint JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuVendor(
        JNIEnv* env, jobject thiz);

    JNIEXPORT jint JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeGetGpuArchitecture(
        JNIEnv* env, jobject thiz);

    // Feature queries
    JNIEXPORT jboolean JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeSupportsFeature(
        JNIEnv* env, jobject thiz, jint feature);

    JNIEXPORT jboolean JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeIsExtensionSupported(
        JNIEnv* env, jobject thiz, jstring extension);

    // Debug
    JNIEXPORT void JNICALL Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_nativeSetDebugName(
        JNIEnv* env, jobject thiz, jlong handle, jstring name);
}

} // namespace copper