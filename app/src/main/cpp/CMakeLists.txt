cmake_minimum_required(VERSION 3.22.1)
project("copper-oxide-renderer" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Android-specific settings
if(ANDROID)
    set(CMAKE_ANDROID_STL c++_shared)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions -frtti -Wall -Wextra -O3")
    add_definitions(-DANDROID -D__ANDROID__ -DNDEBUG)
endif()

# Build type configurations
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
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

# Vulkan Memory Allocator (official GPUOpen source, fetched at configure time)
include(FetchContent)
FetchContent_Declare(
    vma
    URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz
)
FetchContent_MakeAvailable(vma)

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
    renderer/vulkan/vma_impl.cpp
    renderer/vulkan/vulkan_buffer_manager.cpp
    renderer/vulkan/vulkan_texture_manager.cpp
    renderer/vulkan/vulkan_shader_manager.cpp
    renderer/vulkan/vulkan_state_manager.cpp
    renderer/vulkan/vulkan_framebuffer_manager.cpp
    renderer/vulkan/vulkan_sync_manager.cpp
    renderer/vulkan/vulkan_resource_pool.cpp
    renderer/vulkan/vulkan_profiler.cpp
    renderer/vulkan/vulkan_command_buffer.cpp
    renderer/vulkan/vulkan_command_sink.cpp

    # OpenGL ES backend
    renderer/gles/gles_renderer.cpp
    renderer/gles/gles_buffer_manager.cpp
    renderer/gles/gles_texture_manager.cpp
    renderer/gles/gles_shader_manager.cpp
    renderer/gles/gles_state_manager.cpp
    renderer/gles/gles_framebuffer_manager.cpp
    renderer/gles/gles_sync_manager.cpp
    renderer/gles/gles_resource_pool.cpp
    renderer/gles/gles_profiler.cpp
    renderer/gles/gles_command_buffer.cpp
    renderer/gles/gles_command_sink.cpp
    renderer/gles/gles_es31_compat.cpp

    # Platform integration
    platform/android/android_platform.cpp

    # JNI Bridge
    jni/jni_bridge.cpp
)

target_include_directories(copper-oxide PRIVATE ${vma_SOURCE_DIR}/include)

target_link_libraries(copper-oxide
    ${log-lib}
    ${android-lib}
    ${EGL-lib}
    ${GLESv3-lib}
    ${vulkan-lib}
)

# Set output directory
set_target_properties(copper-oxide PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON
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