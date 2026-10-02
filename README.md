# Copper Oxide Renderer

A rendering-library project for running **Minecraft: Java Edition** on Android. Copper Oxide is
not a launcher, not a mod loader, and not a finished client — it is a C++/Kotlin rendering
backbone (Vulkan and OpenGL ES) with a JNI surface, plus a small diagnostics app that drives it.

**Current state: scaffolding, not a working Minecraft renderer.** The repository contains no
Minecraft client, no asset/texture pipeline, no shader compiler, no GL interception layer, and
none of the mod-loader integration that real Minecraft-on-Android requires. Both backends bring
up a device and can clear/present a frame; neither can render the game. See
[Compatibility](#compatibility) for the item-by-item truth and
[`docs/compatibility.md`](docs/compatibility.md) for the engineering checklist that would have to
be completed.

## Project identity

| | |
|---|---|
| Target | Minecraft: Java Edition (desktop client rendering path) |
| Form factor | Rendering library + JNI bridge; a launcher's UI and JVM are not part of this project |
| Launcher independence | Yes — Copper Oxide contains no launcher code and does not bundle one |
| Author | oxide-mc |
| License | MIT ([LICENSE](LICENSE), "Copyright (c) 2026 oxide-mc") |
| Platforms | Android only (`minSdk 26`, API 26+) |
| ABIs | `arm64-v8a` (release), `arm64-v8a` + `x86_64` (debug, for CI emulators) |

### Third-party and licensing note

- **Copper Oxide** is MIT. That covers only this repository's own code.
- **[VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)**
  (v3.3.0) is fetched at CMake configure time and compiled into `libcopper-oxide.so`. VMA is MIT.
- **[Mesa/Zink](https://github.com/mesa3d/mesa)** (MIT),
  **[ANGLE](https://github.com/google/angle)** (BSD-3-Clause),
  **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)** (Apache-2.0) and
  **[glslang](https://github.com/KhronosGroup/glslang)** (BSD-3-Clause / Apache-2.0) are
  referenced as prior art. None of them is vendored here yet, and none of their code is included.
- **[gl4es](https://github.com/gl4es/gl4es)**, **[PojavLauncher](https://github.com/PojavLauncher/PojavLauncher)**
  and **[Zalith](https://github.com/KennnyProject/Zalith)** are GPL-3.0. Copper Oxide has taken
  **no code** from them. If any of their code is ever incorporated, the combined work must be
  GPL-3.0, which is incompatible with this project's MIT license. Ideas alone carry no licence
  obligation.
- **[MobileGL / MobileGlues](https://github.com/MobileGL/MobileGL)** is LGPL-2.1/LGPL-3.0 — also
  referenced only as prior art.
- **Minecraft's assets, textures and shader files are proprietary and are not covered by this
  project's MIT license.** Do not redistribute them. A Minecraft launcher must fetch them at
  runtime from the user's own account.
- **Sodium is licensed under Polyform Shield 1.0.0, not an OSI-approved licence**
  ([Sodium LICENSE.md](https://github.com/CaffeineMC/sodium/blob/dev/LICENSE.md)); **Iris is
  LGPL-3.0** ([Iris LICENSE](https://github.com/IrisShaders/Iris/blob/26.1/LICENSE)). Neither can
  be absorbed into an MIT project. Compatibility with them can only ever mean *cooperating with*
  separately-distributed mods, never vendoring them.

## Compatibility

Legend: **Done** = implemented and exercised · **Partial** = some of it works · **Source only** =
code exists on disk but is not in `CMakeLists.txt`, so it does not compile · **No** = not
implemented · **n/a** = no applicable.

This repository cannot render Minecraft on either backend, so every game-facing row is **No**.
The "Why" column gives the specific technical blocker.

| Capability | OpenGL ES backend | Vulkan backend | Why |
|---|---|---|---|
| Context / surface bring-up | Done | Done | `gles_renderer.cpp` (`init_egl`, `create_egl_context`); `vulkan_renderer.cpp` (`create_instance` … `create_swapchain`) |
| GPU identification & limit query | Partial | Partial | GLES reads `glGetString`/`glGetIntegerv`; Vulkan reads `VkPhysicalDeviceProperties`. Neither feeds real values into `supportsFeature()`/`isExtensionSupported()` on Vulkan |
| Frame pacing loop (begin/end/present) | Done | Done | Kotlin `startRenderLoop()` in `CopperOxideRenderer.kt` |
| **Vanilla Minecraft rendering** | **No** | **No** | `initializeManagers()` on both backends returns `true` without instantiating any manager (`gles_renderer.cpp:586`, `vulkan_renderer.cpp:906`); there is no draw submission anywhere in the compiled sources, and Vulkan's per-frame work is a single `vkCmdClearAttachments` (`vulkan_renderer.cpp:743`) |
| **Mod loaders (Fabric / Forge / NeoForge / Quilt)** | **No** | **No** | No loader detection, class-loading or mixin infrastructure exists in this repository |
| **Resource packs (textures, models, animations)** | **No** | **No** | No image decoders, atlas builder or resource-pack format handling |
| **Sodium** | **No** | **No** | Sodium requires up-to-date OpenGL 4.5-class drivers and states that GL translation layers "are not supported and will very likely not work" ([Sodium README, *Hardware Compatibility*](https://github.com/CaffeineMC/sodium#-hardware-compatibility); [issue #1916, "Pojav Launcher is not supported"](https://github.com/CaffeineMC/sodium/issues/1916), which names Zink as the sole exception). Copper Oxide's GLES path is exactly such a layer, and it also does not implement the entry points Sodium needs — see [`docs/compatibility.md`](docs/compatibility.md#sodium-hard-gates) |
| **Iris / shader packs** | **No** | **No** | Iris lists "Mobile devices (PojavLauncher, etc)" as ❌ *Not supported* ([Iris `docs/usage/drivers.md`](https://github.com/IrisShaders/Iris/blob/master/docs/usage/drivers.md)): Android ES drivers have "huge amounts of bugs" and some OpenGL features "cannot be clearly translated to OpenGL ES". Iris also requires OpenGL 4.5 / ARB-DSA-class features that ES 3.2 does not have. There is additionally **no shader compiler in this repo**, so nothing can be translated |
| **Shader compilation (GLSL → SPIR-V / → ESSL)** | **No** | **No** | **No SPIR-V compiler is linked** (`CMakeLists.txt` fetches only VMA). `ShaderManager::createShaderFromGLSL` is a placeholder (`shader_manager.cpp:88-105`) and `compileAsync()` runs inline (`shader_manager.cpp:258`). The GLES shader manager hand-writes source straight to `glShaderSource` after inserting a precision preamble — that is not a GLSL→ESSL translator, and there is no disk cache |
| **GL interception / translation layer** | **No** | n/a | There is no `libGLESv2`/`gl4es`-style interception, no `GLESWrapper`, no symbol rewriting. The only `eglGetProcAddress` uses are for optional backend extensions. This is the single largest missing piece for making any of the rows above possible |
| **Mojang `GpuDevice` / `CommandEncoder` / `RenderPass` integration** | **No** | **No** | Since ~1.21.6 the client renders through `GpuDevice`/`CommandEncoder`/`RenderPass`/`RenderPipeline`; Sodium and Iris mixin into `GlCommandEncoder`, `GlStateManager` and `GlRenderPass`. Copper Oxide implements none of these types |
| Texture / buffer / framebuffer/state/sync managers | Source only | Source only | Per-backend `.cpp` files exist under `renderer/gles/` and `renderer/vulkan/`, **but none are listed in `CMakeLists.txt`**, so none compile into `libcopper-oxide.so`. `initializeManagers()` on both backends still returns `true` without constructing anything (`gles_renderer.cpp:586`, `vulkan_renderer.cpp:906`), and duplicate class declarations collide if the files *are* added — see below |
| Resource pool, profiler, state cache, command recorder | Source only | Source only | Base implementations carry the handle/caching/command-list logic (`command_buffer.cpp`); the per-backend replay implementations are likewise not in the build |

### What is explicitly *not* claimed

- No benchmark numbers exist. The `benchmark` CI job is a placeholder (`echo "Benchmark job - would run on device farm"`).
- No device has been validated. The `instrumented-tests` CI job runs the smoke test on a
  **SwiftShader**-accelerated x86_64 emulator; the smoke test uses `Assume.assumeTrue(...)` to
  *skip* rather than fail when no GPU backend initialises (`RendererSmokeTest.kt:63`). It asserts
  only that a library loads and that frames advance.
- There is no published artifact. The README previously advertised
  `com.oxide.mc:copper-oxide:1.0.0` as a Maven coordinate; no publishing task exists and no such
  coordinate is available. The Gradle module is `com.android.application` (an APK), not
  `com.android.library`, so there is no AAR.

## Architecture

```
Kotlin API            app/src/main/java/com/oxide/mc/copperoxide/
                      renderer/CopperOxideRenderer.kt   lifecycle, config, render loop,
                                                        stats, GPU queries (JNI externs)
                      gui/MainActivity.kt               diagnostics-only Compose UI
                             |  JNI
                             v
JNI bridge             app/src/main/cpp/jni/jni_bridge.{h,cpp}
                      global RendererBase instance, backend selection, stat/feature queries
                             |
                             v
RendererBase           app/src/main/cpp/renderer/common/renderer_base.{h,cpp}
                      frame loop, config, enums (RendererBackend/RendererFeature/GPUVendor),
                      RendererRegistry + COPPER_REGISTER_RENDERER  (registry is never populated)
                             |
             +---------------+---------------+
             v                               v
Vulkan backend                     OpenGL ES backend
renderer/vulkan/vulkan_renderer.{h,cpp}  renderer/gles/gles_renderer.{h,cpp}
  instance / device / swapchain /       EGL display/context/surface, gl32 header,
  render pass / clear+submit+present    capability query, workarounds, flush+swap
  vma_impl.cpp                         (no GL interception, no draw submission)
             |                               |
             +---------------+---------------+
                             v
Manager layer          renderer/common/          (base implementations — these DO build)
                       buffer_manager.*  texture_manager.*  shader_manager.*
                       framebuffer_manager.*  state_manager.*  command_buffer.*
                       sync_manager.*  resource_pool.*  profiler.*
                       gpu_capabilities.*  renderer_config.*
                       renderer/gles/gles_*.{h,cpp}      renderer/vulkan/vulkan_*.{h,cpp}
                       ^ per-backend implementations exist as source but are NOT in
                         CMakeLists.txt, so they are not compiled. Fix the duplicate
                         declarations below before adding them.
                             |
                             v
Platform layer         platform/android/android_platform.{h,cpp}
                      AssetManager + thermal service handles, window helpers
                      (several accessors are stubs returning constants)
```

File-by-file facts worth knowing before reading the code:

- **Nothing under `renderer/gles/` or `renderer/vulkan/` except `gles_renderer.cpp`,
  `vulkan_renderer.cpp` and `vma_impl.cpp` is in the build.** `CMakeLists.txt` lists 17 `.cpp`
  files; the per-backend manager implementations exist on disk but are absent from it.
- Adding them requires resolving duplicate class declarations first:
  - Vulkan: `VulkanShaderManager` is declared inline in `vulkan_renderer.h:275-313` *and* defined
    in `vulkan_shader_manager.h`. That header's own `INTEGRATOR NOTE`
    (`vulkan_shader_manager.h:53-58`) says the stub "must be deleted from `vulkan_renderer.h`".
    Same pattern for `VulkanBufferManager` (`:203`), `VulkanFramebufferManager` (`:316`) and
    `VulkanStateManager` (`:317`).
  - GLES: `GLESCFramebufferManager` is declared as an empty stub in `gles_renderer.h:271` and
    defined in `gles_framebuffer_manager.h:27`; `GLESCStateManager` likewise
    (`gles_renderer.h:272` vs `gles_state_manager.h:39`).
  - The GLES shader manager is named `GLESShaderManager` (`gles_shader_manager.h:38`) while
    `gles_renderer.h:232` declares `GLESCShaderManager` — the two spellings never meet, so
    neither can be the type the renderer stores.
- `VulkanRenderer::create_allocator`, `create_descriptor_pool` and `create_pipeline_cache` are
  declared but have **no definitions** and are never called, so VMA is linked but never
  initialised and no descriptor pool exists.
- `VulkanRenderer::apply_driver_workarounds()` builds a `GPUCapabilities` object and discards it.
- `GPUCapabilities::queryGLESProperties()` calls `glGetString` without a current GL context, so its
  GL fallback detection returns null strings.
- `CopperOxideRenderer.nativeResetFrameStats` is declared as an `external` Kotlin function but has
  no JNI implementation; calling it throws `UnsatisfiedLinkError`.
- `AndroidManifest.xml` declares no `<uses-feature>` for `glEsVersion` or a Vulkan version.

> **Note on repository state.** The per-backend manager sources listed above were added
> concurrently with this documentation and are not yet wired into the build. Everything marked
> *Source only* reflects that. No claim here depends on them.

## Building

### Prerequisites

| Tool | Version | Source |
|---|---|---|
| JDK | 17 | `app/build.gradle.kts` (`jvmTarget`, `compileOptions`) |
| Android Gradle Plugin | 8.5.0 | `build.gradle.kts` |
| Gradle | 8.7 | `gradle/wrapper/gradle-wrapper.properties` |
| Kotlin | 1.9.23 | `build.gradle.kts` |
| Android SDK | compile/target 35, build-tools 35.0.0 | `gradle.properties` |
| Android NDK | 27.2.12479018 (r27b) | `app/build.gradle.kts` (`ndkVersion`) |
| CMake | 3.22.1 | `app/build.gradle.kts` (`externalNativeBuild.cmake.version`) |
| Compose compiler ext. | 1.5.11 | `gradle.properties` |

The Gradle wrapper fetches Gradle 8.7 automatically; AGP downloads the NDK/CMake packages named
above when they are missing.

### Build commands

```bash
chmod +x gradlew

# Unit tests + formatting/lint, as CI runs them
./gradlew test
./gradlew spotlessApply spotlessCheck
./gradlew detekt
./gradlew lint

# APKs (AGP drives the CMake build itself)
./gradlew assembleDebug      # arm64-v8a + x86_64
./gradlew assembleRelease    # arm64-v8a, minified + resource shrinking
./gradlew bundleRelease      # AAB
```

### Native-only build (what CI's `native-build` job does)

```bash
cd app/src/main/cpp
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_STANDARD=20
cmake --build build -j"$(nproc)"
```

This produces `app/src/main/jniLibs/arm64-v8a/libcopper-oxide.so` (the path is set by
`LIBRARY_OUTPUT_DIRECTORY` in `CMakeLists.txt`; `app/src/main/jniLibs/` is gitignored). It links
`log`, `android`, `EGL`, `GLESv3` and `vulkan` from the NDK, and fetches
**VulkanMemoryAllocator v3.3.0** via `FetchContent` at configure time — the first configure needs
network access.

### Shader compilation

**There is no shader compiler in this build.** No glslang, shaderc, SPIRV-Tools or SPIRV-Cross is
fetched or linked. `ShaderManager::createShaderFromGLSL()` in the base is a stub
(`shader_manager.cpp:88`) that delegates to a backend hook, and
`VulkanShaderManager::onCreateShaderFromGLSL()` fails explicitly for the same reason
(`vulkan_shader_manager.cpp:492-504`). `compileAsync()` runs inline (`shader_manager.cpp:258`).

The GLES shader manager (source present, not compiled) takes a different approach: it inserts a
precision preamble after any existing `#version` line and hands the result to `glShaderSource` on
the device's own driver. That means GLSL it accepts is whatever the Android ESSL compiler accepts
— it is **not** a GLSL→ESSL translator, does no cross-stage or desktop-construct rewriting, and
has no disk cache. SPIR-V ingestion is also device-dependent there: `createShader()` reportedly
fails without ES 3.1 plus `GL_OES_gl_spirv`/`GL_ARB_gl_spirv` rather than pretending to compile.

Building a real toolchain (glslang for GLSL→SPIR-V, SPIRV-Cross for SPIR-V→ESSL) is prerequisite
work, not a configuration step.

### CI/CD

`.github/workflows/ci-cd.yml` runs on pushes to `main`, on `v*` tags, and on pull requests:
`lint` (spotless + detekt + Android Lint) → `unit-tests` → `native-build` (CMake + Ninja for
`arm64-v8a`, platform android-26) → `android-build` (release **and** debug matrix) →
`instrumented-tests` (API 33 x86_64 emulator, `-gpu swiftshader_indirect`) → `security`
(Trivy fs scan, OWASP Dependency-Check) → `release` on tags. The `benchmark` job runs only on
`schedule`/`workflow_dispatch` and is a stub.

## Using the renderer

The module is an **application**, so the supported way to run it today is to build and install the
APK and look at the diagnostics screen. `MainActivity` is documented in-source as existing "only to
give the renderer a surface and expose its diagnostics".

```kotlin
// app/src/main/java/com/oxide/mc/copperoxide/gui/MainActivity.kt (abridged)
val renderer = CopperOxideRenderer(context, RendererConfig.Default)
if (renderer.initialize(surfaceHolder.surface)) {
    Log.i("MC", "backend=${renderer.currentBackend()}")
    renderer.setFrameCallback { stats -> Log.i("MC", "fps=${stats.fps}") }
}
```

A launcher's `onRenderFrame()` hook exists but is empty by default
(`CopperOxideRenderer.onRenderFrame()`), and the backend it would submit work to has no draw path.

Note that `RendererRegistry::instance().create_renderer(...)` is **not** the way to obtain a
renderer: nothing calls `COPPER_REGISTER_RENDERER`, so it returns `nullptr`. Backends are
constructed directly in `jni_bridge.cpp` (`std::make_unique<VulkanRenderer>()` /
`GLESCRenderer()`, with a Vulkan→GLES fallback when `preferredBackend == Auto`).

### Configuration

`RendererConfig` (`CopperOxideRenderer.kt:411`) carries ~30 fields; presets are `Default`,
`Performance`, `BatterySaver`, `Debug`. All of them are forwarded to `RendererConfig` in C++ and
stored. **Most currently have no consumer**: there is no multithreaded recording (the Kotlin loop
runs on a single coroutine on `Dispatchers.Default`), no async compilation, no draw-call
batching, no render-scale management, no low-latency or battery mode. Fields that do affect
behaviour: `vsyncEnabled`, `targetFps` (Kotlin-side pacing), `maxFramesInFlight`
(clamped to 1–3, `vulkan_renderer.cpp:636`), `frameTimeoutMs`, `enableValidation` and
`enableDebugMarkers`.

`supportsFeature()` and `isExtensionSupported()` are honest only on the GLES backend. On Vulkan,
`query_gpu_info()` never populates `gpu_info_.extensions` and `supported_features_` is never set,
so both always return `false`.

## Requirements

- Android 8.0+ (API 26)
- `arm64-v8a` for release builds; `x86_64` is built for debug only
- GPU: Vulkan 1.1 (`VkApplicationInfo.apiVersion` is `VK_API_VERSION_1_1`) **or** an OpenGL ES 3.0
  context (`EGL_CONTEXT_CLIENT_VERSION = 3`). Note the GLES sources include `<GLES3/gl32.h>` and
  query ES 3.2-only enums such as `GL_MAJOR_VERSION`/`GL_MINOR_VERSION`, but the context actually
  requested is ES 3.0 and `GLESv3` (not `GLESv3_2`) is the library linked — so the ES 3.2 feature
  set is detected, not guaranteed.
- Any Vulkan device passes `select_physical_device()`: the feature check is a stub
  (`bool suitable = true; // ... feature checks`, `vulkan_renderer.cpp:305-306`), and
  `create_logical_device()` enables **no** `VkPhysicalDeviceFeatures`.

## GPU identification

`GPUVendor`/`GPUArchitecture` are derived from `GL_VENDOR`+`GL_RENDERER` or from
`VkPhysicalDeviceProperties::vendorID`/`deviceName` by substring matching
(`gles_renderer.cpp:301-358`, `gpu_capabilities.cpp:154-238`).
Adreno/Mali/PowerVR are recognised; NVIDIA, AMD, Intel, Broadcom, Vivante, VeriSilicon and Apple
exist in the enums but are only matched on the GLES string path and default to
`GPUArchitecture::Unknown`.

`GPUCapabilities::buildOptimizationConfig()` produces advisory flags per vendor (UBWC, AFBC,
descriptor indexing, timeline semaphores, dynamic rendering, workgroup size). **None of them
changes rendering behaviour.** The GLES backend reads two of them back to set feature bits; nothing
else consumes the struct. There is therefore no GPU support matrix to publish — treat every device
as untested (see [Requirements](#requirements)).

## Roadmap

Roughly in dependency order; none of this is implemented.

- [ ] Compile the manager layer: delete the duplicate manager class declarations from
      `vulkan_renderer.h` / `gles_renderer.h`, then add the per-backend `.cpp` files to
      `CMakeLists.txt`, reconcile the `GLESCShaderManager` vs `GLESShaderManager` naming, and make
      `initializeManagers()` actually construct them.
- [ ] First real draw path on Vulkan: descriptor pool + allocator initialisation, pipeline
      creation, `vkCmdBindPipeline`/`vkCmdDraw*`, `CommandBuffer` replay.
- [ ] GLSL→SPIR-V→ESSL toolchain (glslang + SPIRV-Cross vendored or fetched).
- [ ] `libcopper-oxide` as an `com.android.library`/AAR with a real publishing path, if a launcher
      integration story is wanted.
- [ ] Minecraft integration: asset loading, texture atlas, MC shader translation.
- [ ] Mod loader integration (Fabric/NeoForge), then assess Sodium and Iris against
      [`docs/compatibility.md`](docs/compatibility.md) rather than assuming either.
- [ ] A GLES translation layer (this is a multi-year project on its own; see the Sodium/Iris
      policies cited above).
- [ ] Device test matrix. Nothing is validated on hardware today.

## Acknowledgements

Prior art studied, no code taken: [Mesa/Zink](https://github.com/mesa3d/mesa),
[ANGLE](https://github.com/google/angle), [gl4es](https://github.com/gl4es/gl4es),
[MobileGL](https://github.com/MobileGL/MobileGL), [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross),
[glslang](https://github.com/KhronosGroup/glslang),
[PojavLauncher](https://github.com/PojavLauncher/PojavLauncher),
[Zalith](https://github.com/KennnyProject/Zalith), and the Sodium/Iris/VulkanMod renderer designs.
See [Project identity](#third-party-and-licensing-note) for the licensing consequences.

## License

MIT — see [LICENSE](LICENSE).

Author: **oxide-mc**.