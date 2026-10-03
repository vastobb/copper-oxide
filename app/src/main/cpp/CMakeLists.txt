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

# ---------------------------------------------------------------------------
# Shader toolchain: shaderc (GLSL -> SPIR-V, plus SPIRV-Tools for validation)
#
# shaderc is Khronos' own packaging of glslang + SPIRV-Tools + SPIRV-Headers
# behind one stable C API, and it is what Android's own shader tooling links.
# Composing those three by hand means resolving who owns SPIRV-Headers, in
# exactly one order, which is the part that goes wrong; shaderc already resolved
# it. Everything below is static so the compiler ends up inside
# libcopper-oxide.so rather than adding .so files to the APK.
#
# Licence: Apache-2.0, which is compatible with this project's MIT licence.
# See THIRD_PARTY_NOTICES.md.
# ---------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Static only; a shared glslang would add a second .so" FORCE)
set(SHADERC_SKIP_TESTS     ON  CACHE BOOL "No gtest in an APK" FORCE)
set(SHADERC_SKIP_EXAMPLES  ON  CACHE BOOL "" FORCE)
set(SHADERC_SKIP_INSTALL   ON  CACHE BOOL "Nothing is installed from an app build" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "glslangValidator has no place in an APK" FORCE)
set(ENABLE_OPT             OFF CACHE BOOL "spirv-opt is not used at run time" FORCE)
set(ENABLE_HLSL            OFF CACHE BOOL "Copper Oxide has no HLSL path" FORCE)
set(ENABLE_GLSLANG_JS      OFF CACHE BOOL "" FORCE)
set(ENABLE_EXTERNAL        OFF CACHE BOOL "Do not build glslang's External/ deps" FORCE)
set(ENABLE_PCH             OFF CACHE BOOL "A PCH per target in a FetchContent build is pure cost" FORCE)
# ON so glslang's objects match copper-oxide's -frtti/-fexceptions instead of
# differing across a static-library boundary.
set(ENABLE_RTTI            ON  CACHE BOOL "" FORCE)
set(ENABLE_EXCEPTIONS      ON  CACHE BOOL "Compile failures are returned, not thrown" FORCE)
set(GLSLANG_TESTS          OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    shaderc
    URL https://github.com/google/shaderc/archive/refs/tags/v2024.4.tar.gz
)
FetchContent_MakeAvailable(shaderc)


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
    renderer/common/shader_cache.cpp
    renderer/common/shader_translator.cpp
    renderer/common/shaderc_translator.cpp

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
    renderer/gles/gles_missing_es31.cpp

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
    shaderc            # GLSL -> SPIR-V, and SPIRV-Tools for validation
)

# Visibility and PIC only.
#
# LIBRARY_OUTPUT_DIRECTORY is deliberately NOT set. The Android Gradle Plugin
# drives an externalNativeBuild, and it points the library output at
# build/intermediates/cxx/<abi>/<buildType>/<abi>/ and packages the .so from
# there. Overriding the directory sends the library somewhere AGP does not look,
# so the APK ships without it and System.loadLibrary fails with "not found" at
# run time - which is exactly what the instrumented tests were hitting.
set_target_properties(copper-oxide PROPERTIES
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