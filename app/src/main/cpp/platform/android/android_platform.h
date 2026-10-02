#pragma once

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <memory>
#include <mutex>
#include <string>

namespace copper {

class AndroidPlatform {
public:
    static AndroidPlatform& instance() {
        static AndroidPlatform platform;
        return platform;
    }

    // JNI environment management
    JNIEnv* get_jni_env();
    JavaVM* get_java_vm() const { return java_vm_; }
    void set_java_vm(JavaVM* vm) { java_vm_ = vm; }

    // Activity & context
    jobject get_activity() const { return activity_; }
    void set_activity(jobject activity) { activity_ = activity; }

    jobject get_context() const { return context_; }
    void set_context(jobject context) { context_ = context; }

    // Asset manager
    AAssetManager* get_asset_manager() const { return asset_manager_; }
    void set_asset_manager(AAssetManager* mgr) { asset_manager_ = mgr; }

    // Native window
    ANativeWindow* get_native_window() const { return native_window_; }
    void set_native_window(ANativeWindow* window) { native_window_ = window; }

    // Screen info
    int get_screen_width() const { return screen_width_; }
    int get_screen_height() const { return screen_height_; }
    float get_screen_density() const { return screen_density_; }
    void set_screen_info(int width, int height, float density) {
        screen_width_ = width;
        screen_height_ = height;
        screen_density_ = density;
    }

    // SDK version
    int get_sdk_version() const { return sdk_version_; }
    void set_sdk_version(int version) { sdk_version_ = version; }

    // ABI
    std::string get_abi() const { return abi_; }
    void set_abi(const std::string& abi) { abi_ = abi; }

    // Memory info
    int64_t get_total_memory() const { return total_memory_; }
    int64_t get_available_memory() const { return available_memory_; }
    void update_memory_info();

    // Thermal state
    float get_thermal_level() const { return thermal_level_; }
    void set_thermal_level(float level) { thermal_level_ = level; }

    // Battery level
    float get_battery_level() const { return battery_level_; }
    void set_battery_level(float level) { battery_level_ = level; }

    // Logging
    static void log(int priority, const char* tag, const char* fmt, ...);
    static void log_v(int priority, const char* tag, const char* fmt, va_list args);

    // Thread attachment
    bool attach_current_thread(JNIEnv** env);
    void detach_current_thread();

    // Class loading
    jclass find_class(const char* name);
    jmethodID get_method_id(jclass clazz, const char* name, const char* sig);
    jfieldID get_field_id(jclass clazz, const char* name, const char* sig);

private:
    AndroidPlatform() = default;
    ~AndroidPlatform() = default;

    JavaVM* java_vm_ = nullptr;
    jobject activity_ = nullptr;
    jobject context_ = nullptr;
    AAssetManager* asset_manager_ = nullptr;
    ANativeWindow* native_window_ = nullptr;
    int screen_width_ = 0;
    int screen_height_ = 0;
    float screen_density_ = 1.0f;
    int sdk_version_ = 0;
    std::string abi_;
    int64_t total_memory_ = 0;
    int64_t available_memory_ = 0;
    float thermal_level_ = 0.0f;
    float battery_level_ = 1.0f;
};

#define COPPER_LOG_TAG "CopperOxide"
#define COPPER_LOGV(...) copper::AndroidPlatform::log(ANDROID_LOG_VERBOSE, COPPER_LOG_TAG, __VA_ARGS__)
#define COPPER_LOGD(...) copper::AndroidPlatform::log(ANDROID_LOG_DEBUG, COPPER_LOG_TAG, __VA_ARGS__)
#define COPPER_LOGI(...) copper::AndroidPlatform::log(ANDROID_LOG_INFO, COPPER_LOG_TAG, __VA_ARGS__)
#define COPPER_LOGW(...) copper::AndroidPlatform::log(ANDROID_LOG_WARN, COPPER_LOG_TAG, __VA_ARGS__)
#define COPPER_LOGE(...) copper::AndroidPlatform::log(ANDROID_LOG_ERROR, COPPER_LOG_TAG, __VA_ARGS__)

// JNI helper macros
#define COPPER_JNI_METHOD(return_type, method_name) \
    extern "C" JNIEXPORT return_type JNICALL \
    Java_com_oxide_mc_copperoxide_renderer_CopperOxideRenderer_##method_name

#define COPPER_JNI_METHOD_GUI(return_type, method_name) \
    extern "C" JNIEXPORT return_type JNICALL \
    Java_com_oxide_mc_copperoxide_gui_CopperOxideGUI_##method_name

// Exception handling
inline void throw_java_exception(JNIEnv* env, const char* class_name, const char* message) {
    jclass clazz = env->FindClass(class_name);
    if (clazz) {
        env->ThrowNew(clazz, message);
        env->DeleteLocalRef(clazz);
    }
}

inline void throw_runtime_exception(JNIEnv* env, const char* message) {
    throw_java_exception(env, "java/lang/RuntimeException", message);
}

inline void throw_illegal_state_exception(JNIEnv* env, const char* message) {
    throw_java_exception(env, "java/lang/IllegalStateException", message);
}

inline void throw_illegal_argument_exception(JNIEnv* env, const char* message) {
    throw_java_exception(env, "java/lang/IllegalArgumentException", message);
}

inline void throw_out_of_memory_exception(JNIEnv* env, const char* message) {
    throw_java_exception(env, "java/lang/OutOfMemoryError", message);
}

} // namespace copper