# Shader system — GLSL → SPIR-V → GLSL ES

Status: design, not implemented. Nothing in this document is in the tree yet.

> **How to read the line references.** Every `file.cpp:NNN` in this document is anchored to
> commit **`3531309`**. Other workstreams are editing these files concurrently — the per-backend
> `VulkanShaderManager` and `VulkanRenderer` sources have already moved since that commit — so
> **re-anchor before you edit**: `git grep -n '<symbol>' app/src/main/cpp/<path>`. Where a
> reference is load-bearing (i.e. a change lands in that exact place), the function name is given
> alongside the line number so a drifted line number is obvious rather than misleading.

Scope: the source-to-source translation path that lets Copper Oxide accept desktop GLSL
(Minecraft / Iris shader packs) and hand something the device driver can compile, on both
backends. The companion document [`pipeline-cache.md`](pipeline-cache.md) covers what happens
after the driver gets the shader — pipeline compilation, caching and specialization constants.

Companion doc: [`pipeline-cache.md`](pipeline-cache.md).

---

## 0. Where the code stands today (read this first)

Verified against `HEAD` = `3531309`:

| Fact | Evidence |
|---|---|
| No shader compiler is vendored. `CMakeLists.txt` fetches **only** VulkanMemoryAllocator. | `app/src/main/cpp/CMakeLists.txt:36-42` |
| `ShaderManager::createShaderFromGLSL()` is a placeholder that delegates to a backend hook and carries the comment "In a real implementation, this would use glslang to compile GLSL to SPIR-V". | `app/src/main/cpp/renderer/common/shader_manager.cpp:88-105` |
| `VulkanShaderManager::onCreateShaderFromGLSL()` fails honestly and returns `false`; its comment is the sketch this document replaces. | `renderer/vulkan/vulkan_shader_manager.cpp:488-513` |
| `ShaderManager::compileAsync()` runs inline. Its own comment says callers "must treat this as blocking". | `renderer/common/shader_manager.cpp:258-266` |
| The GLES backend compiles the caller's source verbatim on the device driver (`glShaderSource` after a define-injection pass). It is not a translator. | `renderer/gles/gles_shader_manager.cpp:324-391`, `inject_defines()` at `:137-174` |
| The per-backend manager `.cpp` files are **not in the build**. *(Partially resolved in the working tree after `3531309`: `CMakeLists.txt` now lists `renderer/vulkan/vulkan_*.cpp`. Confirm before you start.)* | `CMakeLists.txt:45-72` at `3531309` lists 17 `.cpp` files; README §"File-by-file facts" |
| `VulkanShaderManager` is defined twice (`vulkan_renderer.h:275-313` inline stub and `vulkan_shader_manager.h:59`). Both headers' `INTEGRATOR NOTE`s say the stubs must be deleted. | `vulkan_shader_manager.h:54-58`, `vulkan_framebuffer_manager.h:25-31`, `vulkan_state_manager.h:29-35` |
| `VulkanRenderer` exposes no public `device()`, `instance()`, `physicalDevice()`, `renderPass()` or `pipelineCache()` accessors. *(Resolved in the working tree after `3531309` — see `vulkan_renderer.h` public section.)* | requested verbatim in `vulkan_framebuffer_manager.h:33-45` and `vulkan_state_manager.h:37-46` |

**Two blockers must be cleared before any of this lands** (integration work, not design work;
tracked in README §"Roadmap"):

1. Delete the inline manager stubs from `vulkan_renderer.h` (`VulkanBufferManager` at `:203`,
   `VulkanShaderManager` at `:275`, `VulkanFramebufferManager` at `:316`, `VulkanStateManager` at
   `:317`) and replace them with `#include "vulkan_*_manager.h"`. Reconcile
   `GLESShaderManager` (`gles_shader_manager.h:38`) vs `GLESCShaderManager`
   (`gles_renderer.h:232`). **Check the current `vulkan_renderer.h` first** — a concurrent
   workstream is removing these stubs.
2. Add the accessors to `VulkanRenderer`'s public section and add every per-backend `.cpp` to
   `add_library(copper-oxide SHARED ...)`. **Also check first** — same workstream.

Neither is interesting; both are prerequisites.

---

## 1. The pipeline, and why it is two tools

```
                 ┌─────────────── render thread (has GL/EGL context, has VkDevice) ───────────────┐
  GLSL source ──▶│ ShaderManager::createShaderFromGLSL / compileAsync                            │
  + defines      └──────────────┬──────────────────────────────────────────┬────────────────────┘
                                │ tier 1: isDirectShader?                   │ tier 1: always translate
                                ▼                                          ▼
                       passthrough to driver                        ShaderTranslator (worker thread)
                 (GLES only: GLESShaderManager)          ┌───────────────────┴────────────────────┐
                                                       │ glslang: GLSL ─▶ SPIR-V               │  SPIRV-Cross: SPIR-V ─▶ ESSL
                                                       │ EShClientVulkan  │ EShClientOpenGL   │
                                                       └──────────┬─────────────────────────────┘
                                                                  ▼
                                                        ShaderTranslator::onResult
                                                                  │  (completion queue, drained at frame start)
                                                                  ▼
                                          backend: vkCreateShaderModule  /  glShaderSource+glCompileShader
```

Two tools, two very different jobs:

* **glslang** GLSL → SPIR-V. Parse with **Vulkan input semantics** (`EShClientVulkan`) because
  Minecraft/Iris GLSL uses explicit `layout(set=, binding=)`, `layout(location=)` and push
  constants — none of which exist in the desktop-GLSL dialect glslang would otherwise assume.
* **SPIRV-Cross** SPIR-V → GLSL ES, through its **C API** (`spirv_cross_c.h`), which is the
  ABI-stable entry point and the only one that survives SPIRV-Cross's C++ API churn.

This is exactly the pair MobileGlues ships
([`MobileGlues-cpp/CMakeLists.txt`](https://github.com/MobileGL-Dev/MobileGlues/blob/main/MobileGlues-cpp/CMakeLists.txt)
links `glslang::glslang` and `spirv-cross-c`;
[`gl/glsl/glsl_for_es.cpp`](https://github.com/MobileGL-Dev/MobileGlues/blob/main/MobileGlues-cpp/gl/glsl/glsl_for_es.cpp)).

### 1.1 Target semantics differ per backend — and that is a cache-key field

glslang is told three things independently: the **input** dialect, the **client** that will host
the result, and the **output** language
([`glslang::TShader::setEnvInput/setEnvClient/setEnvTarget`](https://github.com/KhronosGroup/glslang/blob/main/glslang/Public/ShaderLang.h)).

* **Vulkan backend** (artifact is SPIR-V handed to `vkCreateShaderModule`):
  ```cpp
  shader.setEnvInput (EShSourceGlsl, stage, EShClientVulkan, glsl_version);
  shader.setEnvClient(EShClientVulkan, EShTargetVulkan_1_1);
  shader.setEnvTarget(EShTargetSpv,    EShTargetSpv_1_5);
  ```
  `EShTargetVulkan_1_1` matches `VulkanRenderer::create_instance()`, which sets
  `app_info.apiVersion = VK_API_VERSION_1_1` (`vulkan_renderer.cpp:228`).
* **GLES backend** (artifact is ESSL text handed to `glCompileShader`):
  ```cpp
  shader.setEnvInput (EShSourceGlsl, stage, EShClientVulkan, glsl_version);
  shader.setEnvClient(EShClientOpenGL, EShTargetOpenGL_450);   // <- emit with OpenGL semantics
  shader.setEnvTarget(EShTargetSpv,    EShTargetSpv_1_5);
  shader.setPreamble("#undef VULKAN\n");
  ```
  This is verbatim MobileGlues' configuration
  ([`glsl_for_es.cpp:717-722`](https://github.com/MobileGL-Dev/MobileGlues/blob/main/MobileGlues-cpp/gl/glsl/glsl_for_es.cpp#L717)).
  The *client* is OpenGL precisely because the SPIR-V is only an intermediate here: SPIRV-Cross
  must be able to render it as ESSL. Emitting with Vulkan semantics makes SPIRV-Cross emit
  `layout(set=…, binding=…)`, which ESSL does not accept.

Because the client differs, **the same source produces different SPIR-V for the two backends**.
That is why `target_api` is a first-class field of the cache key (§4).

### 1.2 `TBuiltInResource` must be complete — this is a proven foot-gun

glslang seeds its built-in block from a `TBuiltInResource`. MobileGlues hit a bug worth
copying verbatim so we do not rediscover it
([`glsl_for_es.cpp:133-155`](https://github.com/MobileGL-Dev/MobileGlues/blob/main/MobileGlues-cpp/gl/glsl/glsl_for_es.cpp#L133)):

> Ten fields this table never set, left at 0 by the value-initialisation above. Nine are
> mesh-shader limits that glslang only reads when a shader asks for them, so 0 was harmless.
> **`maxDualSourceDrawBuffersEXT` was not**: glslang emits
> `mediump vec4 gl_SecondaryFragDataEXT[gl_MaxDualSourceDrawBuffersEXT];` into the ESSL built-in
> block, and an array sized 0 fails to parse — which fails the whole built-in table, so **every
> shader routed through glslang was rejected** with "unsupported shader version".

Rule for this codebase:

* Start from `glslang::TBuiltInResource{}` (value-initialised) and set **every** field, including
  the ones glslang only reads on demand. Copy MobileGlues' `InitResources()`
  (`glsl_for_es.cpp:27-158`) as the starting table — it is the only battle-tested one we have
  seen — and add a unit test that asserts no field is zero except the intentional
  `maxVertexAtomicCounters = 0` / `maxGeometryAtomicCounters = 0` style ones, with an
  allow-list.
* Because we populate it from device limits, the resource table is **device-dependent**, and
  therefore participates in the SPIR-V cache key (§4).

### 1.3 Pass is mandatory for ESSL, and its result must be checked

```cpp
// SPIRV-Cross: a silently dropped GLSL_ES option emits desktop GLSL and hands it
// straight to the driver, so these calls are checked too.
// (MobileGlues, glsl_for_es.cpp:817-829)
if (spvc_compiler_options_set_uint (options, SPVC_COMPILER_OPTION_GLSL_VERSION, essl_version) != SPVC_SUCCESS) return fail;
if (spvc_compiler_options_set_bool (options, SPVC_COMPILER_OPTION_GLSL_ES,     SPVC_TRUE)     != SPVC_SUCCESS) return fail;
if (spvc_compiler_install_compiler_options(compiler, options)                               != SPVC_SUCCESS) return fail;
```

Both `GLSL_VERSION` and `GLSL_ES` must be set **and** checked
([`spirv_cross_c.h`](https://github.com/KhronosGroup/SPIRV-Cross/blob/master/spirv_cross_c.h),
`SPVC_COMPILER_OPTION_GLSL_VERSION = 8 | GLSL_BIT`, `SPVC_COMPILER_OPTION_GLSL_ES = 9 | GLSL_BIT`).
`spvc_result` values other than `SPVC_SUCCESS` are negative error codes, and on failure
SPIRV-Cross leaves out-parameters untouched, so a dropped return code hands the next call an
uninitialised handle — MobileGlues wraps every call in a `spvc_ok()` helper for exactly that
reason (`glsl_for_es.cpp:766-775`).

Use `SPVC_CAPTURE_MODE_TAKE_OWNERSHIP` and RAII-wrap `spvc_context` — the emitted string is owned
by the context and is only valid until `spvc_context_destroy` (`spvc_context_guard_t`,
`glsl_for_es.cpp:755-763`).

---

## 2. Vendoring: the exact CMake

Follows the existing VMA pattern in `app/src/main/cpp/CMakeLists.txt:36-42` (FetchContent at
configure time, static, no install rules). Insert **after** the VMA block and **before**
`add_library(copper-oxide SHARED ...)`.

```cmake
# ---------------------------------------------------------------------------
# Shader toolchain: glslang (GLSL -> SPIR-V) + SPIRV-Cross (SPIR-V -> GLSL ES)
#
# Option set follows MobileGlues-cpp/CMakeLists.txt, which is the only
# Android/mobile deployment of this pair we have measured:
#   https://github.com/MobileGL-Dev/MobileGlues/blob/main/MobileGlues-cpp/CMakeLists.txt
# We use FetchContent instead of their git submodules so the build needs no
# manual submodule step, matching how VMA is vendored above.
# ---------------------------------------------------------------------------

# SPIRV-Cross's own project() call declares LANGUAGES CXX C, and a nested
# project() can enable extra languages. This line is therefore defensive, not
# required: with SPIRV_CROSS_CLI=OFF the only C source (their c_api_test.c) is
# never compiled. Kept so a future option flip cannot surprise us.
enable_language(C)

# glslang picks STATIC vs SHARED from BUILD_SHARED_LIBS. Pin it: a shared glslang
# would mean a second .so in jniLibs/ and a second global symbol namespace on
# Android (see the -Bsymbolic-functions note in MobileGlues' CMakeLists).
# Safe against the VMA FetchContent above: VulkanMemoryAllocator v3.3.0 is a header-only
# INTERFACE target and does not branch on BUILD_SHARED_LIBS.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# --- glslang ---------------------------------------------------------------
set(ENABLE_SPIRV           ON  CACHE BOOL "We only need SPIR-V output" FORCE)
set(ENABLE_OPT             OFF CACHE BOOL "Disable spirv-opt usage in glslang" FORCE)
set(ENABLE_HLSL            OFF CACHE BOOL "Disable HLSL input for glslang" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "Disable glslangValidator/spirv-remap" FORCE)
set(ENABLE_RTTI            ON  CACHE BOOL "Keep RTTI consistent with -frtti below" FORCE)
set(ENABLE_EXCEPTIONS      ON  CACHE BOOL "ShaderManager propagates parse errors" FORCE)
set(BUILD_EXTERNAL         OFF CACHE BOOL "Do not build glslang's External/ deps" FORCE)
set(GLSLANG_TESTS          OFF CACHE BOOL "No gtest in an APK" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "Nothing is installed from this build" FORCE)
set(ENABLE_PCH             OFF CACHE BOOL "No PCH in a static-lib FetchContent build" FORCE)

# --- SPIRV-Cross -----------------------------------------------------------
set(SPIRV_CROSS_ENABLE_C_API    ON  CACHE BOOL "Enable C API" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL     ON  CACHE BOOL "GLSL (ESSL) backend only" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL     OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL      OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP      OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT  OFF CACHE BOOL "We reflect via the C API, not the JSON backend" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL     OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_CLI             OFF CACHE BOOL "No spirv-cross binary in an APK" FORCE)
set(SPIRV_CROSS_SHARED          OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_STATIC          ON  CACHE BOOL "Static libs" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS    OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL    ON  CACHE BOOL "No install/export rules" FORCE)

FetchContent_Declare(
    glslang
    URL https://github.com/KhronosGroup/glslang/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
)
FetchContent_Declare(
    spirv-cross
    URL https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
)
FetchContent_MakeAvailable(glslang spirv-cross)
```

And the target wiring, next to the existing `target_link_libraries` at `CMakeLists.txt:76-82`:

```cmake
# glslang's target carries its PUBLIC include dir (the glslang root) and the
# generated build_info.h dir; SPIRV-Cross's spirv-cross-c carries its PUBLIC
# include dir (the SPIRV-Cross root, which is where spirv.h lives).
target_link_libraries(copper-oxide
    glslang::glslang      # GLSL -> SPIR-V  (TShader/TProgram/GlslangToSpv)
    spirv-cross-c         # SPIR-V -> ESSL  (spvc_* C API; PRIVATE-links glsl+core)
)
```

Resulting include paths (verified against each project's CMake):
`#include <glslang/Public/ShaderLang.h>`, `#include <glslang/Include/Types.h>`,
`#include <glslang/SPIRV/GlslangToSpv.h>`, `#include <spirv_cross_c.h>`.
(The `spirv_cross/spirv_cross_c.h` spelling MobileGlues uses only works because they copy headers
into their own `include/spirv_cross/`; with FetchContent use the bare name.)

### 2.1 Why each option, one line each

| Option | Value | Why |
|---|---|---|
| `ENABLE_OPT` | `OFF` | glslang's root `CMakeLists.txt` does `if (ENABLE_OPT) message(SEND_ERROR "ENABLE_OPT set but SPIR-V tools not found")` when the `SPIRV-Tools-opt` target is absent. Without this line **configure fails**. It also drops the SPIRV-Tools dependency (a third vendored project, minutes of build time, ~1 MB of APK) we do not need — we do no SPIR-V optimisation. |
| `ENABLE_HLSL` | `OFF` | Drops 7 `HLSL/*.cpp` files from `MachineIndependent`, and upstream now prints `message(DEPRECATION ...)` for this option. Copper Oxide has no HLSL path at all, so there is nothing to lose. |
| `ENABLE_GLSLANG_BINARIES` | `OFF` | Skips `StandAlone/` (`glslangValidator`, `spirv-remap`). Nothing links them. (glslang forces this off on `ANDROID` anyway; setting it makes the intent explicit and keeps desktop/CI configure working.) |
| `SPIRV_CROSS_ENABLE_C_API` | `ON` | This is what builds the `spirv-cross-c` target we link. The C++ API is not built at all. |
| `SPIRV_CROSS_ENABLE_GLSL` | `ON` | The ESSL emitter is `spirv_glsl.cpp`. Required — `SPIRV_CROSS_ENABLE_CPP` `FATAL_ERROR`s without it. |
| `SPIRV_CROSS_ENABLE_{HLSL,MSL,CPP,REFLECT,UTIL}` | `OFF` | ~1/3 of SPIRV-Cross is dead weight for us (HLSL, Metal, C++, JSON reflection, `spirv-cross-util`). APK size and build time only. |
| `SPIRV_CROSS_CLI` | `OFF` | The `spirv-cross` binary requires GLSL **and** HLSL **and** MSL **and** CPP **and** REFLECT **and** UTIL and static, or CMake `FATAL_ERROR`s. Turning it off is what lets the five `OFF`s above be legal at all. |
| `SPIRV_CROSS_STATIC` / `BUILD_SHARED_LIBS` | `ON` / `OFF` | Static, so the tools end up inside `libcopper-oxide.so` instead of adding `.so` files. |
| `ENABLE_RTTI` | `ON` | glslang's default is `OFF`, which appends `-fno-rtti` to glslang's objects while `copper-oxide` compiles with `-frtti` (`CMakeLists.txt:11`). That flag mismatch across a static-library boundary is a latent hazard; `ON` costs nothing. MobileGlues sets it for the same reason. |
| `ENABLE_EXCEPTIONS` | `ON` | We propagate glslang/SPIRV-Cross failures as `false` returns; a `-fno-exceptions` build would turn those into aborts. (`-fexceptions` is already on: `CMakeLists.txt:11`.) |
| `GLSLANG_TESTS`, `GLSLANG_ENABLE_INSTALL`, `SPIRV_CROSS_SKIP_INSTALL` | `OFF`/`ON` | No gtest, no `install()`/`export()` rules in an app build. glslang already defaults both off when not the top-level project; SPIRV-Cross does not, and its install block writes cmake config files into the build dir for nothing. |
| `ENABLE_PCH` | `OFF` | glslang defaults it on; a PCH per target in a FetchContent static build is pure build time for an out-of-tree dependency. |
| `BUILD_EXTERNAL` | `OFF` | glslang would otherwise try `add_subdirectory(External)` if that directory exists in the tarball. |
| `enable_language(C)` | — | Defensive: SPIRV-Cross's `project()` declares `LANGUAGES CXX C`, and a nested `project()` can enable extra languages, so this line is belt-and-braces rather than required. |

### 2.2 Pinning and supply chain

The tags above (`vulkan-sdk-1.4.363.0`) are the current matching Vulkan SDK tags for both
projects. Add `URL_HASH SHA256=...` to both `FetchContent_Declare` calls — the VMA entry above
does not have one and that is a pre-existing gap worth fixing in the same commit, since CI runs
`dependency-check`/Trivy over the fetched sources.

Version numbers also go into the **cache key** (§4) so an upgrade cannot silently reuse stale
artifacts.

---

## 3. The `ShaderTranslator` interface

New files: `app/src/main/cpp/renderer/common/shader_translator.h` and `.cpp`.
It is deliberately backend-agnostic: it touches no GL and no Vulkan handle, only CPU text
transforms, which is what makes it safe on a worker thread (§5).

```cpp
#pragma once
// renderer/common/shader_translator.h
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "shader_manager.h"   // for ShaderStage

namespace copper {

// What the caller handed us. Decides which key we build and which tool runs.
enum class ShaderSourceFormat : uint32_t {
    Unknown  = 0,
    GlslSource = 1,   // desktop / ESSL text
    SpirvWords  = 2,  // already-compiled SPIR-V, pass through or lower to ESSL
};

// What the consumer needs out. Matches ShaderManager's two backends.
enum class TranslationTarget : uint32_t {
    Unknown = 0,
    GlslEs   = 1,   // text for glShaderSource (GLES backend)
    SpirvVulkan = 2 // words for vkCreateShaderModule (Vulkan backend)
};

// Which rung of the ladder produced this result. See §5.4.
enum class TranslationTier : uint8_t {
    DirectPassthrough = 0,  // driver compiled the caller's source unchanged
    Translated        = 1,  // glslang + SPIRV-Cross succeeded at the requested target
    DegradedTarget    = 2,  // succeeded at a lower target version after rewriting
    MinimalFallback   = 3,  // compiled-in last-resort shader
    ForcedSuccess     = 4,  // DEBUG ONLY: untranslated source, success reported anyway
};

struct ShaderTranslationResult {
    TranslationTier   tier      = TranslationTier::MinimalFallback;
    bool              ok        = false;   // artifact is usable
    std::string       text;                // ESSL, when target == GlslEs
    std::vector<uint32_t> spirv;           // words, when target == SpirvVulkan
    uint32_t          target_version_used  = 0;  // ESSL version actually emitted
    std::string       info_log;            // glslang getInfoLog() / spvc last error
};

struct ShaderTranslationRequest {
    ShaderStage          stage = ShaderStage::Vertex;
    TranslationTarget    target = TranslationTarget::Unknown;
    std::string_view     source;                        // GLSL text or nothing
    std::span<const uint32_t> spirv;                    // set when format == SpirvWords
    std::vector<std::string> defines;                   // NOT sorted here; buildCacheKey sorts
    uint32_t             target_essl_version = 320;     // GlslEs only
    uint32_t             rewrite_pass_version = kRewritePassVersion;
    bool                 force_translate = false;       // debug: skip tier 0
};

class ShaderTranslator {
public:
    virtual ~ShaderTranslator() = default;

    // GLSL text -> SPIR-V words. Vulkan semantics. Safe on any thread.
    virtual ShaderTranslationResult compileGLSLtoSPIRV(const ShaderTranslationRequest&) = 0;

    // SPIR-V words -> GLSL ES text. OpenGL semantics out, GLSL_ES forced on.
    // Safe on any thread.
    virtual ShaderTranslationResult translateSPIRVtoESSL(const ShaderTranslationRequest&) = 0;

    // Tier 0. True when `glsl` is already valid for `host_essl_version` and can go
    // straight to the driver with no glslang involvement.
    //
    // NOT THREAD SAFE: the GLES implementation must query the live context
    // (GLESShaderManager::es_version_at_least() calls glGetIntegerv), so this is
    // only called from the render thread. Keep it out of compileAsync().
    virtual bool isDirectShader(std::string_view glsl, uint32_t host_essl_version) const = 0;

    // Stable cache key bytes (§4). Pure function of the request + the process-wide
    // identity blob; safe on any thread.
    virtual std::string buildCacheKey(const ShaderTranslationRequest&,
                                      ShaderSourceFormat) const = 0;

    // The 32-byte device-identity blob folded into SPIR-V keys (§4.3). Set once by
    // the backend at construction from VkPhysicalDeviceProperties.
    virtual void setDeviceIdentity(const DeviceIdentity&) = 0;

    // Tier 3/4 support. Pre-compiled once, synchronously, in the constructor, so
    // the fallback never needs a file on disk or a second glslang run.
    virtual const ShaderTranslationResult& minimalFallback(TranslationTarget) const = 0;

    // True when the debug-only tier 5 is permitted. Off unless explicitly enabled.
    virtual bool allowForcedSuccess() const = 0;
};

} // namespace copper
```

Notes:

* `compileGLSLtoSPIRV` / `translateSPIRVtoESSL` are the two entry points; a `translate()` helper
  that chains them is a convenience, not part of the interface.
* `isDirectShader` mirrors MobileGlues' `is_direct_shader()` / `can_run_essl3()`
  ([`gl/shader.cpp:26-47`](https://github.com/MobileGL-Dev/MobileGlues/blob/main/MobileGlues-cpp/gl/shader.cpp#L26)):
  `#version 100` always passes; `#version 300/310/320 es` pass when
  `host_essl_version >= that`; anything else (i.e. desktop `#version 450 core`) fails. Skipping
  this test is the single biggest avoidable cost in the whole system — it is the difference
  between "driver compiles what it already understands" and "we run glslang for nothing".
* `minimalFallback` returning a *pre-built* artifact matters: tier 3 must work when glslang
  itself is the thing that failed.

### 3.1 Host ESSL version plumbing

`GLESShaderManager` already has `es_version_at_least(int,int)`
(`gles_shader_manager.h:121`, `gles_shader_manager.cpp:672-678`). `ShaderTranslator` needs the
same number, but it is backend-agnostic and has no context. Pass it in:

```cpp
// GLESShaderManager::onCreateShaderFromGLSL, replacing the direct glShaderSource call at :357-378
if (translator_->isDirectShader(glsl_source, host_essl_version_)) {
    // tier 0: exactly today's behaviour, just decided explicitly
}
```

`host_essl_version_` is captured once on the render thread (from the same
`glGetIntegerv(GL_MAJOR_VERSION/GL_MINOR_VERSION)` that `es_version_at_least()` uses) and
refreshed when the EGL context is recreated. Do **not** query it from a worker thread.

---

## 4. Cache key: exact byte layout

Two artifacts, therefore two keys, from one builder. The layout is deliberately
self-delimiting — every field is length-prefixed — so a new field can be appended without
invalidating the framing, only the entries (bump `kCacheKeyVersion`).

### 4.1 Framing

```
+0   u32  magic          = 0x434F4B31  ('C','O','K','1', little endian)
+4   u32  key_version    = 1
+8   ...  field stream ...
```

Every field in the stream is:

```
     u32  byte_length (little endian)
     byte payload[byte_length]
```

Scalars are encoded as fixed-width little-endian payloads, so the whole key is one byte string
with no struct padding and no endianness ambiguity.

### 4.2 Field stream — exact order

| # | Field | Encoding | Present for |
|---|---|---|---|
| 0 | **format tag** | `u32` LE: `0x00000001` = GlslSource, `0x00000002` = SpirvWords | always |
| 1 | **stage** | `u8` = `ShaderStage` enum value (`renderer/common/shader_manager.h:15-30`) | always |
| 2 | **source bytes** | raw source text bytes for `GlslSource`; raw SPIR-V **words, little endian, 4 bytes each** for `SpirvWords` | always |
| 3 | **sorted defines** | `u32 count`, then `count` × (`u32 len`, `len` bytes) | always (count 0 is legal) |
| 4 | **target api** | ASCII: `"gl"` or `"vulkan"` | always |
| 5 | **toolchain versions** | `u32` LE glslang version (`glslang::GetKhronosToolId()` + `GLSLANG_VERSION` packed; see §4.4) **then** `u32` LE packed `SPVC_C_API_VERSION_MAJOR/MINOR` | always |
| 6 | **driver identity** | 32 bytes, §4.3 | **`SpirvWords` key only** |
| 7 | **rewrite-pass version** | `u32` LE, `kRewritePassVersion` | always |

Defines are normalised before hashing: strip leading/trailing whitespace, drop empties, **sort
bytewise ascending, de-duplicate**. Sort order matters — `getOrCreateShader("chunk", …)` is
called with `defines` assembled per call site
(`ShaderManager::createShaderFromGLSL(stage, source, entry_point, defines)`,
`shader_manager.h:85`), and nothing guarantees two call sites pass them in the same order.
Unsorted defines would produce two cache entries for one shader.

Field 3 hashing the *defines only* and not a concatenated `#define` block is deliberate: the
`#define` text we actually generate lives in `GLESShaderManager::inject_defines()`
(`gles_shader_manager.cpp:137-174`), and a caller is allowed to pass a ready-made directive that
starts with `#` (`:150-153`). Hashing the normalised list is stable against that formatting.

### 4.3 The driver-identity blob (field 6)

Exactly 32 bytes, laid out like `VkPipelineCacheHeaderVersionOne`
([spec](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#VkPipelineCacheHeaderVersionOne)):

```
+0   u32  header_version = 1
+4   u32  vendorID            (VkPhysicalDeviceProperties::vendorID)
+8   u32  deviceID            (VkPhysicalDeviceProperties::deviceID)
+12  u32  driverVersion       (VkPhysicalDeviceProperties::driverVersion)
+16  byte pipelineCacheUUID[16]
```

**Why the ESSL key does not carry it, and the SPIR-V key does.** The common claim "SPIR-V is not
portable across drivers" is only half right, and the distinction decides the cache layout:

* **glslang's output SPIR-V is portable.** It is a deterministic function of (source, defines,
  glslang version, the `TBuiltInResource` table, target env). Nothing about the Adreno it will run
  on enters the generator. Nothing driver-specific is stored.
* **What is not portable is the driver's own compilation product** — `VkPipelineCache` blobs,
  `VkPipelineBinaryKHR` binaries. The spec says so about the cache: *"The results of pipeline
  compiles, however, may depend on the vendor ID, device ID, driver version, and other details of
  the device"*, which is precisely why the cache header carries those four fields
  ([Pipeline Cache Header](https://docs.vulkan.org/spec/latest/chapters/pipelines.html)).
* **The SPIR-V key still needs device identity, for a different reason**: §1.2 populates
  `TBuiltInResource` from `VkPhysicalDeviceLimits`. Two devices with different limits produce
  *different SPIR-V from the same source*. The honest formulation is therefore: device identity
  enters the SPIR-V key **because the resource table is device-derived**, not because the SPIR-V
  format is device-specific. A stricter and more correct alternative, if the table ever becomes
  a fixed constant, is to replace this field with a hash of the table actually used.

### 4.4 Toolchain version packing

```cpp
// glslang: <major><minor><patch><flavor> as a single u32, stable across builds.
const uint32_t kGlslangToolchainId =
      static_cast<uint32_t>(GLSLANG_VERSION_MAJOR) << 24
    | static_cast<uint32_t>(GLSLANG_VERSION_MINOR) << 16
    | static_cast<uint32_t>(GLSLANG_VERSION_PATCH) << 8
    | static_cast<uint32_t>(GLSLANG_VERSION_FLAVOR);   // 0 = release, 1 = snapshot

// SPIRV-Cross: the C API advertises exactly this triple.
const uint32_t kSpirvCrossToolchainId =
      static_cast<uint32_t>(SPVC_C_API_VERSION_MAJOR) << 16
    | static_cast<uint32_t>(SPVC_C_API_VERSION_MINOR) << 8
    | static_cast<uint32_t>(SPVC_C_API_VERSION_PATCH);
```

`SPVC_C_API_VERSION_*` come from `spirv_cross_c.h`; they are "bumped if ABI or API breaks", which
is exactly the compatibility we care about. `GLSLANG_VERSION_*` come from the generated
`glslang/build_info.h`.

### 4.5 On-disk framing

Hash the key with SHA-256 (use the streaming implementation MobileGlues already ships,
[`gl/glsl/cache.cpp`](https://github.com/MobileGL-Dev/MobileGlues/blob/main/MobileGlues-cpp/gl/glsl/cache.cpp) —
`Cache::computeSHA256`, which streams over the caller's buffer and only materialises the final
partial block). Do **not** put the key bytes in the filename; put the 32-byte digest and keep
the full key as the first field of the record so a hash collision is *detectable* rather than
silent.

Record layout, one file per backend-and-target so a corrupt file cannot poison the other:

```
file  := u32 record_count
         record*
record := u32 key_len, byte key[key_len],       // the full §4 key stream, not just the digest
          u32 payload_len, byte payload[payload_len]
```

`payload` is ESSL text (target `gl`) or SPIR-V words little-endian (target `vulkan`).

Persistence policy, taken from MobileGlues because it was arrived at the hard way
(`cache.cpp:125-175`):

* `count, then entries` blob — no single-record append path exists, so a save is a full rewrite.
* **Defer saves**: flush when 16 entries are pending **or** 5 s since the last save, whichever
  comes first, checked on every cache operation (not just inserts, so a run of hits still flushes
  a tail). Bounded loss: at most the 15 most recent entries, recompiled next run.
* **Write to `<path>.new` and `rename()` over the live file.** `rename(2)` is atomic against
  this process dying, which is the failure mode that matters; it promises nothing about power
  loss, which regenerable data does not need.
* On a short write: `remove()` the temp file and keep what was already in place.
* Byte size bound from `RendererConfig::shaderCacheSizeMb` (default 64,
  `renderer_config.h:60`), which is already plumbed into `ShaderManager::initialize()` at
  `shader_manager.cpp:46-53` and set from JNI at `jni/jni_bridge.cpp:151`.

---

## 5. The failure / fallback ladder

Five rungs, walked top to bottom. **Tiers 0-3 are safe by default. Tier 4 is not.**

| Tier | Name | What happens | Safe by default? |
|---|---|---|---|
| 0 | **Direct passthrough** | `isDirectShader()` says the source is already valid for the host ESSL version → hand it to `glCompileShader` untouched. No glslang, no rewrite, no cache lookup. | **Yes.** Produces byte-identical behaviour to today's GLES backend. |
| 1 | **Translate** | glslang → SPIR-V → SPIRV-Cross → ESSL at the requested target version. | **Yes.** The only path that produces correct output. |
| 2 | **Degraded target** | A **fixed, ordered** retry ladder inside tier 1. See the list below. Each step is recorded in `ShaderTranslationResult::info_log` and counted. | **Yes**, provided the ladder is finite and fixed (it is: ≤4 steps). |
| 3 | **Minimal built-in fallback** | A tiny compiled-in shader that is valid ESSL 300 *and* valid Vulkan GLSL. Draws a recognisable solid colour so a broken shader pack is obvious on screen instead of a black void. Built once, synchronously, in the `ShaderTranslator` constructor. | **Yes**, with mandatory loudness: one `LOGE` per failure, a counter, and **no** attempt to keep the failure quiet. |
| 4 | **Forced success** | Pass the *untranslated* source to the driver and report success even though it failed, or report success without compiling at all. | **No — debug only.** Gated behind a new `RendererConfig::allowForcedShaderSuccess`, default `false`, and additionally `#ifndef NDEBUG`-guarded. |

### 5.1 Tier 2 — the exact degradation ladder

Ordered, and each step only entered when the previous one failed:

1. **Requested ESSL version** (320 by default, `renderer_config.h` / config plumbing).
2. **320 → 310 → 300**. 300 is the floor because the GLES backend's documented minimum is
   "OpenGL ES 3.0+" (README §Requirements). `SPVC_COMPILER_OPTION_GLSL_VERSION` is clamped to
   `max(essl_version, 300)` — MobileGlues does exactly this
   (`glsl_for_es.cpp:820-821`) because anything below 300 is not ESSL 3.
3. **Rewrite pass** applied on the way down, in this order (each is a whole-source text pass, so
   `kRewritePassVersion` must be bumped whenever one changes):
   * strip `layout(binding = N)` / `layout(set = N)` — ESSL has neither
     (`removeLayoutBinding()`, `glsl_for_es.cpp:228-234`);
   * ensure a `precision highp float;` / `highp int;` preamble exists
     (`forceSupporterOutput()`, `glsl_for_es.cpp:171-226`);
   * normalise `#version`: absent → insert `#version 150`; desktop `< 140` →
     `#version 150 compatibility` (`get_or_add_glsl_version()`, `glsl_for_es.cpp:670-683`).
4. **Neutralise desktop-only extensions**: rewrite `#ifdef GL_ARB_derivative_control` /
   `#ifndef` into `#if 0` / `#if 1` and inject a `textureQueryLod` polyfill when
   `GL_EXT_texture_query_lod` is absent (`preprocess_glsl()`, `glsl_for_es.cpp:638-668`).

Every step is logged with the tier-2 index so a shader-pack bug report says
"needs ESSL 300 + rewrite pass 3", not "failed".

### 5.2 Tier 3 — the fallback shader

Two constants, one per target, compiled by the same tools at construction time so they cannot
drift from the toolchain:

```glsl
// ESSL 300 fragment shader — tier 3 for the GLES backend.
#version 300 es
precision mediump float;
out vec4 fragColor;
void main() { fragColor = vec4(1.0, 0.0, 1.0, 1.0); }   // unmistakable magenta
```

```glsl
// Vulkan GLSL 450 fragment shader — tier 3 for the Vulkan backend. Compiled to
// SPIR-V with setEnvClient(EShClientVulkan, EShTargetVulkan_1_1) at construction.
#version 450
layout(location = 0) out vec4 fragColor;
void main() { fragColor = vec4(1.0, 0.0, 1.0, 1.0); }
```

For the Vulkan backend the fallback must be compatible with the render pass `VulkanShaderManager`
binds: exactly one colour attachment, no depth (`make_color_blend_state()` and
`make_depth_stencil_state()` at `vulkan_shader_manager.cpp:252-280`), so a single
`location = 0` output and no `gl_FragDepth` write is sufficient.

If even the fallback cannot be built (glslang is broken), the result is `ok == false` and
`createShaderFromGLSL()` returns handle `0`. That is already the contract the rest of the code
expects: `ShaderManager::createGraphicsPipeline()` refuses dangling handles
(`shader_manager.cpp:135-142`). Do not invent a fake success here.

### 5.3 Why tier 4 is not safe by default — and the upstream counter-example

MobileGlues **does** ship forced success as the default. Its `glGetShaderiv` override does:

```cpp
if (global_settings.ignore_error >= IgnoreErrorLevel::Partial && pname == GL_COMPILE_STATUS && !*params) {
    LOG_W_FORCE("Shader %d compilation failed: %s", shader, log);
    LOG_W_FORCE("Now try to cheat.");
    *params = GL_TRUE;
}
```

(`gl/shader.cpp:110-121`; default `ignore_error = IgnoreErrorLevel::Partial` in
`config/settings.cpp:27`.)

For a launcher-facing renderer this default is wrong and we should say why explicitly, because
someone will otherwise "fix" it by copying upstream:

* A forced `GL_TRUE` means `glLinkProgram` proceeds on a shader that does not exist. The result is
  a program that links and draws **nothing** — or draws garbage — with no error anywhere. In
  Minecraft that is a black screen or a corrupted world, and the user has no way to report it.
* Copper Oxide already has a working counter-pattern: `VulkanShaderManager::onCreateShaderFromGLSL()`
  returns `false` and logs, with the comment "Returning a fake module would produce a pipeline
  that fails later (or, worse, silently draws nothing)" (`vulkan_shader_manager.cpp:492-496`).
  Keep that property.
* Tier 3 already covers the legitimate need (a broken third-party shader pack should show
  something diagnosable). Tier 4 covers only "the developer is debugging the translator", which is
  exactly what a debug-only flag is for.

### 5.4 Where the ladder lives

The ladder is `ShaderTranslator` logic, **not** `ShaderManager` logic. `ShaderManager` stays
backend-agnostic and keeps its two existing hooks; only the *content* passed to them changes.
See §7.

---

## 6. Async compilation

### 6.1 What can and cannot leave the render thread

| Work | Off the render thread? | Why |
|---|---|---|
| `isDirectShader()` | **No** | Needs the live GL context (`es_version_at_least()` calls `glGetIntegerv`). |
| Source rewrites (`#version`, defines, precision) | Yes | Pure text. |
| glslang parse + `GlslangToSpv` | Yes | Pure CPU, no driver, no context. |
| SPIRV-Cross → ESSL | Yes | Pure CPU. |
| `glShaderSource` / `glCompileShader` | **No** | GL calls need the context; the driver compile blocks (see §6.4). |
| `vkCreateShaderModule` | **No** | `VkDevice` objects require external synchronisation unless created with `VK_KHR_synchronization2`. |
| `vkCreateGraphicsPipelines` | **No** (but see §6.5) | Same, plus it is the real stall. |

So "async compilation" means: **translation is off-thread, driver compilation is not.** That is
exactly the split MobileGlues has — their `GLSLtoGLSLES_2()` runs the translation wherever it is
called, and the subsequent `glCompileShader` is still the driver's blocking call.

### 6.2 Thread pool

* **Translation pool: 2 workers.** Small and fixed rather than derived from the core count.
  Rationale: glslang is single-threaded per shader, so extra workers only buy overlap between
  independent shaders — they do not make one shader compile faster. Two is enough to keep a
  worker busy while the render thread waits on the GPU, without competing with the render thread
  for a core or with the platform compositor, which are the two things that actually hurt on a
  thermally-throttled phone. Expose it as `RendererConfig::shaderCompileThreads` (default 2,
  clamp 1..4) so a measurement can move it. Note `RendererConfig::thermalThrottlingAware`
  (`renderer_config.h:68`) already exists as the hook for dropping it under load.
* **Pipeline compiler pool: 1 worker**, started later (see `pipeline-cache.md` §5). ANGLE runs
  exactly one at a time: *"Currently, only one pipeline creation job is allowed at a time.
  Additionally, posting these jobs is rate limited."*
* Do not merge the two pools. Translation is CPU-bound and interruptible; pipeline creation enters
  the driver and is neither.

### 6.3 `glslang::InitializeProcess()`

The contract is explicit in the header
([`glslang/Public/ShaderLang.h`](https://github.com/KhronosGroup/glslang/blob/main/glslang/Public/ShaderLang.h)):

```cpp
// Call this exactly once per process before using anything else
GLSLANG_EXPORT bool InitializeProcess();
// Call once per process to tear down everything
GLSLANG_EXPORT void FinalizeProcess();
```

Requirements for this codebase:

1. **Run it once, on the render thread, inside `ShaderTranslator`'s constructor**, before the
   worker threads are started and before any `TShader` exists. A `TShader` constructed before
   `InitializeProcess()` is undefined behaviour, so "lazily on first use" only works if the
   lazy init is race-free — MobileGlues' `static bool glslang_inited`
   ([`glsl_for_es.cpp:841-850`](https://github.com/MobileGL-Dev/MobileGlues/blob/main/MobileGlues-cpp/gl/glsl/glsl_for_es.cpp#L841))
   is not: two threads can both see `false`. Use `std::call_once` if you keep lazy init; the
   constructor is simpler and gives a deterministic failure point.
2. `ShaderTranslator` is owned by whoever owns `ShaderManager`. Construct it in
   `ShaderManager::initialize()` (`shader_manager.cpp:46-53`) — which `VulkanRenderer::initializeManagers()`
   (`vulkan_renderer.cpp:906-909`) and the GLES manager constructor already call — and destroy it
   in `ShaderManager::shutdown()`.
3. **`FinalizeProcess()` runs in `~ShaderTranslator`, after the pool has been joined.** It is
   process-global teardown; calling it while a worker is mid-parse is a use-after-free. Never
   call it per-shader, and never call it on a `shut_down` path that can race a still-running job.
4. If a renderer is created and destroyed twice in one process (surface loss + recreate,
   `RendererRenderer::onSurfaceDestroyed()` at `vulkan_renderer.cpp:958-969`), the second
   `InitializeProcess()` is a no-op-or-worse. Guard the whole lifecycle with a process-level
   `std::once_flag` pair so only the first translator initialises and only the last finalises —
   or, simpler, make `ShaderTranslator` a function-local static that outlives every renderer.
5. `glslang` links `Threads::Threads` internally; nothing else is needed on the worker side.

**Fail loudly**: if `InitializeProcess()` returns `false`, disable tiers 1-4 permanently for this
process, log one error, and let tier 0 carry the frame. A renderer that silently translates
nothing is the bug this whole document exists to prevent.

### 6.4 How results reach the render thread

A completion queue drained at the top of the frame, never a callback executed on the worker:

```cpp
// renderer/common/shader_manager.h
class ShaderManager {
    ...
    // Called by backends from the top of onBeginFrame, before any command
    // recording. Vulkan: vulkan_renderer.cpp onBeginFrame (before vkWaitForFences,
    // :676). GLES: gles_renderer.cpp onBeginFrame, with the context current.
    void drainPendingCompilations();
};
```

Shape:

```cpp
struct PendingCompilation {
    uint64_t              job_id       = 0;
    std::string           cache_key;      // §4 key stream
    ShaderStage           stage;
    TranslationTarget     target;
    std::string           entry_point;
    std::function<void(uint64_t)> callback;   // invoked ON the render thread
};

// Impl:
//   std::mutex                          completion_mutex_;
//   std::deque<PendingCompilation>      completions_;     // MPSC
//   std::atomic<uint64_t>               next_job_id_{1};
//   std::unordered_map<uint64_t, std::shared_future<uint64_t>> in_flight_;  // dedup
```

Rules:

1. Worker threads do **only** translation (§6.1). They never call GL and never touch `VkDevice`.
2. A finished translation is moved into `completions_` with a `std::move`, no deep copy of the
   SPIR-V words beyond the one the result already owns.
3. `drainPendingCompilations()` is called at the very top of `onBeginFrame()`, **before**
   `vkWaitForFences` (`vulkan_renderer.cpp:688`) and before `fp_vkAcquireNextImageKHR` (`:693`),
   so the finished work is visible to the rest of the frame and the fence wait is pure wait.
4. Each completion runs `onCreateShader*` on the render thread (creating the `VkShaderModule` /
   the GL shader), publishes the handle with release semantics, then invokes the caller's
   `callback`. The callback therefore always runs on the render thread — document that, because
   existing callers will assume a thread.
5. `drainPendingCompilations()` must take **no** long-held lock. Take `completion_mutex_`, splice
   the deque into a local, release, then do the Vulkan/GL work outside the lock. Holding the base
   `ShaderManager` mutex across `vkCreateGraphicsPipelines` would serialise the entire manager
   against the render thread — and see the deadlock note at `shader_manager.cpp:206`.
6. **In-flight dedup is mandatory**, not an optimisation. `getOrCreateShader()`
   (`shader_manager.cpp:198-213`) deliberately drops the lock before calling `creator()`, so two
   threads missing the same key both create. Once translation runs off-thread, the render thread
   can miss on a key a worker is already translating and create a duplicate. The fix is the
   `in_flight_` map: `getOrCreateShader` publishes a `shared_future` under the lock, and the
   creator stores the handle through `std::promise` so both callers observe one result.
7. Cap the queue. A shader pack with 4000 programs must not enqueue 4000 driver compiles the
   instant a chunk is resident. Keep a bounded in-flight count (default 4) and refuse
   `compileAsync` beyond it, returning handle `0` — the caller already handles that
   (`createGraphicsPipeline` rejects handle 0, `shader_manager.cpp:139-142`).

### 6.5 Pipeline creation is the real stall — not module creation

Three independent pieces of evidence:

* ANGLE's own analysis: *"creating pipelines is a heavy operation (in particular, converting
  SPIR-V to assembly and optimizing it), and doing so at draw time has multiple draw backs,
  including visible hitching"*
  ([`PipelineCreation.md`](https://github.com/google/angle/blob/main/src/libANGLE/renderer/vulkan/doc/PipelineCreation.md)).
* ANGLE's flow doc is explicit about the split: *"At this time, `VkShaderModule`s are created
  (and cached). The appropriate specialization constants are then resolved and the `VkPipeline`
  object is created"* — i.e. the module is cheap and the pipeline is not
  ([`ShaderModuleCompilation.md`](https://github.com/google/angle/blob/main/src/libANGLE/renderer/vulkan/doc/ShaderModuleCompilation.md)).
* In this repo's own GLES backend: *"GL work runs unlocked: `glCompileShader()` runs driver code
  that can block for tens of milliseconds and may re-enter"*
  (`gles_shader_manager.cpp:360-361`).

**The Vulkan consequence.** The spec defines `vkCreateShaderModule` as taking SPIR-V and
returning a module — no compilation step, no machine code, no ISA selection. The SPIR-V → ISA
translation happens inside `vkCreateGraphicsPipelines`. So moving glslang off the render thread
removes ~0 ms of driver stall on the Vulkan path and **only** removes the CPU-side parse cost
(typically single-digit milliseconds per shader, and entirely eliminated on a cache hit). The
hitch the player feels is in `vkCreateGraphicsPipelines`, and the mitigation is the
rate-limited background compiler plus handle swap-in described in
[`pipeline-cache.md`](pipeline-cache.md) §5.

The corollary for scheduling: **do the translation off-thread, and keep the pipeline create
time-limited.** Budget it: at most one `vkCreateGraphicsPipelines` per frame at first, with the
remainder deferred. If a single pipeline create still exceeds ~4 ms, defer it — the next frame's
budget is no worse.

For measurement, prefer `VK_KHR_pipeline_executable_properties`
(`VkPipelineCreationFeedbackCreateInfo` + `vkGetPipelineExecutablePropertiesKHR`) when the driver
advertises it. Do **not** build the design on it: it is not in the Android 8 (API 26) baseline,
and `vkGetPhysicalDeviceProperties2` must be reachable through `vkEnumerateDeviceExtensionProperties`
first.

---

## 7. What changes in `shader_manager.cpp`

The base keeps its shape and its two backend hooks. Four changes, in dependency order.

### 7.1 `Impl` gains a translator, a queue and a job table

```diff
--- a/app/src/main/cpp/renderer/common/shader_manager.cpp
+++ b/app/src/main/cpp/renderer/common/shader_manager.cpp
@@
+#include "shader_translator.h"
+
+#include <atomic>
+#include <condition_variable>
+#include <deque>
+#include <future>
+#include <thread>
+
 namespace copper {

 class ShaderManager::Impl {
 public:
@@
     std::unordered_map<uint64_t, Shader> shaders;
     std::unordered_map<uint64_t, Pipeline> pipelines;
     std::unordered_map<std::string, uint64_t> shader_cache;
     std::unordered_map<std::string, uint64_t> pipeline_cache;
     uint64_t next_shader_handle = 1;
     uint64_t next_pipeline_handle = 1;
     std::mutex mutex;
     RendererBase* renderer = nullptr;
     size_t max_cache_size_mb = 64;
     size_t current_cache_size_mb = 0;
+
+    // --- shader translation (this document) -------------------------------
+    std::unique_ptr<ShaderTranslator> translator;
+
+    // Worker threads. Sized from RendererConfig::shaderCompileThreads (default 2).
+    // They call ONLY ShaderTranslator; never GL, never VkDevice.
+    std::vector<std::thread> workers;
+    std::mutex work_mutex;
+    std::condition_variable work_cv;
+    std::deque<Job> work_queue;
+    bool stopping = false;
+
+    // Results, moved here by workers and drained by drainPendingCompilations()
+    // at the top of onBeginFrame. No lock is held across driver calls.
+    std::mutex completion_mutex;
+    std::deque<PendingCompilation> completions;
+
+    // Dedup: one in-flight translation per cache key. getOrCreateShader() already
+    // drops the lock before calling creator() (see its comment at :206), so two
+    // threads missing the same key both created. That was latent while creation was
+    // synchronous; it becomes a real duplicate-module bug once creation is async.
+    std::unordered_map<std::string, std::shared_future<uint64_t>> in_flight;
+
+    size_t max_in_flight = 4;
 };
```

### 7.2 `initialize()` constructs the translator; `shutdown()` joins and finalises

```diff
 bool ShaderManager::initialize(RendererBase* renderer) {
     pImpl->renderer = renderer;
     if (renderer) {
         const auto& config = renderer->getConfig();
         pImpl->max_cache_size_mb = config.shaderCacheSizeMb;
+        // ShaderTranslator ctor calls glslang::InitializeProcess() exactly once
+        // and compiles the tier-3 fallback shaders. Both must happen before any
+        // worker starts and before any TShader exists.
+        pImpl->translator = ShaderTranslator::create(/*backend target*/ ...);
+        pImpl->max_in_flight = 4;
+        const unsigned n = clamp(config.shaderCompileThreads, 1u, 4u);
+        pImpl->workers.reserve(n);
+        for (unsigned i = 0; i < n; ++i) {
+            pImpl->workers.emplace_back([this] { worker_loop(); });
+        }
     }
     return true;
 }

 void ShaderManager::shutdown() {
+    // Join BEFORE the base lock is taken and before glslang::FinalizeProcess().
+    {
+        std::lock_guard<std::mutex> lock(pImpl->work_mutex);
+        pImpl->stopping = true;
+    }
+    pImpl->work_cv.notify_all();
+    for (auto& worker : pImpl->workers) {
+        if (worker.joinable()) worker.join();
+    }
+    pImpl->workers.clear();
+
     std::lock_guard<std::mutex> lock(pImpl->mutex);
     for (auto& [handle, pipeline] : pImpl->pipelines) {
         onDestroyPipeline(handle);
     }
@@
     pImpl->shader_cache.clear();
     pImpl->pipeline_cache.clear();
     pImpl->current_cache_size_mb = 0;
+    // Destructor runs glslang::FinalizeProcess(). Must be after the join above.
+    pImpl->translator.reset();
 }
```

### 7.3 `createShaderFromGLSL()` translates instead of delegating raw text

```diff
 uint64_t ShaderManager::createShaderFromGLSL(ShaderStage stage, const std::string& glsl_source,
                                              const std::string& entry_point,
                                              const std::vector<std::string>& defines) {
-    // In a real implementation, this would use glslang to compile GLSL to SPIR-V
-    // For now, return a placeholder
-    uint64_t handle = pImpl->next_shader_handle++;
-
-    Impl::Shader shader;
-    shader.handle = handle;
-    shader.stage = stage;
-    shader.entry_point = entry_point;
-
-    if (!onCreateShaderFromGLSL(handle, stage, glsl_source, entry_point, defines)) {
-        return 0;
-    }
-
-    pImpl->shaders[handle] = std::move(shader);
-    return handle;
+    ShaderTranslationRequest request;
+    request.stage   = stage;
+    request.target  = nativeTarget();                 // new virtual; GlslEs or SpirvVulkan
+    request.source  = glsl_source;
+    request.defines = defines;
+
+    const std::string key = pImpl->translator->buildCacheKey(request, ShaderSourceFormat::GlslSource);
+
+    ShaderTranslationResult result = translateSynchronously(request);
+    if (!result.ok) {
+        return 0;   // contract: callers treat 0 as failure (see createGraphicsPipeline)
+    }
+
+    std::lock_guard<std::mutex> lock(pImpl->mutex);
+    const uint64_t handle = pImpl->next_shader_handle++;
+
+    Impl::Shader shader;
+    shader.handle = handle;
+    shader.stage = stage;
+    shader.entry_point = entry_point;
+    shader.spirv = std::move(result.spirv);
+    // The source that was actually compiled, after the rewrite pass. Debug
+    // tooling and the on-disk ESSL cache want this, not the caller's original.
+    shader.compiled_source = std::move(result.text);
+    shader.translation_tier = result.tier;
+
+    if (result.spirv.empty()) {
+        if (!onCreateShaderFromGLSL(handle, stage, shader.compiled_source, entry_point, defines)) {
+            return 0;
+        }
+    } else {
+        if (!onCreateShader(handle, stage, shader.spirv, entry_point)) {
+            return 0;
+        }
+    }
+
+    pImpl->shaders[handle] = std::move(shader);
+    pImpl->current_cache_size_mb += result.bytes();
+    return handle;
 }
```

Supporting changes in the same commit:

* `ShaderManager` gains `virtual TranslationTarget nativeTarget() const` and
  `virtual uint32_t hostEsslVersion() const` (both pure-virtual would break the two existing
  backends, so give the base a `Unknown` default that makes `createShaderFromGLSL` return `0`).
* Two new `RendererConfig` fields are referenced above — `shaderCompileThreads` and
  `allowForcedShaderSuccess`. Adding one is a three-file change, not a one-liner:
  `renderer/common/renderer_config.h` (declaration + default),
  `renderer/common/renderer_config.cpp` (`clampToValidRanges()`,
  `validate()`, and the `Performance`/`BatterySaver`/`Debug` presets), and
  `jni/jni_bridge.cpp` (`nativeInitialize`'s fixed argument list at `:100-158` plus the
  `config.<field> = <arg>` block at `:129-158`). `CopperOxideRenderer.kt` is the Kotlin side.
* `Impl::Shader` gains `std::string compiled_source;` and
  `TranslationTier translation_tier;`. It already has
  `std::unordered_map<std::string,uint32_t> specialization_constants` at `:19` — that map is
  **write-only** (nothing reads it; the backend keeps its own copy at
  `vulkan_shader_manager.cpp:123`). Delete one of the two during this change.
* `Impl::Shader::spirv` exists but is only filled by `createShader()` (`:75`). This change makes
  it authoritative for the GLSL path too, which is what lets the tier-3 fallback and the
  SPIR-V cache work without a second entry point.
* `current_cache_size_mb` (`:40`) is declared and never updated. Start updating it here, and
  enforce `max_cache_size_mb` with an LRU in §7.5.

### 7.4 `compileAsync()` becomes a real job

```diff
 void ShaderManager::compileAsync(const std::string& key, ShaderStage stage,
                                  const std::string& source,
                                  std::function<void(uint64_t)> callback) {
-    // Compilation currently runs inline. Callers must treat this as blocking:
-    // a background pool is wired up with the real shader translation backend.
-    const uint64_t handle = getOrCreateShader(
-        key, [this, stage, &source]() { return createShaderFromGLSL(stage, source, "main", {}); });
-    if (callback) {
-        callback(handle);
-    }
+    // NOTE the existing bug this removes: the lambda captured `source` by
+    // reference and was invoked inline. Handed to a worker it would be a
+    // dangling reference on return.
+    std::string owned_source = source;
+
+    {
+        std::lock_guard<std::mutex> lock(pImpl->mutex);
+        const auto cached = pImpl->shader_cache.find(key);
+        if (cached != pImpl->shader_cache.end()) {
+            if (callback) callback(cached->second);
+            return;
+        }
+        // Deduplicate against a translation already in flight for this key.
+        const auto existing = pImpl->in_flight.find(key);
+        if (existing != pImpl->in_flight.end()) {
+            // Attach to the running job; its own completion will invoke both.
+            if (callback) pImpl->waiters[key].push_back(std::move(callback));
+            return;
+        }
+        if (pImpl->in_flight.size() >= pImpl->max_in_flight) {
+            LOGW("compileAsync(%s): %zu jobs already in flight, returning 0",
+                 key.c_str(), pImpl->in_flight.size());
+            if (callback) callback(0);
+            return;
+        }
+    }
+
+    PendingCompilation job;
+    job.cache_key = key;
+    job.stage     = stage;
+    job.entry_point = "main";
+    job.target    = nativeTarget();
+    job.source    = std::move(owned_source);
+    job.job_id    = pImpl->next_job_id_.fetch_add(1, std::memory_order_relaxed);
+    job.callback  = std::move(callback);
+
+    {
+        std::lock_guard<std::mutex> lock(pImpl->work_mutex);
+        pImpl->work_queue.push_back(std::move(job));
+    }
+    pImpl->work_cv.notify_one();
 }
+
+void ShaderManager::drainPendingCompilations() {
+    // Called from the top of onBeginFrame, before any command recording.
+    std::deque<PendingCompilation> ready;
+    {
+        std::lock_guard<std::mutex> lock(pImpl->completion_mutex);
+        ready.swap(pImpl->completions);
+    }
+    for (auto& job : ready) {
+        // Driver calls happen HERE, on the render thread, outside every lock.
+        const uint64_t handle = createShaderFromGLSL(job.stage, job.source,
+                                                     job.entry_point, {});
+        {
+            std::lock_guard<std::mutex> lock(pImpl->mutex);
+            if (handle != 0) pImpl->shader_cache.emplace(job.cache_key, handle);
+            pImpl->in_flight.erase(job.cache_key);
+            for (auto& waiter : pImpl->waiters[job.cache_key]) waiter(handle);
+            pImpl->waiters.erase(job.cache_key);
+        }
+        if (job.callback) job.callback(handle);
+    }
+}
```

`worker_loop()` is the mirror image: pop, `translator->compileGLSLtoSPIRV` (+ SPIRV-Cross for the
`GlslEs` target), move the result into `completions` under `completion_mutex`, notify nothing.
It never acquires `pImpl->mutex`.

### 7.5 Fix the cache bookkeeping while we are here

Two concrete defects in the existing cache code, both surfaced by making compilation concurrent:

* `destroyShader()` scans `shader_cache` and erases only the **first** entry whose value is the
  handle, then `break`s (`shader_manager.cpp:125-130`). Same in `destroyPipeline()` (`:188-193`).
  `getOrCreateShader`/`getOrCreatePipeline` allow the same handle to be filed under more than one
  key, so one of those keys is left dangling and a later lookup returns a destroyed handle. Erase
  **all** matching entries, or index by handle in the first place.
* `getOrCreateShader()`/`getOrCreatePipeline()` are not atomic (§6.4 rule 6). While here, replace
  the "miss → unlock → create → lock → emplace" shape with a per-key promise so exactly one
  creator runs.

And `max_cache_size_mb` / `current_cache_size_mb` need an actual LRU; the disk-cache size policy
in §4.5 and the in-memory bound should share one accounting so the two cannot disagree.

---

## 8. Backend-side changes

### `renderer/vulkan/vulkan_shader_manager.cpp`

`onCreateShaderFromGLSL()` (`:488-513`) stops being a `return false`. It becomes the consumer of
a pre-translated artifact:

```cpp
bool VulkanShaderManager::onCreateShaderFromGLSL(uint64_t handle, ShaderStage stage,
                                                 const std::string& glsl_source,
                                                 const std::string& entry_point,
                                                 const std::vector<std::string>& defines) {
+   // ShaderManager::createShaderFromGLSL now hands us the artifact that the
+   // translator produced for THIS device (SPIR-V words), not raw GLSL.
+   // createShader() already validates the payload: non-empty, word count a
+   // multiple of 4, and the 0x07230203 magic (see :451-466).
+   const std::vector<uint32_t> spirv = spirv_for_handle(handle);
+   if (spirv.empty()) {
+       LOGE("onCreateShaderFromGLSL(%llu): no translated SPIR-V", ...);
+       return false;
+   }
+   return onCreateShader(handle, stage, spirv, entry_point);
}
```

Also in this file:

* Install a real `SpecializationConstantIdResolver` built on SPIRV-Cross reflection. The
  insertion-order fallback at `:87-116` is a **silent wrong-rendering** bug, not a warning —
  see [`pipeline-cache.md`](pipeline-cache.md) §4.4.
* `onCreateGraphicsPipeline` passes `renderer_->pipelineCache()`, which is `VK_NULL_HANDLE`
  today. See [`pipeline-cache.md`](pipeline-cache.md) §2.

### `renderer/gles/gles_shader_manager.cpp`

`onCreateShaderFromGLSL()` (`:324-391`) keeps its structure — it already does the right thing
(`glShaderSource` + `glCompileShader` + honest `false` on `GL_COMPILE_STATUS`) — and gains one
branch in front of it:

```cpp
    // Tier 0: the source is already valid for this context. Compile it verbatim.
    if (translator_ && translator_->isDirectShader(glsl_source, host_essl_version_)) {
        /* ...existing glShaderSource + glCompileShader path, unchanged... */
    }
    // Otherwise the translator (tier 1/2/3) hands us ESSL text and this same
    // driver path consumes it. The driver still does the final compile -- we
    // have not made driver compilation async, only the GLSL->ESSL part.
```

`inject_defines()` (`:137-174`) moves *after* the translator, because the translator needs the
defines folded into the source before hashing (§4.2 field 3) and before the rewrite pass. Keep
the function where it is; just call it on the translated text.

### `renderer/vulkan/vulkan_renderer.cpp` / `gles_renderer.cpp`

Add one line each, at the top of `onBeginFrame()`:

* Vulkan, before `vkWaitForFences` at `vulkan_renderer.cpp:688`:
  `if (auto* shaders = getShaderManager()) shaders->drainPendingCompilations();`
* GLES, in `GLESCRenderer::onBeginFrame()` with the EGL context already current
  (`gles_renderer.cpp` — the manager's `requireContext()` at
  `gles_shader_manager.cpp:338` assumes it).

---

## 9. Risks / open questions

| Risk | Mitigation |
|---|---|
| glslang adds ~1.5–3 MB to `libcopper-oxide.so`; the release build is minified (`app/build.gradle.kts`) but native code is not stripped by R8. | Measure the delta in the first PR and report it. `ENABLE_OPT`/`ENABLE_HLSL`/the five `SPIRV_CROSS_ENABLE_*=OFF` are already the APK-size work; do not add SPIRV-Tools back "just for spirv-val". |
| glslang/SPIRV-Cross build time in CI. | The `native-build` CI job runs a full CMake configure+build; expect it to grow. Budget for it; do not disable the job. |
| A zeroed `TBuiltInResource` field reintroduces the MobileGlues bug (§1.2). | The allow-list unit test. This is a test, not a comment. |
| Two translator instances in one process (surface recreate) double-`FinalizeProcess`. | §6.3 rule 4: process-level `once_flag`, or a function-local static translator. |
| Vulkan-path translation buys little (driver does the real work). | Expected and stated up front (§6.5). The Vulkan win is correctness (any GLSL compiles at all) plus the cached `GlslangToSpv` offload, not stall removal. Stall removal is `pipeline-cache.md`. |

---

## 10. Sources

Upstream facts asserted in this document, with the URL that establishes each:

* MobileGlues option set and link targets — <https://github.com/MobileGL-Dev/MobileGlues/blob/main/MobileGlues-cpp/CMakeLists.txt>
* glslang usage, `EShClientVulkan` input / `EShClientOpenGL` client, `#undef VULKAN` preamble — <https://github.com/MobileGL-Dev/MobileGlues/blob/main/MobileGlues-cpp/gl/glsl/glsl_for_es.cpp>
* `TBuiltInResource` must be complete (`maxDualSourceDrawBuffersEXT` incident) — same file, `InitResources()` comments
* `GLSL_ES` + `GLSL_VERSION` must both be set and checked — same file, `spirv_to_essl()`
* `SPVC_CAPTURE_MODE_TAKE_OWNERSHIP` + context-owned output — same file, `spirv_to_essl()`
* Direct-passthrough test — <https://github.com/MobileGL-Dev/MobileGlues/blob/main/MobileGlues-cpp/gl/shader.cpp> (`is_direct_shader`, `can_run_essl3`)
* Forced-success tier and its upstream default — same file (`glGetShaderiv` override) and <https://github.com/MobileGL-Dev/MobileGlues/blob/main/MobileGlues-cpp/config/settings.cpp>
* Deferred-save / temp-file+rename cache policy — <https://github.com/MobileGL-Dev/MobileGlues/blob/main/MobileGlues-cpp/gl/glsl/cache.cpp>
* glslang option semantics and the `ENABLE_OPT` `SEND_ERROR` — <https://github.com/KhronosGroup/glslang/blob/main/CMakeLists.txt>
* `glslang::InitializeProcess` / `FinalizeProcess` contract, `setEnvInput/setEnvClient/setEnvTarget` — <https://github.com/KhronosGroup/glslang/blob/main/glslang/Public/ShaderLang.h>
* SPIRV-Cross option names, `spirv-cross-c` target, CLI's all-backends `FATAL_ERROR` — <https://github.com/KhronosGroup/SPIRV-Cross/blob/master/CMakeLists.txt>
* `SPVC_COMPILER_OPTION_GLSL_ES` / `GLSL_VERSION`, `SPVC_C_API_VERSION_*`, `spvc_compiler_get_specialization_constants` — <https://github.com/KhronosGroup/SPIRV-Cross/blob/master/spirv_cross_c.h>
* Pipeline compile cost, `VK_EXT_graphics_pipeline_library` split, rate-limited monolithic swap-in — <https://github.com/google/angle/blob/main/src/libANGLE/renderer/vulkan/doc/PipelineCreation.md>
* Module-vs-pipeline creation split; pre-rotation applied via specialization-constant-conditional code — <https://github.com/google/angle/blob/main/src/libANGLE/renderer/vulkan/doc/ShaderModuleCompilation.md>
* `VkPipelineCacheHeaderVersionOne` layout and "results of pipeline compiles may depend on the vendor ID, device ID, driver version" — <https://docs.vulkan.org/spec/latest/chapters/pipelines.html>
* A `VkPipelineCache` is internally synchronised and usable from multiple threads — same page, "Creating a Pipeline Cache"
* A wrong `constantID` in `VkSpecializationMapEntry` is silently ignored — same page, "Specialization Constants"