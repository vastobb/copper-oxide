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
# Vulkan has no GLSL dialect: a Vulkan shader module is a SPIR-V binary. shaderc
# is Khronos/Google's packaging of glslang + SPIRV-Tools + SPIRV-Headers behind
# one stable API. Everything is static, so the compiler ends up inside
# libcopper-oxide.so instead of adding .so files to the APK.
#
# WHY THE THREE DEPENDENCIES ARE FETCHED HERE AND NOT BY shaderc ITSELF
#
# shaderc v2024.4 has no .gitmodules at all; third_party/ holds only CMake glue
# that add_subdirectory's SPIRV-Tools and glslang from third_party/<name>. Its
# own configure therefore aborts with "SPIRV-Tools was not found" unless those
# directories are already populated - which is what utils/git-sync-deps and
# DEPS exist to do. Rather than depend on that script, the three projects are
# fetched here, in the order they depend on each other, and shaderc then reuses
# the targets that already exist: its glue is guarded by `if (NOT TARGET ...)`
# for both SPIRV-Tools and glslang, and by `if (IS_DIRECTORY ...)` for the
# headers, which a plain clone does not have. So exactly one copy of each is
# built, in one place.
#
# Order matters and is load-bearing:
#   1. SPIRV-Headers first. It is header-only and creates the SPIRV-Headers
#      INTERFACE target. FetchContent also defines SPIRV-Headers_SOURCE_DIR,
#      which SPIRV-Tools honours: it uses that directory in preference to its
#      own submodule and skips adding it again. SPIRV-Tools does not link that
#      target for its own sources, so the include directory is attached to it
#      explicitly below - SPIR-V headers are just .h and .hpp files, and an
#      extra include directory cannot break a target that already had it.
#   2. SPIRV-Tools second, so its SPIRV-Tools target exists.
#   3. glslang third. ENABLE_OPT is off, so glslang does not pull in the
#      optimiser or SPIRV-Tools itself. The same include directory is attached
#      for the same reason.
#   4. shaderc last, which finds both targets already defined and adds neither.
#
# The revisions are the ones shaderc's own DEPS pins for v2024.4, so the three
# are exactly the combination Khronos tests together rather than three
# independently chosen "latest" tags.
#
# Licence: Apache-2.0, compatible with this project's MIT licence.
# See THIRD_PARTY_NOTICES.md.
# ---------------------------------------------------------------------------
set(SHADERC_PINNED_TAG "v2024.4" CACHE STRING "shaderc tag; also the SPIR-V cache key version")
set(SPIRV_HEADERS_REV "3f17b2af6784bfa2c5aa5dbb8e0e74a607dd8b3b" CACHE STRING "From shaderc's DEPS")
set(SPIRV_TOOLS_REV   "4d2f0b40bfe290dea6c6904dafdf7fd8328ba346" CACHE STRING "From shaderc's DEPS")
set(GLSLANG_REV       "a0995c49ebcaca2c6d3b03efbabf74f3843decdb" CACHE STRING "From shaderc's DEPS")

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
# shaderc drives optimisation itself; glslang does not need to carry spirv-opt,
# which also means it does not need SPIRV-Tools.
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "glslang's External/ submodules are not populated" FORCE)
set(ENABLE_PCH OFF CACHE BOOL "A PCH per vendored target is pure cost" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
# Skips the spirv-val/spirv-opt/spirv-dis executables; the library still builds.
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)

FetchContent_Declare(spirv_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
    GIT_TAG ${SPIRV_HEADERS_REV} GIT_SHALLOW OFF)
FetchContent_MakeAvailable(spirv_headers)

FetchContent_Declare(spirv_tools
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git
    GIT_TAG ${SPIRV_TOOLS_REV} GIT_SHALLOW OFF)
FetchContent_MakeAvailable(spirv_tools)

FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG ${GLSLANG_REV} GIT_SHALLOW OFF)
FetchContent_MakeAvailable(glslang)

# glslang's own CMake puts a SPIR-V headers include directory on its targets
# only on the ENABLE_OPT branch, because that is the branch where it pulls in
# its own External/spirv-headers submodule. With the optimiser off - which is
# what keeps glslang from needing a second copy of SPIRV-Tools - nothing is
# added, and its sources still include <spirv/unified1/spirv.hpp>.
#
# So the directory is attached here, once, to the two targets that need it.
# This has to happen AFTER FetchContent_MakeAvailable(glslang), because before
# then those targets do not exist and target_include_directories would fail.
if(DEFINED SPIRV-Headers_SOURCE_DIR)
    set(COPPER_SPIRV_HEADERS_INCLUDE "${SPIRV-Headers_SOURCE_DIR}/include")
    foreach(dependent_target SPIRV-Tools SPIRV-Tools-static glslang SPIRV)
        if(TARGET ${dependent_target})
            target_include_directories(${dependent_target} PUBLIC "${COPPER_SPIRV_HEADERS_INCLUDE}")
        endif()
    endforeach()
endif()

FetchContent_Declare(shaderc
    GIT_REPOSITORY https://github.com/google/shaderc.git
    GIT_TAG ${SHADERC_PINNED_TAG} GIT_SHALLOW TRUE)
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
        "${COPPER_SPIRV_HEADERS_INCLUDE}"
        "${spirv_tools_SOURCE_DIR}/include")
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