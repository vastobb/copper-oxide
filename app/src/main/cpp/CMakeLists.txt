cmake_minimum_required(VERSION 3.22.1)
project("copper-oxide-renderer" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Android-specific settings
if(ANDROID)
    set(CMAKE_ANDROID_STL c++_shared)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions -frtti -Wall -Wextra")
    add_definitions(-DANDROID -D__ANDROID__)
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/common
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/vulkan
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/gles
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/android
    ${CMAKE_CURRENT_SOURCE_DIR}/jni
)

# Find required libraries
find_library(log-lib log)
find_library(android-lib android)
find_library(EGL-lib EGL)
find_library(GLESv3-lib GLESv3)
find_library(vulkan-lib vulkan)

# Copper Oxide Renderer Library
add_library(copper-oxide SHARED
    # Common renderer
    renderer/common/renderer_base.cpp
    renderer/common/renderer_config.cpp
    renderer/common/gpu_capabilities.cpp
    renderer/common/buffer_manager.cpp
    renderer/common/texture_manager.cpp
    renderer/common/shader_manager.cpp
    renderer/common/framebuffer_manager.cpp
    renderer/common/state_manager.cpp
    renderer/common/command_buffer.cpp
    renderer/common/sync_manager.cpp
    renderer/common/resource_pool.cpp
    renderer/common/profiler.cpp

    # Vulkan backend
    renderer/vulkan/vulkan_renderer.cpp
    renderer/vulkan/vulkan_device.cpp
    renderer/vulkan/vulkan_swapchain.cpp
    renderer/vulkan/vulkan_pipeline.cpp
    renderer/vulkan/vulkan_descriptor.cpp
    renderer/vulkan/vulkan_command_buffer.cpp
    renderer/vulkan/vulkan_sync.cpp
    renderer/vulkan/vulkan_memory.cpp
    renderer/vulkan/vulkan_shader.cpp
    renderer/vulkan/vulkan_buffer.cpp
    renderer/vulkan/vulkan_texture.cpp
    renderer/vulkan/vulkan_framebuffer.cpp
    renderer/vulkan/vulkan_state.cpp

    # OpenGL ES backend
    renderer/gles/gles_renderer.cpp
    renderer/gles/gles_device.cpp
    renderer/gles/gles_shader.cpp
    renderer/gles/gles_buffer.cpp
    renderer/gles/gles_texture.cpp
    renderer/gles/gles_framebuffer.cpp
    renderer/gles/gles_state.cpp

    # Platform integration
    platform/android/android_platform.cpp
    platform/android/android_surface.cpp
    platform/android/android_egl.cpp
    platform/android/android_vulkan_surface.cpp

    # JNI Bridge
    jni/jni_bridge.cpp
    jni/renderer_jni.cpp
    jni/config_jni.cpp
    jni/diagnostics_jni.cpp
)

target_link_libraries(copper-oxide
    ${log-lib}
    ${android-lib}
    ${EGL-lib}
    ${GLESv3-lib}
    ${vulkan-lib}
)

# Set output directory
set_target_properties(copper-oxide PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../jniLibs/${ANDROID_ABI}
)

# Install headers
install(FILES
    renderer/common/renderer_base.h
    renderer/common/renderer_config.h
    renderer/common/gpu_capabilities.h
    renderer/vulkan/vulkan_renderer.h
    renderer/gles/gles_renderer.h
    platform/android/android_platform.h
    jni/jni_bridge.h
    DESTINATION include/copper-oxide
)