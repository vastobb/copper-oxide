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
# Shader toolchain: shaderc (GLSL -> SPIR-V) plus SPIRV-Tools (spirv-val)
#
# shaderc is Khronos/Google's packaging of glslang + SPIRV-Tools + SPIRV-Headers
# behind one stable API. Composing those three by hand means resolving who owns
# SPIRV-Headers, in exactly one order, which is the part that goes wrong;
# shaderc's own CMake already resolved it. Everything is static, so the compiler
# ends up inside libcopper-oxide.so instead of adding .so files to the APK.
#
# WHY GIT_REPOSITORY and not a URL tarball: shaderc's third_party/CMakeLists.txt
# builds SPIRV-Tools and glslang only from `third_party/` subdirectories, and
# aborts with "SPIRV-Tools was not found - required for compilation" otherwise. A
# GitHub source tarball does not contain submodules, so the tarball cannot
# build. FetchContent therefore clones the three submodules we actually need.
# googletest, effcee, re2 and abseil are the test-only dependencies and are
# deliberately omitted: SHADERC_SKIP_TESTS keeps them from being referenced.
#
# Licence: Apache-2.0, compatible with this project's MIT licence.
# See THIRD_PARTY_NOTICES.md.
# ---------------------------------------------------------------------------
set(SHADERC_PINNED_TAG "v2024.4" CACHE STRING "shaderc tag; also the SPIR-V cache key version")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Static only; a shared library would add a .so" FORCE)
set(SHADERC_SKIP_TESTS ON CACHE BOOL "No gtest in an APK" FORCE)
set(SHADERC_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_INSTALL ON CACHE BOOL "Nothing is installed from an app build" FORCE)
# The copyright check downloads a script; there is nothing here to check.
set(SHADERC_SKIP_COPYRIGHT_CHECK ON CACHE BOOL "" FORCE)
# -Werror on a cross-compile for two Android ABIs is a coin flip we lose for no
# gain: a warning in a vendored project is not our bug and must not fail our build.
set(SHADERC_ENABLE_WERROR_COMPILE OFF CACHE BOOL "" FORCE)
# glslangValidator and friends are host tools with no place in an APK.
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
# shaderc drives optimisation itself; glslang does not need to carry spirv-opt.
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_PCH OFF CACHE BOOL "A PCH per vendored target is pure cost" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
# Skips spirv-val/spirv-opt/spirv-dis executables; the static library still builds.
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    shaderc
    GIT_REPOSITORY https://github.com/google/shaderc.git
    GIT_TAG ${SHADERC_PINNED_TAG}
    GIT_SHALLOW TRUE
    GIT_SUBMODULES third_party/spirv-headers third_party/spirv-tools third_party/glslang
    # Not recursive: spirv-tools has its own submodules (effcee, re2, abseil,
    # googletest, jsoncpp) and they are test-only, so cloning them would be pure
    # configure-time cost on every CI run.
    GIT_SUBMODULES_RECURSE OFF
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
    shaderc            # GLSL -> SPIR-V
)

# Real SPIR-V validation. shaderc links SPIRV-Tools privately, so both the
# library and its headers have to be requested explicitly; without this the
# translator could only do a structural check and would have to say so.
if(TARGET SPIRV-Tools)
    target_include_directories(copper-oxide PRIVATE
        "${shaderc_SOURCE_DIR}/third_party/spirv-headers/include"
        "${shaderc_SOURCE_DIR}/third_party/spirv-tools/include")
    target_compile_definitions(copper-oxide PRIVATE COPPER_HAVE_SPIRV_TOOLS=1)
    target_link_libraries(copper-oxide PRIVATE SPIRV-Tools)
else()
    message(WARNING "SPIRV-Tools is unavailable; SPIR-V will be structurally checked only")
endif()

# The pinned tag doubles as the compiler identity in the SPIR-V cache key, so it
# is compiled in rather than repeated in the source where it could drift.
target_compile_definitions(copper-oxide PRIVATE
    "COPPER_SHADERC_TAG=\"${SHADERC_PINNED_TAG}\"")

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