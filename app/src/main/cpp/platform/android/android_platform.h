#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <vector>
#include <jni.h>
#include <android/native_window.h>

namespace copper {

class AndroidPlatform {
public:
    AndroidPlatform();
    ~AndroidPlatform();

    AndroidPlatform(const AndroidPlatform&) = delete;
    AndroidPlatform& operator=(const AndroidPlatform&) = delete;
    AndroidPlatform(AndroidPlatform&&) noexcept = default;
    AndroidPlatform& operator=(AndroidPlatform&&) noexcept = default;

    bool initialize(JavaVM* jvm, JNIEnv* env, jobject context);
    void shutdown();

    // Thermal management
    float getThermalThrottlingRatio();
    void registerThermalCallback(std::function<void(float)> callback);

    // Memory pressure
    void registerMemoryPressureCallback(std::function<void(int)> callback);

    // Asset management
    bool loadAsset(const std::string& path, std::vector<uint8_t>& out_data);
    bool assetExists(const std::string& path);

    // Window/surface
    void* createNativeWindow(ANativeWindow* window);
    void destroyNativeWindow(void* native_window);

    // Performance hints
    void setPerformanceHint(int hint_type, int value);

    // Battery status
    float getBatteryLevel();
    bool isCharging();

    // Display info
    struct DisplayInfo {
        uint32_t width = 0;
        uint32_t height = 0;
        float density = 1.0f;
        float refresh_rate = 60.0f;
    };
    DisplayInfo getDisplayInfo();

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace copper