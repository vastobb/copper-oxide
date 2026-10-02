#include "android_platform.h"
#include <android/asset_manager_jni.h>
#include <android/configuration.h>
#include <android/looper.h>
#include <sys/system_properties.h>

namespace copper {

JNIEnv* AndroidPlatform::get_jni_env() {
    JNIEnv* env = nullptr;
    if (java_vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (java_vm_->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            return nullptr;
        }
    }
    return env;
}

bool AndroidPlatform::attach_current_thread(JNIEnv** env) {
    if (!java_vm_) return false;
    jint result = java_vm_->AttachCurrentThread(env, nullptr);
    return result == JNI_OK;
}

void AndroidPlatform::detach_current_thread() {
    if (java_vm_) {
        java_vm_->DetachCurrentThread();
    }
}

jclass AndroidPlatform::find_class(const char* name) {
    JNIEnv* env = get_jni_env();
    if (!env) return nullptr;
    return env->FindClass(name);
}

jmethodID AndroidPlatform::get_method_id(jclass clazz, const char* name, const char* sig) {
    JNIEnv* env = get_jni_env();
    if (!env || !clazz) return nullptr;
    return env->GetMethodID(clazz, name, sig);
}

jfieldID AndroidPlatform::get_field_id(jclass clazz, const char* name, const char* sig) {
    JNIEnv* env = get_jni_env();
    if (!env || !clazz) return nullptr;
    return env->GetFieldID(clazz, name, sig);
}

void AndroidPlatform::update_memory_info() {
    JNIEnv* env = get_jni_env();
    if (!env || !activity_) return;

    jclass activity_class = env->GetObjectClass(activity_);
    jmethodID get_system_service = env->GetMethodID(activity_class, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring mem_service = env->NewStringUTF("activity");
    jobject activity_manager = env->CallObjectMethod(activity_, get_system_service, mem_service);
    env->DeleteLocalRef(mem_service);

    if (activity_manager) {
        jclass am_class = env->GetObjectClass(activity_manager);
        jmethodID get_memory_info = env->GetMethodID(am_class, "getMemoryInfo", "(Landroid/app/ActivityManager$MemoryInfo;)V");
        jclass mem_info_class = env->FindClass("android/app/ActivityManager$MemoryInfo");
        jobject mem_info = env->AllocObject(mem_info_class);

        env->CallVoidMethod(activity_manager, get_memory_info, mem_info);

        jfieldID total_mem_field = env->GetFieldID(mem_info_class, "totalMem", "J");
        jfieldID avail_mem_field = env->GetFieldID(mem_info_class, "availMem", "J");

        total_memory_ = env->GetLongField(mem_info, total_mem_field);
        available_memory_ = env->GetLongField(mem_info, avail_mem_field);

        env->DeleteLocalRef(mem_info);
        env->DeleteLocalRef(am_class);
        env->DeleteLocalRef(activity_manager);
    }
    env->DeleteLocalRef(activity_class);
}

void AndroidPlatform::log(int priority, const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v(priority, tag, fmt, args);
    va_end(args);
}

void AndroidPlatform::log_v(int priority, const char* tag, const char* fmt, va_list args) {
    __android_log_vprint(priority, tag, fmt, args);
}

} // namespace copper