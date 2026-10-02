#include "android_platform.h"
#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

#define LOG_TAG "CopperOxide-Platform"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

struct AndroidPlatform::Impl {
public:
    JavaVM* jvm = nullptr;
    jobject context = nullptr;
    jclass context_class = nullptr;
    jmethodID get_assets_method = nullptr;
    jobject asset_manager = nullptr;
    AAssetManager* native_asset_manager = nullptr;
    jclass thermal_manager_class = nullptr;
    jmethodID get_thermal_method = nullptr;
    jobject thermal_manager = nullptr;
    std::function<void(float)> thermal_callback;
    std::function<void(int)> memory_callback;
};

AndroidPlatform::AndroidPlatform() : pImpl(std::make_unique<Impl>()) {}
AndroidPlatform::~AndroidPlatform() = default;

bool AndroidPlatform::initialize(JavaVM* jvm, JNIEnv* env, jobject context) {
    pImpl->jvm = jvm;
    pImpl->context = env->NewGlobalRef(context);
    
    // Get AssetManager
    pImpl->context_class = env->GetObjectClass(context);
    pImpl->get_assets_method = env->GetMethodID(pImpl->context_class, "getAssets", "()Landroid/content/res/AssetManager;");
    if (pImpl->get_assets_method) {
        pImpl->asset_manager = env->CallObjectMethod(context, pImpl->get_assets_method);
        pImpl->asset_manager = env->NewGlobalRef(pImpl->asset_manager);
        pImpl->native_asset_manager = AAssetManager_fromJava(env, pImpl->asset_manager);
    }

    // Get ThermalManager
    jclass context_class = env->GetObjectClass(context);
    jmethodID get_system_service = env->GetMethodID(context_class, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring thermal_service = env->NewStringUTF("thermalservice");
    jobject thermal_service_obj = env->CallObjectMethod(context, get_system_service, thermal_service);
    if (thermal_service_obj) {
        pImpl->thermal_manager = env->NewGlobalRef(thermal_service_obj);
    }
    env->DeleteLocalRef(thermal_service);

    LOGI("Android platform initialized");
    return true;
}

void AndroidPlatform::shutdown() {
    if (pImpl->context || pImpl->asset_manager || pImpl->thermal_manager) {
        JNIEnv* env = nullptr;
        bool did_attach = false;
        if (pImpl->jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
            pImpl->jvm->AttachCurrentThread(&env, nullptr);
            did_attach = (env != nullptr);
        }
        if (env) {
            if (pImpl->asset_manager) env->DeleteGlobalRef(pImpl->asset_manager);
            if (pImpl->thermal_manager) env->DeleteGlobalRef(pImpl->thermal_manager);
            if (pImpl->context) env->DeleteGlobalRef(pImpl->context);
        }
        if (did_attach) {
            pImpl->jvm->DetachCurrentThread();
        }
        pImpl->asset_manager = nullptr;
        pImpl->thermal_manager = nullptr;
        pImpl->context = nullptr;
    }
    LOGI("Android platform shutdown");
}

float AndroidPlatform::getThermalThrottlingRatio() {
    return 0.0f; // Would query thermal status
}

void AndroidPlatform::registerThermalCallback(std::function<void(float)> callback) {
    pImpl->thermal_callback = std::move(callback);
}

void AndroidPlatform::registerMemoryPressureCallback(std::function<void(int)> callback) {
    pImpl->memory_callback = std::move(callback);
}

bool AndroidPlatform::loadAsset(const std::string& path, std::vector<uint8_t>& out_data) {
    if (!pImpl->native_asset_manager) return false;
    
    AAsset* asset = AAssetManager_open(pImpl->native_asset_manager, path.c_str(), AASSET_MODE_BUFFER);
    if (!asset) return false;
    
    off_t size = AAsset_getLength(asset);
    out_data.resize(size);
    AAsset_read(asset, out_data.data(), size);
    AAsset_close(asset);
    return true;
}

bool AndroidPlatform::assetExists(const std::string& path) {
    if (!pImpl->native_asset_manager) return false;
    AAsset* asset = AAssetManager_open(pImpl->native_asset_manager, path.c_str(), AASSET_MODE_BUFFER);
    if (asset) {
        AAsset_close(asset);
        return true;
    }
    return false;
}

void* AndroidPlatform::createNativeWindow(ANativeWindow* window) {
    return window;
}

void AndroidPlatform::destroyNativeWindow(void* native_window) {
    ANativeWindow* window = static_cast<ANativeWindow*>(native_window);
    if (window) {
        ANativeWindow_release(window);
    }
}

void AndroidPlatform::setPerformanceHint(int hint_type, int value) {
    // Would call PowerManager.setPerformanceHint
}

float AndroidPlatform::getBatteryLevel() {
    return 1.0f; // Would query battery level
}

bool AndroidPlatform::isCharging() {
    return false; // Would query charging status
}

AndroidPlatform::DisplayInfo AndroidPlatform::getDisplayInfo() {
    DisplayInfo info;
    // Would query display metrics
    return info;
}

} // namespace copper