# Compatibility checklist

An engineering checklist for making Copper Oxide able to render Minecraft: Java Edition on
Android, and for assessing (not promising) Sodium and Iris support.

Scope and honesty rules for this document:

- **Status is read from the source in this repository**, not from intent. "Done" means there is
  working code in the tree, on the path the build actually compiles.
- Where a fact comes from an upstream project, the URL is given. Where I could not verify a claim
  I say so instead of asserting it.
- Nothing here is a compatibility promise. Sodium and Iris upstream both decline to support this
  class of platform, so several gates below are unlikely to be closable on Android.

Legend: `[x]` done · `[~]` partial · `[ ]` not done.

---

## 0. Ground rules about what Copper Oxide is (all verified in this tree)

- [x] The module is `com.android.application` (`app/build.gradle.kts:2`) — an APK. There is no
      `com.android.library`, no `maven-publish`, and no `com.oxide.mc:copper-oxide` artifact.
- [x] No Minecraft client code exists here. No launcher code exists here.
- [x] No GL interception exists: no `libGLESv2`/`libGLESv3` symbol interposition, no `GLESWrapper`,
      no `dlopen`-based entry-point redirection, no GLSL→ESSL source rewriter.
- [x] No mod-loader integration: no Fabric/Forge/NeoForge/Quilt detection, no Mixin, no
      class-loading hooks, no entry-point discovery.
- [x] No shader compiler is linked. `app/src/main/cpp/CMakeLists.txt` fetches only
      VulkanMemoryAllocator v3.3.0 and lists 17 `.cpp` files, all of them the common/base sources
      plus the two backends. `ShaderManager::createShaderFromGLSL()` in the base is a placeholder
      (`shader_manager.cpp:88`) and `compileAsync()` runs inline (`shader_manager.cpp:258`).
      The GLES shader manager writes source straight to `glShaderSource` on the device driver
      after inserting a precision preamble — not a GLSL→ESSL translator.
- [x] No draw submission exists: grepping `app/src/main/cpp` for `vkCmdDraw*` / `glDrawArrays` /
      `glDrawElements` / `glDrawRangeElements` returns only *comments*. Vulkan's per-frame work is
      `vkCmdBeginRenderPass` → `vkCmdClearAttachments` → `vkCmdEndRenderPass`
      (`vulkan_renderer.cpp:742-744`).

> **Repository state note.** Per-backend manager sources (`renderer/gles/gles_*`,
> `renderer/vulkan/vulkan_*`) were being added concurrently with this document and are **not** in
> `CMakeLists.txt`, so none of them compile into `libcopper-oxide.so` and none is reflected below
> as working. Their presence is recorded in §1.2 and §1.3 only as build-integration debt.

**Consequence:** every game-facing gate below is `[ ]`. The list is the work, not a description of
current capability.

---

## 1. Backend bring-up (needed before anything else)

### 1.1 Vulkan

- [x] `vkCreateInstance`, `VK_KHR_surface`, `VK_KHR_android_surface`, `VK_EXT_debug_utils`
- [x] Instance-level function pointer loading via `vkGetInstanceProcAddr` (`vulkan_renderer.cpp:30`)
- [x] Device creation with `VK_KHR_swapchain`, device-level loading via `vkGetDeviceProcAddr` (`:48`)
- [x] `VkSwapchainKHR` with extent clamping to `min/maxImageExtent`, `VK_FORMAT_UNDEFINED` rejection,
      FIFO→MAILBOX selection (`:376`)
- [x] Single-attachment `VkRenderPass` + per-image `VkFramebuffer` (`:507`, `:530`)
- [x] Per-frame acquire / fence wait / submit / present with out-of-date handling (`:676-788`)
- [x] Command pool + `frames_in_flight_` clamped to 1–3 (`:636`)
- [ ] **Physical-device selection with real requirements.** `bool suitable = true;` with a
      `// ... feature checks` placeholder (`vulkan_renderer.cpp:305-306`). Needs an explicit required
      set (at minimum: `VkPhysicalDeviceFeatures::multiDrawIndirect` or
      `VK_EXT_multi_draw_direct`, `shaderStorageImageExtendedFormats`, `samplerAnisotropy`, fill-mode
      non-solid for shader packs, `maxColorAttachments`).
- [ ] **`VkPhysicalDeviceFeatures` enablement.** `create_logical_device()` passes an all-zero
      feature struct (`:347-348`); `pEnabledFeatures` therefore enables nothing.
- [ ] `create_allocator()`, `create_descriptor_pool()`, `create_pipeline_cache()` are declared in
      `vulkan_renderer.h:179-181`, have **no definitions**, and are never called. VMA is linked
      (`vma_impl.cpp`) but never initialised; no descriptor pool exists.
- [ ] Graphics pipeline creation, descriptor set layout/pool/allocate, pipeline cache.
- [ ] `vkCmdBindPipeline`, `vkCmdBindDescriptorSets`, `vkCmdSetViewport/Scissor`, `vkCmdDraw*`,
      `vkCmdDrawIndexed*`, `vkCmdPushConstants`.
- [ ] `CommandBuffer` recording exists in the base (`command_buffer.cpp`) but nothing replays it.
- [ ] Queue family selection: `compute_queue_`/`transfer_queue_` are aliases of `graphics_queue_`
      (`:336-338`, `:362-363`). A real 3-queue split is needed before async compute.
- [ ] Multi-frame command buffer submission — currently `VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT`
      with per-frame fences only.
- [ ] `VulkanRenderer::query_gpu_info()` never fills `gpu_info_.extensions`;
      `supported_features_` is never set, so `supportsFeature()` / `isExtensionSupported()`
      (`:922-931`) always return `false`.
- [ ] `VulkanRenderer::apply_driver_workarounds()` constructs a `GPUCapabilities` and discards it
      (`:884-888`).

### 1.2 OpenGL ES

- [x] `eglGetDisplay`/`eglInitialize`/`eglBindAPI(EGL_OPENGL_ES_API)`
- [x] Config selection with four descending fallback templates
      (`gles_renderer.cpp:33-38`), window+pbuffer surface types
- [x] Context creation with `EGL_CONTEXT_CLIENT_VERSION = 3` (`:224`)
- [x] Window surface and 1×1 pbuffer fallback for headless probing (`:238-250`)
- [x] `make_current()` per frame — the Kotlin render loop hops coroutine threads (`:444-455`)
- [x] `glFlush` on frame end (no `glFinish`), `eglSwapBuffers` on present, `EGL_CONTEXT_LOST`
      detection (`:457-480`)
- [x] `glGetString`/`glGetIntegerv` capability and limit query (`:252-414`)
- [x] Extension string parsing (`:363`)
- [ ] **GL interception.** Nothing intercepts `libGLESv3`. Without it the Minecraft JVM calls
      real Android GLES, not Copper Oxide. This is the whole problem.
      *(The `eglGetProcAddress` calls in `gles_buffer_manager.cpp:34` and
      `gles_shader_manager.cpp:55` resolve optional backend extensions — `glObjectLabel`,
      `glSpecializeShader` — not JVM entry points. They are not interception.)*
- [ ] **Context version mismatch.** Sources include `<GLES3/gl32.h>` and query ES 3.2-only enums
      (`GL_MAJOR_VERSION`, `GL_MINOR_VERSION` at `:271-274`, `glGetString(GL_EXTENSIONS)` is the
      3.0 form), but the requested context is ES 3.0 and the NDK library linked is `GLESv3`, not
      `GLESv3_2`. Feature bits are therefore *probed* on a context that may not have them.
- [ ] The last config template falls back to `EGL_OPENGL_ES2_BIT` (`:37`), which cannot satisfy
      `EGL_CONTEXT_CLIENT_VERSION = 3`; that path fails at context creation rather than degrading.
- [ ] **Per-backend manager sources are not compiled.** `gles_buffer_manager.*`,
      `gles_texture_manager.*`, `gles_shader_manager.*`, `gles_framebuffer_manager.*`,
      `gles_state_manager.*`, `gles_sync_manager.*`, `gles_command_buffer.*`,
      `gles_resource_pool.*`, `gles_profiler.*` exist under `renderer/gles/` but none appears in
      `CMakeLists.txt`. `initializeManagers()` returns `true` without constructing any of them
      (`:586-592`).
- [ ] **Duplicate class declarations will collide on build.** `GLESCFramebufferManager` is an
      empty stub in `gles_renderer.h:271` and a real class in `gles_framebuffer_manager.h:27`;
      `GLESCStateManager` likewise (`gles_renderer.h:272` vs `gles_state_manager.h:39`).
      Additionally `gles_shader_manager.h:38` defines **`GLESShaderManager`** while
      `gles_renderer.h:232` declares **`GLESCShaderManager`** — the two spellings never meet, so
      neither can be the type the renderer stores.
- [ ] `GPUCapabilities::queryGLESProperties()` calls `glGetString` with **no current GL context**
      (`gpu_capabilities.cpp:311-313`), so the GL fallback detection path returns nulls.

### 1.3 Shared

- [ ] **Duplicate declarations block the build, on both backends.**
      - Vulkan: `vulkan_shader_manager.cpp` is **not in `CMakeLists.txt`**, and its
        `VulkanShaderManager` collides with the inline stub in `vulkan_renderer.h:275-313`. The
        header's own `INTEGRATOR NOTE` (`vulkan_shader_manager.h:53-58`) documents this. The same
        applies to `VulkanBufferManager` (`:203`), `VulkanFramebufferManager` (`:316`) and
        `VulkanStateManager` (`:317`).
      - GLES: the equivalent stubs remain at `gles_renderer.h:271-272` while
        `gles_framebuffer_manager.h` / `gles_state_manager.h` now define real classes with the
        same names (§1.2).
      - Fix the duplicate declarations *before* adding any of these files, or the build fails on
        redefinition.
- [ ] `RendererRegistry` / `COPPER_REGISTER_RENDERER` (`renderer_base.h:281-315`) is never
      populated; `jni_bridge.cpp` constructs backends directly. Either register them or delete the
      registry.
- [ ] `CopperOxideRenderer.nativeResetFrameStats` is declared `external` in Kotlin
      (`CopperOxideRenderer.kt:394`) with no JNI implementation → `UnsatisfiedLinkError`.
- [ ] `AndroidManifest.xml` declares no `<uses-feature android:glEsVersion=…>` and no
      `<uses-feature android:name="android.hardware.vulkan.version">`, so Play Store filtering and
      runtime checks are absent.
- [ ] `AndroidPlatform::getThermalThrottlingRatio()`, `setPerformanceHint()`, `getBatteryLevel()`,
      `isCharging()`, `getDisplayInfo()` all return constants (`android_platform.cpp:83-145`).

---

## 2. Minecraft-compatible GLES layer: required GL entry points

This is the surface a translation layer must intercept, because it is what LWJGL/Minecraft calls.
Everything is `[ ]`.

### 2.1 Context, extensions, formats

- [ ] `eglCreateContext` / `eglDestroyContext` with `EGL_CONTEXT_CLIENT_VERSION` ≥ 3
- [ ] `eglMakeCurrent(dpy, draw, read, ctx)` with correct thread affinity — Minecraft's render
      thread must own the context for its whole lifetime
- [ ] `glGetString(GL_EXTENSIONS)` **and** `glGetStringi(GL_EXTENSIONS, i)` (ES 3.2 form; LWJGL uses
      the indexed form on newer versions)
- [ ] `glGetIntegerv` / `glGetFloatv` / `glGetBooleanv` for `GL_MAX_*` limits — a layer that
      over-reports limits will make Minecraft allocate structures the ES driver cannot back
- [ ] `glGetString(GL_VERSION)`, `GL_VENDOR`, `GL_RENDERER`, `GL_SHADING_LANGUAGE_VERSION` —
      **must report a version Minecraft accepts.** Reporting ES 3.1 where Minecraft demands 3.2+,
      or vice versa, changes code paths on the Java side.
- [ ] Extension strings must be *honest*: advertising `GL_ARB_*`/`GL_EXT_*` names that the
      translation is only partially emulating is the classic cause of "works until it doesn't".

### 2.2 Buffers, mapping, sync (needed by any modern MC pipeline)

- [ ] `glGenBuffers` / `glBindBuffer` / `glBufferData` / `glBufferSubData` / `glDeleteBuffers`
- [ ] `glBindBufferRange` / `glBindBufferBase` (uniform + shader-storage binding points)
- [ ] `glMapBuffer` / `glMapBufferRange` / `glUnmapBuffer` / `glFlushMappedBufferRange`
      / `glGetBufferSubData`
- [ ] `glCopyBufferSubData`
- [ ] `glFenceSync` / `glClientWaitSync` / `glWaitSync` / `glGetSynciv` / `glDeleteSync`
      (ES 3.0 core, but *not* guaranteed correct on Android; Iris/Sodium depend on them)
- [ ] Persistent coherent mapping (`glBufferStorage` with `GL_MAP_PERSISTENT_BIT` |
      `GL_MAP_COHERENT_BIT`) and `glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT)`
- [ ] `glReadPixels` with `GL_PACK_BUFFER` / `glReadPixels` into a mapped buffer
- [ ] Pixel-store state: `glPixelStorei(GL_UNPACK_ALIGNMENT|ROW_LENGTH|SKIP_PIXELS|SKIP_ROWS|IMAGE_HEIGHT|SWAP_BYTES)`
- [ ] `glFinish` / `glFlush` semantics that do not stall the whole pipeline

### 2.3 Textures and samplers

- [ ] `glGenTextures` / `glBindTexture` / `glTexImage2D` / `glTexSubImage2D` /
      `glTexImage3D` / `glTexSubImage3D` / `glCompressedTexImage2D` / `glCompressedTexSubImage2D`
- [ ] `glTexStorage2D` / `glTexStorage3D` (immutable storage)
- [ ] `glGenerateMipmap`, `glTexParameteri/f/iv`, `glGetTexParameter*`, `glGetTexLevelParameter*`
- [ ] All sampler targets: `2D`, `2D_ARRAY`, `3D`, `CUBE_MAP`, `2D_MULTISAMPLE`
- [ ] `glActiveTexture` / `glBindTextureUnit`-equivalent, and multi-bind (`glBindTextures`)
- [ ] Sampler objects: `glGenSamplers` / `glBindSampler` / `glSamplerParameteri/f/iv` /
      `glDeleteSamplers` — **ES 3.0/3.1 core; some Android drivers get this wrong**
- [ ] `glCopyTexImage2D` / `glCopyTexSubImage2D` / `glCopyImageSubData` (GL 4.3 semantics)
- [ ] `GL_TEXTURE_MAX_ANISOTROPY_EXT` / `GL_TEXTURE_LOD_BIAS` / `GL_TEXTURE_SWIZZLE_*`
- [ ] `GL_EXT_texture_swizzle` / `GL_ARB_texture_swizzle` equivalent — Iris rewrites swizzles
      at draw time (`ARBTextureSwizzle.GL_TEXTURE_SWIZZLE_RGBA`, `IrisRenderSystem.java`)

### 2.4 Framebuffers and multiple render targets

- [ ] `glGenFramebuffers` / `glBindFramebuffer` / `glFramebufferTexture2D` /
      `glFramebufferRenderbuffer` / `glFramebufferTextureLayer` / `glCheckFramebufferStatus`
- [ ] Read/draw framebuffer split (`GL_READ_FRAMEBUFFER` / `GL_DRAW_FRAMEBUFFER`)
- [ ] `glGenRenderbuffers` / `glBindRenderbuffer` / `glRenderbufferStorage` /
      `glRenderbufferStorageMultisample` / `glBlitFramebuffer`
- [ ] `glDrawBuffers` / `glReadBuffer` / `glClearBufferfv|iv|uiv|fi` — **ES 3.2 guarantees only 8
      draw buffers and 4 colour attachments** (ES 3.2 §21.46 *Implementation Dependent Values*,
      Table 21.46 — spec-derived; I could not fetch the PDF in this environment, so treat the
      exact numbers as unverified minimums and query `GL_MAX_DRAW_BUFFERS` /
      `GL_MAX_COLOR_ATTACHMENTS` at runtime). Vanilla uses 8; shader packs routinely want more.
- [ ] `glClearBufferfv/iv/uiv/fi` per-attachment clears
- [ ] **Per-attachment blend state** — `glEnablei(GL_BLEND, i)` / `glDisablei` /
      `glBlendEquationi` / `glBlendFuncSeparatei` (`GL_ARB_draw_buffers_blend` on desktop; ES has
      no portable equivalent). Iris requires this: see §4.2.
- [ ] `glInvalidateFramebuffer` / `glInvalidateSubFramebuffer`

### 2.5 Shaders and programs

- [ ] `glCreateShader` / `glShaderSource` / `glCompileShader` / `glGetShaderiv` /
      `glGetShaderInfoLog` / `glDeleteShader`
- [ ] `glCreateProgram` / `glAttachShader` / `glLinkProgram` / `glGetProgramiv` /
      `glGetProgramInfoLog` / `glUseProgram` / `glDeleteProgram`
- [ ] `glGetUniformLocation`, `glGetUniformBlockIndex`, `glUniformBlockBinding`,
      `glGetFragDataLocation` / `glBindFragDataLocation`
- [ ] **Explicit output-location binding.** ES GLSL ES 3.00 has no `layout(location=)` on fragment
      outputs; a layer must inject bindings and preserve the source's `#version`/`#extension`/`#line`
      structure so error line numbers still point at the user's shader.
- [ ] **`glBindAttribLocation` before link.** ES exposes no `GL_VERTEX_SHADER` pre-link binding of
      the same form in all drivers; verify against the actual device.
- [ ] `highp`/`mediump`/`lowp` qualifier handling in fragment shaders — ESSL fragment `highp`
      is optional and some drivers silently demote it. Iris explicitly notes this class of problem
      (`drivers.md`: "features of OpenGL that cannot be clearly translated to OpenGL ES").
- [ ] `glGetActiveUniform` / `glGetActiveAttrib` / `glGetUniformIndices` /
      `glGetActiveUniforms` with `UNIFORM_BLOCK_*` queries
- [ ] Transform feedback (`glBeginTransformFeedback`, `glTransformFeedbackVaryings`) — ES 3.0
      core, used by some vanilla paths and by shader packs.

### 2.6 Draw, instancing, indirect

- [ ] `glDrawArrays` / `glDrawElements` / `glDrawRangeElements`
- [ ] `glDrawArraysInstanced` / `glDrawElementsInstanced` / `glDrawElementsBaseVertex`
- [ ] `glMultiDrawArrays` / `glMultiDrawElements` / `glMultiDrawElementsBaseVertex`
- [ ] `glDrawArraysIndirect` / `glDrawElementsIndirect` / `glMultiDrawElementsIndirect`
- [ ] `glPrimitiveRestartIndex`, `glProvokingVertex` semantics
- [ ] Vertex array objects: `glGenVertexArrays` / `glBindVertexArray` / `glEnableVertexAttribArray`
      / `glVertexAttribPointer` / `glVertexAttribIPointer` / `glVertexAttribDivisor`
- [ ] `glMultiDrawElementsIndirectCount` / `GL_ARB_multi_draw_indirect`
- [ ] Geometry/tessellation stages (`GL_EXT_geometry_shader`, `GL_EXT_tessellation_shader`) — used by
      shader packs, absent from most ES 3.2 implementations

### 2.7 Compute

- [ ] `glDispatchCompute` / `glDispatchComputeIndirect` / `glMemoryBarrier` — ES 3.1 core;
      `GL_KHR_compute_shader` style caps must be advertised correctly
- [ ] Shader storage buffer objects (`GL_SHADER_STORAGE_BUFFER`, `glBindBufferBase`,
      `glGetBufferSubData` from an SSBO) — ES 3.1 core
- [ ] Image load/store (`glBindImageTexture`, `glGetImage*`, `glTexImage*` with image format)
      — **ES 3.1 core but the weakest area of Android driver support**

### 2.8 State semantics (the part that produces *silent* corruption)

A layer that redirects calls but does not reproduce the exact state machine will not fail loudly;
it will render wrong frames. These are the semantics to match:

- [ ] Every one of `glEnable`/`glDisable` on **all** targets, including `GL_*_` per-attachment
- [ ] All `glBlendFunc`/`glBlendFuncSeparate`/`glBlendEquation`/`glBlendEquationSeparate` variants
      **and their per-attachment (`*i`) forms**
- [ ] `glDepthFunc`, `glDepthMask`, `glStencilFunc*`, `glStencilOp*`, `glStencilMask*`,
      `glColorMask`
- [ ] `glCullFace`, `glFrontFace`, **`glPolygonMode`** (wireframe for the `wireframe` render
      type and for some debug modes; ES 3.2 requires `GL_EXT_polygon_mode`-style polyline support)
- [ ] `glViewport` / `glScissor` (with `GL_SCISSOR_TEST`), and the fact that `glViewport` is *not*
      framebuffer state
- [ ] Default values of every piece of state, initialised to spec defaults at context creation
- [ ] Feedback loops: reading and writing the same texture/attachment in one draw is undefined;
      a layer must not accidentally make it "work" in a way that diverges from a real driver
- [ ] Default-framebuffer vs. FBO binding independence (`GL_DRAW_FRAMEBUFFER_BINDING` vs.
      `GL_READ_FRAMEBUFFER_BINDING` must be tracked separately)
- [ ] **Interception must be complete.** If any entry point escapes interception, control reaches
      the real driver and state diverges. This is the failure mode behind most "GL4ES works but
      breaks on one specific screen" reports.

### 2.9 Mojang's own abstractions (this is what the mods actually hook)

Since ~1.21.6 the client no longer talks to GL directly from its render passes; it goes through
Mojang's abstraction. Verified in the mod sources:

- [ ] **Do not rely on intercepting GL alone.** Sodium mixins
      `com.mojang.renderpearl.backend.opengl.GlStateManager`
      ([`GlStateManagerMixin.java`](https://github.com/CaffeineMC/sodium/blob/dev/common/src/main/java/net/caffeinemc/mods/sodium/mixin/features/render/viewport/GlStateManagerMixin.java))
      and wraps `GL33C.glViewport` with a `@WrapWithCondition`. Sodium also mixins
      `com.mojang.renderpearl.frontend.FrontendGpuDevice` for an `@Accessor("backend")`
      ([`GpuDeviceAccessor.java`](https://github.com/CaffeineMC/sodium/blob/dev/common/src/main/java/net/caffeinemc/mods/sodium/mixin/core/GpuDeviceAccessor.java)).
      Iris mixins `GlCommandEncoder`, `RenderPass` and `GlStateManager`
      ([`MixinGlCommandEncoder.java`](https://github.com/IrisShaders/Iris/blob/master/common/src/main/java/net/irisshaders/iris/mixin/MixinGlCommandEncoder.java),
      [`MixinRenderPass.java`](https://github.com/IrisShaders/Iris/blob/master/common/src/main/java/net/irisshaders/iris/mixin/MixinRenderPass.java),
      [`MixinGlStateManager.java`](https://github.com/IrisShaders/Iris/blob/master/common/src/main/java/net/irisshaders/iris/mixin/MixinGlStateManager.java)).
- [ ] A layer that redirects GL symbols but leaves Mojang's `GlStateManager` caches believing the
      *real* driver holds state will silently corrupt rendering. Either
      (a) make `RenderSystem.getDevice()` return a `GpuDevice` whose backend is a genuine
      `GlDevice` subclass that routes through Copper Oxide, **or**
      (b) accept that the mods' mixins will observe the wrong state.
- [ ] Vertex/index data paths the mods assume: `GpuBuffer`, `GpuBufferSlice`, `GpuSampler`,
      `RenderPass.setPipeline/setUniform/setVertexBuffer/setIndexBuffer/pushConstants`
      (see [`DefaultChunkRenderer.java`](https://github.com/CaffeineMC/sodium/blob/dev/common/src/main/java/net/caffeinemc/mods/sodium/client/render/chunk/DefaultChunkRenderer.java)).
- [ ] `RenderSystem.getSamplerCache()` and `RenderSystem.getProjectionMatrixBuffer()` equivalents.

---

## 3. Sodium hard gates

Sodium's own position, quoted:

> We only provide official support for graphics cards which have up-to-date drivers that are
> compatible with OpenGL 4.5 or newer.
>
> Devices which need to use OpenGL translation layers (such as GL4ES, ANGLE, etc) are not
> supported and will very likely not work with Sodium. These translation layers do not implement
> required functionality, and they suffer from underlying driver bugs which cannot be worked around.

— [Sodium README, *Hardware Compatibility*](https://github.com/CaffeineMC/sodium#-hardware-compatibility)

and, on the maintainer's Pojav-launcher ruling:

> "Zink is the exception to this rule, as it is an OpenGL-on-Vulkan translation layer which
> actually has access to the aforementioned functionality… Our team is not going to fix
> compatibility with broken graphics drivers."
> — [Sodium issue #1916](https://github.com/CaffeineMC/sodium/issues/1916) (closed,
> `E-will-not-fix`, `A-drivers`)

Consequences for a Copper Oxide-backed GLES layer:

- [ ] **Gate S1 — OpenGL 4.5-class surface.** Sodium requires far more than ES 3.2 core. Specific
      entry points Sodium's own sources depend on (verified in `IrisRenderSystem`/Sodium sources,
      listed below in §4.2 for the shared set): `GL_ARB_direct_state_access` or GL 4.5 core,
      `ARB_multi_bind` / `glBindSamplers`, `glBufferStorage`, `glBindBufferBase`,
      `GL_ARB_shader_storage_buffer_object` + `GL_ARB_buffer_storage`, `glDispatchCompute` +
      `glMemoryBarrier`, `glBindImageTexture`, `GL_ARB_draw_buffers_blend`, `ARB_texture_swizzle`,
      `glPolygonMode`, `glCopyImageSubData`, `glGetStringi`.
      *Status: `[ ]` — Copper Oxide implements none of these; there is no interception layer at all.*
- [ ] **Gate S2 — multidraw with base vertex.** Sodium's OpenGL terrain path is a single call per
      region: `RenderPass.multiDrawIndexed(offsets, counts, baseVertex, drawCount)`
      ([`GLDrawBatch.java`](https://github.com/CaffeineMC/sodium/blob/dev/common/src/main/java/net/caffeinemc/mods/sodium/client/gpu/device/batch/GLDrawBatch.java)).
      That resolves to `glMultiDrawElementsBaseVertex` under Mojang's GL backend, which has no
      direct ES equivalent — `GL_EXT_multi_draw_indirect` (`glMultiDrawElementsIndirect`) has a
      different signature (strided command structs, no `baseVertex`).
      *Status: `[ ]` — not implementable without a real multi-draw/indirect emulation path.*
- [ ] **Gate S3 — a `GpuDevice` backend Sodium recognises.** Sodium's `DrawBackend` enum has exactly
      three values and refuses to run if none applies:
      `OPENGL`, `VK_MULTIDRAW` (requires `device.getDeviceInfo().features().multiDrawDirectInterleaved()`,
      i.e. `VK_EXT_multi_draw` / `VK_KHR_multi_draw`), and `VK_INDIRECT` (requires
      `multiDrawIndirect`); otherwise it throws `IllegalStateException`
      ([`DrawBackend.java`](https://github.com/CaffeineMC/sodium/blob/dev/common/src/main/java/net/caffeinemc/mods/sodium/client/gpu/device/backend/DrawBackend.java)).
      A Copper Oxide Vulkan device would therefore have to expose `multiDrawDirectInterleaved` or
      `multiDrawIndirect` truthfully in its `DeviceInfo`, and would have to implement
      `VK_MULTIDRAW`/`VK_INDIRECT` command encoding.
      *Status: `[ ]` — Copper Oxide does not implement Mojang's `GpuDevice` at all; `create_logical_device()`
      enables no `VkPhysicalDeviceFeatures`.*
- [ ] **Gate S4 — terrain vertex data path.** Sodium uploads geometry and reads section mesh data
      through a persistent-mapped buffer arena
      ([`client/gpu/arena/`](https://github.com/CaffeineMC/sodium/tree/dev/common/src/main/java/net/caffeinemc/mods/sodium/client/gpu/arena)),
      using `MemoryIntrinsics` direct writes. Requires
      `GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT` mapping and correct
      `GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT` behaviour.
      *Status: `[ ]`.*
- [ ] **Gate S5 — Sodium's mixins must find their targets.** `GlStateManagerMixin` targets
      `com.mojang.renderpearl.backend.opengl.GlStateManager` and wraps a `GL33C.glViewport` call;
      `GpuDeviceAccessor` targets `FrontendGpuDevice`. If Copper Oxide replaces the GL backend with
      its own class, both mixins fail to apply and Sodium crashes at class-load.
      *Status: `[ ]`.*
- [ ] **Gate S6 — mixin compatibility.** Fabric/NeoForge mixin application against
      Mojang-mapped `renderpearl` internals is version-locked. Copper Oxide would have to
      re-target or shadow-patch those classes per Minecraft version.
      *Status: `[ ]`.*
- [ ] **Gate S7 — Android driver bugs, not just missing features.** Sodium's policy blames
      translation layers partly for "underlying driver bugs which cannot be worked around".
      A Copper Oxide layer inherits this. Not closable by engineering alone.
      *Status: `[ ]` — likely permanent on ES drivers.*

**Verdict: Sodium is `[ ]` not supported today, and the gap is structural, not incremental.**
Note also that Sodium is [Polyform Shield 1.0.0](https://github.com/CaffeineMC/sodium/blob/dev/LICENSE.md),
not an OSI licence — Copper Oxide can only cooperate with a separately-installed Sodium, never
vendor it.

---

## 4. Iris / shader-pack hard gates

Iris's own position, quoted:

> - Mobile devices (PojavLauncher, etc)
>   - ❌ Not supported
>
> "Not supported. Android OpenGL ES drivers have huge amounts of bugs and poorly support most
> features of OpenGL ES. GL4ES has many bugs as well, and there are some features of OpenGL that
> cannot be clearly translated to OpenGL ES."
>
> "If your configuration is marked as *❌ Not supported*, we ask that you please do not submit
> issue reports or make support requests to us."
> — [Iris `docs/usage/drivers.md`](https://github.com/IrisShaders/Iris/blob/master/docs/usage/drivers.md)

- [ ] **Gate I1 — Iris requires a GL `GpuDevice`.** `IrisRenderSystem.getGlDevice()` is
      `(GlDevice) ((GpuDeviceAccessor) RenderSystem.getDevice()).getBackend()` — an unchecked cast
      to `com.mojang.blaze3d.opengl.GlDevice`. A Vulkan-backed Copper Oxide device cannot satisfy
      it, and Iris has no Vulkan path.
      *Status: `[ ]` — Copper Oxide implements no `GpuDevice`.*
- [ ] **Gate I2 — DSA.** `initRenderer()` enables DSA only when `GL.getCapabilities().OpenGL45` or
      `GL_ARB_direct_state_access`; otherwise it falls back to a save/restore path over
      `GlStateManager._glBindFramebuffer` etc. and logs "DSA support not detected." A translation
      layer must either emulate `GL_ARB_direct_state_access` or accept a large performance cliff
      and still behave correctly under the fallback.
      *Status: `[ ]`.*
- [ ] **Gate I3 — MRT depth.** Iris queries `GL_MAX_DRAW_BUFFERS`
      ([`SamplerLimits.java`](https://github.com/IrisShaders/Iris/blob/master/common/src/main/java/net/irisshaders/iris/gl/sampler/SamplerLimits.java))
      and builds framebuffers with up to ~32 colour attachments for large shader packs. ES 3.2
      guarantees `MAX_COLOR_ATTACHMENTS` ≥ 4 and `MAX_DRAW_BUFFERS` ≥ 8 (ES 3.2 §21.46 Table 21.46;
      **spec-derived minimums, not independently verified here**). Beyond the guaranteed minimum a
      device may still report 8, which is insufficient for the common packs.
      *Status: `[ ]`.*
- [ ] **Gate I4 — per-attachment blend.** `IrisRenderSystem` uses `glEnablei/glDisablei(GL_BLEND, i)`
      and `ARBDrawBuffersBlend.glBlendFuncSeparateiARB`, then **mutates Mojang's state cache
      directly**: `((BooleanStateExtended) GlStateManagerAccessor.getBLEND().mode).setUnknownState()`.
      ES has no `EXT_draw_buffers_blend`; `EXT_shader_framebuffer_fetch` is a different mechanism
      entirely. A layer must emulate indexed blend state *and* keep `GlStateManager`'s cache
      coherent.
      *Status: `[ ]`.*
- [ ] **Gate I5 — SSBOs and compute.** `supportsSSBO()` requires GL 4.4 or
      `ARB_shader_storage_buffer_object` + `ARB_buffer_storage`; `dispatchCompute` uses
      `GL45C.glDispatchCompute`; `memoryBarrier` uses `GL45C.glMemoryBarrier`. ES 3.1 has SSBOs
      and compute, but shader packs written against desktop idioms will need work; a
      GLSL→ESSL rewrite is required and does not exist in this repo.
      *Status: `[ ]`.*
- [ ] **Gate I6 — sampler objects + multi-bind.** `glGenSampler`/`glBindSampler`/
      `glSamplerParameteri`/`glSamplerParameterf` and `GL45C.glBindSamplers(0, emptyArray)` when
      `hasMultibind`, else a per-unit loop. ES 3.0 has sampler objects; `glBindSamplers` needs
      `EXT_multi_bind` and many Android drivers mis-handle it.
      *Status: `[ ]`.*
- [ ] **Gate I7 — GLSL→ESSL rewrite with diagnostics.** Must preserve `#version`/`#extension`,
      inject `layout(location=…)` output bindings, inject precision qualifiers (fragment `highp` is
      optional in ESSL), and preserve `#line` directives so `glGetShaderInfoLog` reports the
      user's line numbers. Iris itself points to
      [`glsl-transformer`](https://github.com/IrisShaders/glsl-transformer) for this kind of patch
      workflow. The GLES shader manager in this repo only inserts a precision preamble after
      `#version` and submits to the device driver — no cross-stage or desktop-construct rewriting,
      and no `#line` remapping.
      *Status: `[ ]` — the file exists but is not in the build, and the transformation it performs
      is a fraction of what this gate requires.*
- [ ] **Gate I8 — image load/store and polygon mode.** `glBindImageTexture` (GL 4.2 or
      `ARB_shader_image_load_store`), `glDispatchComputeIndirect`, `glPolygonMode(GL_FRONT_AND_BACK, …)`,
      `glCopyImageSubData`, `glGetStringi`, `NVX_gpu_memory_info` (`getVRAM()`), `glClearBufferSubData`.
      *Status: `[ ]`.*
- [ ] **Gate I9 — Iris mixin targets must resolve.** `MixinGlCommandEncoder`, `MixinRenderPass`,
      `MixinGlStateManager` and its accessor target Mojang internals; `iris.accesswidener` and
      `neoforge/.../accesstransformer.cfg` widen access. A layer that replaces those classes breaks
      all of them.
      *Status: `[ ]`.*

**Verdict: Iris is `[ ]` not supported today.** Iris is LGPL-3.0 and, like Sodium, can only be
co-operated with, never vendored into an MIT project.

---

## 5. Vulkan-native path: what a `GpuDevice` integration requires

This is the only route with a plausible chance of both mods, because Sodium explicitly
special-cases a `VulkanDevice` backend and Zink is the one translation approach Sodium names as
viable.

- [ ] **V1 — Implement `GpuDeviceBackend` / provide a `VulkanDevice`.** `RenderSystem.getDevice()`
      must return a `FrontendGpuDevice` wrapping a backend that Sodium's `GpuDeviceAccessor`
      recognises via `instanceof VulkanDevice`
      ([`DrawBackend.chooseBackend()`](https://github.com/CaffeineMC/sodium/blob/dev/common/src/main/java/net/caffeinemc/mods/sodium/client/gpu/device/backend/DrawBackend.java)).
      Copper Oxide currently exposes no Java-side device abstraction at all — only
      `CopperOxideRenderer`.
- [ ] **V2 — Truthful `DeviceInfo.features()`.** At minimum `multiDrawDirectInterleaved` (or
      `multiDrawIndirect`), plus what a translation would otherwise fake. Advertising a feature the
      backend cannot honour is worse than not advertising it: Sodium takes that branch and crashes
      or corrupts.
- [ ] **V3 — `CommandEncoder` / `RenderPass` / `RenderPipeline`.** Vanilla and both mods drive
      rendering through these. Needs `setPipeline`, `setUniform` (including `GpuBufferSlice` /
      `GpuSampler`), `setVertexBuffer`, `setIndexBuffer`, `pushConstants`, `multiDrawIndexed`
      equivalents, draw/drawIndexed/drawIndirect/dispatch, resource transitions, and correct
      barrier insertion.
- [ ] **V4 — Command ordering and memory.** Sodium submits from worker threads and relies on
      explicit ordering plus fences. Copper Oxide's current single-threaded
      `Dispatchers.Default` coroutine plus per-frame fences is not enough.
- [ ] **V5 — Image/buffer formats and layout tracking.** Vanilla uses `RGBA8`, `RGBA8_SRGB`, `R8`,
      and (with Sodium/OIT) additional formats; NaCl-less `NaN`-free float handling matters for
      shader packs.
- [ ] **V6 — Descriptor model.** `descriptor_pool_` and `create_descriptor_pool()` are declared
      but never defined or called; the renderer enables no features. Need a real descriptor
      allocator + `DescriptorBinding` policy (binding counts are all the current
      `PipelineLayoutDesc` carries).
- [ ] **V7 — SPIR-V ingestion.** Vanilla/MC shaders arrive as GLSL and must be compiled.
      `ShaderManager::createShader(stage, spirv, entry)` is the seam, but the Vulkan shader
      manager is not in the build (§1.3). A glslang integration is required — none is linked.
- [ ] **V8 — Vulkan version.** Instance is created with `VK_API_VERSION_1_1`
      (`vulkan_renderer.cpp:228`); VMA is compiled against a Vulkan 1.0 surface with dynamic
      function loading (`vma_impl.cpp`). Anything above 1.1 must be feature-detected, not assumed.
- [ ] **V9 — Surface/swapchain semantics Minecraft needs.** MC owns the window; the swapchain must
      not tear, must handle `SUBOPTIMAL`, must survive `onSurfaceDestroyed`/recreate. Partially
      present.
- [ ] **V10 — Mod-loader injection point.** Even a perfect Vulkan renderer needs Fabric/NeoForge
      loading to be observable so mods attach. None of this is present.

---

## 6. Test strategy

Two tiers. Neither exists today beyond a single smoke test.

### 6.1 What can be validated emulated (fast, CI, no device farm)

- [ ] Native library loads on `x86_64` and `arm64-v8a` (`androidTest`)
- [ ] `RendererConfig` presets serialise correctly (exists: `RendererConfigTest.kt`, 4 tests, JVM)
- [ ] Backend fallback: Vulkan init fails → GLES used (`jni_bridge.cpp:133-164`) — needs an
      injectable failure path to be testable
- [ ] `EGL`/`Vulkan` object create/destroy symmetry under sanitizers
- [ ] Command recorder produces the expected command list from the base `CommandBuffer`
- [ ] **Build-integration guard:** every `.cpp` under `renderer/` is either listed in
      `CMakeLists.txt` or deliberately excluded, and no two translation units define the same
      class. Both classes of breakage currently exist (§1.2, §1.3). A CI grep comparing the
      `find app/src/main/cpp -name '*.cpp'` output against the `add_library` list would catch the
      first; a compile would catch the second.
- [ ] ShaderManager cache and handle-lifetime rules (`destroyShader` refuses while a pipeline
      references it — `shader_manager.cpp:107-133`)
- [ ] Every JNI symbol declared `external` in Kotlin has an implementation (currently
      `nativeResetFrameStats` does not — §1.3)
- [ ] ABI: `readelf`/`llvm-nm` on `libcopper-oxide.so` for each ABI; assert no undefined 1.1/1.2
      Vulkan symbols (the stated intent of `vma_impl.cpp`)
- [ ] Formatting/lint/static analysis gates (exists: `spotless`, `detekt`, `lint` in CI)

### 6.2 What must be validated on real hardware

Everything about actual GPU behaviour. A SwiftShader emulator proves nothing about driver
conformance.

- [ ] **Per-GPU context/instance success matrix.** Adreno 6xx/7xx, Mali Midgard/Bifrost/Valhall/G715,
      PowerVR Rogue/BXM/Furian. At minimum: context creation, correct `GL_VENDOR`/`GL_RENDERER`/
      `GL_VERSION`, sane `GL_MAX_*` values.
      *Note: today these are matched by substring (`gles_renderer.cpp:301-358`), which misclassifies
      e.g. Adreno "640" as "600" and any Mali name containing `"g7"` as Valhall-or-newer. Fix
      before publishing a support matrix.*
- [ ] **Per-driver stress of every GL entry point in §2** — a conformance harness that calls each
      intercepted function and checks `glGetError()`, compared against a non-translated reference.
- [ ] **State-divergence test:** render a known frame through the real driver and through the layer
      and compare pixel-exact. This is the only reliable detector for §2.8 bugs.
- [ ] **Fence/sync correctness:** `glFenceSync`/`glClientWaitSync` timing and
      persistent-mapping visibility, checked under concurrent producers.
- [ ] **Anisotropic filtering, mipmapping, sRGB correctness** on texture-heavy screens.
- [ ] **Framebuffer format coverage:** every framebuffer format vanilla uses, plus
      `EXT_color_buffer_half_float`/float render targets.
- [ ] **Thermal / memory-pressure paths** — `AndroidPlatform::getThermalThrottlingRatio()` is a stub,
      so these cannot currently be tested at all.
- [ ] **Context-loss and surface recreate** (backgrounding, rotation, display changes).
- [ ] **Long-run stability:** hours of continuous play, watching for driver resets and allocation
      growth.
- [ ] **Mod validation, if it is ever attempted:** Sodium and Iris on a device, with a real
      Minecraft version, with the mods' own mixins confirmed applied. There is no such result
      today.

### 6.3 Honest statement of current coverage

| Suite | Location | What it actually proves |
|---|---|---|
| JVM unit | `app/src/test/.../RendererConfigTest.kt` | Four config presets hold their documented values |
| Instrumented | `app/src/androidTest/.../RendererSmokeTest.kt` | `libcopper-oxide.so` loads; *if* a backend initialises on the emulator, frames advance. Every meaningful assertion is behind `Assume.assumeTrue(..., initialized)` — i.e. it **skips** when it fails |
| CI emulator | `.github/workflows/ci-cd.yml:254-328` | API 33 x86_64 AVD, `-gpu swiftshader_indirect`. Software rasteriser; not a conformance signal |
| Benchmark | `.github/workflows/ci-cd.yml:469-509` | Nothing — the step body is `echo "Benchmark job - would run on device farm"` |
| Hardware matrix | — | **Does not exist.** No GPU has been validated |

---

## 7. Open questions (do not answer these by assertion)

- [ ] Which Minecraft version(s) are targeted? Every §2/§3/§4 item shifts between versions,
      because Mojang's renderer abstraction changed at ~1.21.6 and Sodium/Iris re-target it.
- [ ] Is the intended distribution an AAR, an `.so` loaded by a third-party launcher, or a
      full client? The licensing in §2.9 and §3 changes with the answer.
- [ ] Is the GLES backend intended to be a full translation layer (gl4es-scale effort, ~years,
      with Sodium/Iris still declining support), or only a private backend for a client Copper Oxide
      itself drives? If the latter, §3 and §4 can be descoped entirely and the honest README
      statement is "does not target modded clients".
- [ ] Should Copper Oxide attempt Mojang-`GpuDevice`-level integration on Vulkan (the only route
      Sodium names as viable) and drop the GLES backend? The GLES path cannot pass Sodium's own
      support policy.