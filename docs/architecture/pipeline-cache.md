# Pipeline cache — four levels, specialization constants, and what not to build yet

Status: design, not implemented. Nothing in this document is in the tree yet.

> **How to read the line references.** Every `file.cpp:NNN` in this document is anchored to commit
> **`3531309`**. Other workstreams are editing these files concurrently — `vulkan_renderer.{h,cpp}`
> and the per-backend managers have already moved since that commit — so **re-anchor before you
> edit**: `git grep -n '<symbol>' app/src/main/cpp/<path>`. Function names are given alongside the
> line numbers wherever the reference is load-bearing.

Companion doc: [`shader-system.md`](shader-system.md) — that document gets source text into the
device driver. This one is about what happens after: pipeline creation, which is where the
frame-time hitch actually lives.

---

## 0. Where the code stands today (read this first)

Verified against `HEAD` = `3531309`:

| Fact | Evidence |
|---|---|
| `pipeline_cache_` is **never created**. It is only destroyed. | `renderer/vulkan/vulkan_renderer.h:139` declares it; the only use in `vulkan_renderer.cpp` is `vkDestroyPipelineCache` in `shutdown()` at `:192-195`. |
| `VulkanRenderer::create_pipeline_cache()` is declared and has **no definition**, and is never called. | declared `vulkan_renderer.h:210`; `grep -rn create_pipeline_cache` finds only the declaration. |
| Therefore both pipeline creation calls run with a null cache: `vkCreateGraphicsPipelines(device, renderer_->pipelineCache(), 1, …)` and `vkCreateComputePipelines(…, renderer_->pipelineCache(), …)`. | `vulkan_shader_manager.cpp:673-674` and `:747-748` |
| The only pipeline cache that exists is a `std::unordered_map<std::string, uint64_t>` keyed by a **caller-supplied string**, with no eviction, no bound, and no state hash. | `shader_manager.cpp:34` (`pipeline_cache`), `:215-229` (`getOrCreatePipeline`) |
| `getOrCreatePipeline` is not atomic: "miss → unlock → create → lock → emplace". Two threads can both create. | `shader_manager.cpp:215-229` (the deliberate unlock is documented for the *shader* variant at `:206`) |
| `destroyPipeline` erases only the **first** cache entry whose value is the handle, then `break`s. | `shader_manager.cpp:188-193` |
| `create_render_pass()` **destroys the previous render pass** and creates a new one, on every `create_swapchain()`. | `vulkan_renderer.cpp:530-534`, reached from `create_swapchain()` at `:475` |
| The render pass is baked into every graphics pipeline (`create_info.renderPass = render_pass`). | `vulkan_shader_manager.cpp:667` |
| So a surface resize invalidates **every** live pipeline, silently, with no cache entry removal. | combination of the two rows above |
| `FrameStats::shader_compilations` and `FrameStats::pipeline_compilations` exist and are **never incremented**. | `renderer_base.h:156-157` |
| `RendererConfig::enablePipelineCaching` and `::enableAsyncShaderCompilation` exist and are **never read**. | `renderer_config.h:44` and `:50` |
| Specialization-constant ids fall back to "ids in insertion order" when no resolver is installed. | `vulkan_shader_manager.cpp:87-116`; declared at `vulkan_shader_manager.h:71-77` |

Two of those rows are not "missing feature" but **live correctness bugs** that will produce
misrendering, not just slowness:

1. **Resize kills every pipeline** (rows 7-8). `create_swapchain()` is called from
   `recreate_swapchain()` (`:603-613`) on `VK_ERROR_OUT_OF_DATE_KHR`/`VK_SUBOPTIMAL_KHR` from
   `onBeginFrame` (`:696-699`) or `onPresent` (`:781-784`), and from `onResize` (`:790-802`).
   Every subsequent `vkCmdBindPipeline` with a stale handle is undefined behaviour.
2. **Specialization constants can be silently dropped** (§4.4).

Neither is observable today because nothing creates a pipeline yet, and
`renderer/vulkan/*.cpp` is not even in the build (`CMakeLists.txt:45-72`; see README
§"File-by-file facts"). Fixing them is Stage 0 of §5.

---

## 1. Why pipelines, not shader modules

From [`shader-system.md`](shader-system.md) §6.5, restated because it decides the whole priority
order: `vkCreateShaderModule` takes SPIR-V and returns a module. The spec defines no compilation
step in it — no machine code, no ISA selection. SPIR-V → ISA happens inside
`vkCreateGraphicsPipelines`. ANGLE says the same thing about its own backend:

> creating pipelines is a heavy operation (in particular, converting SPIR-V to assembly and
> optimizing it), and doing so at draw time has multiple draw backs, including visible hitching
> — [ANGLE PipelineCreation.md](https://github.com/google/angle/blob/main/src/libANGLE/renderer/vulkan/doc/PipelineCreation.md)

> At this time, `VkShaderModule`s are created (and cached). The appropriate specialization
> constants are then resolved and the `VkPipeline` object is created
> — [ANGLE ShaderModuleCompilation.md](https://github.com/google/angle/blob/main/src/libANGLE/renderer/vulkan/doc/ShaderModuleCompilation.md)

Consequence: **any optimisation that only touches modules or SPIR-V is a rounding error.** The
levers that matter, in order of expected value, are (a) not creating the same pipeline twice,
(b) not creating it on the render thread, (c) not creating it at all on the next process run.

---

## 2. Non-portability: what is device-specific and what is not

Getting this backwards is the most expensive mistake available here, so it is worth being precise.

**Device-specific (must be keyed on device identity):**

* `VkPipelineCache` blobs. The spec's own reason for the header: *"The results of pipeline
  compiles, however, may depend on the vendor ID, device ID, driver version, and other details of
  the device"* — which is why the header carries `vendorID`, `deviceID` and `pipelineCacheUUID`
  ([Pipeline Cache Header](https://docs.vulkan.org/spec/latest/chapters/pipelines.html)). The
  header is 32 bytes: `headerSize`, `headerVersion`, `vendorID`, `deviceID`,
  `pipelineCacheUUID[16]`, all least-significant-byte-first.
* `VkPipelineBinaryKHR` blobs, for the same reason plus more.

**Not device-specific:**

* SPIR-V produced by *our* glslang invocation. It is a pure function of source + defines +
  glslang version + the `TBuiltInResource` table + target environment. The Adreno it will run on
  never enters the generator.
* ESSL text produced by *our* SPIRV-Cross invocation. Same argument; this is why the ESSL cache
  key in [`shader-system.md`](shader-system.md) §4 carries no device identity.

**The one real coupling between device identity and SPIR-V**: we populate `TBuiltInResource` from
`VkPhysicalDeviceLimits`, so two devices with different limits produce different SPIR-V from the
same source. Device identity therefore enters the SPIR-V cache key *because the resource table is
device-derived*, not because SPIR-V is a device format. If that table ever becomes a fixed
constant, replace the identity field with a hash of the table itself.

The same "internally synchronised" property is what makes a background compiler possible:

> The use of the pipeline cache object in these commands is internally synchronized, and the same
> pipeline cache object can be used in multiple threads simultaneously.
> — [Vulkan spec, Creating a Pipeline Cache](https://docs.vulkan.org/spec/latest/chapters/pipelines.html)

So one `VkPipelineCache` can be handed to a worker thread doing `vkCreateGraphicsPipelines` while
the render thread reads it — but the `VkPipeline` **out-param** is ours and must be published
under our own lock.

---

## 3. The four-level cache

Modelled on ANGLE's design, which is the one production mobile Vulkan backend that documents
its structure. Levels 1 and 3 correspond to ANGLE's `GraphicsPipelineDesc::hash(subset)` maps and
its `PipelineCacheAccess`; level 4 is ANGLE's `PipelineHelper::getPreferredPipeline()`.

```
 L3  currently bound handle          StateManager::getBoundPipeline()
     │                              VulkanStateManager::pipeline_layout_ (:188)
     ▼
 L2  transition table                last N (state-vector → pipeline) pairs, plus
     │                              "does the previous bind satisfy this?"
     ▼
 L1  process-wide state map          PipelineStateKey → PipelineEntry
     │                              (VkPipeline, VkPipelineLayout, set layouts, refcount)
     ▼
 L0  driver VkPipelineCache          one per pipeline-library type, per device-identity epoch
     │
     ▼
    disk                           vkGetPipelineCacheData blob, header-checked
```

### L0 — driver `VkPipelineCache`

Owned by `VulkanRenderer`, one handle per pipeline-library type, created in
`create_pipeline_cache()` (`vulkan_renderer.h:210`, currently undefined) and called from
`create_logical_device()`.

* **One cache per library type, never one shared cache.** ANGLE's own guidance for
  `VK_EXT_graphics_pipeline_library`: *"Applications should still use pipeline caches to amortize
  compilation across similar stage blobs but should avoid mixing different stage types in the same
  `VkPipelineCache`, to avoid unnecessary lookup overhead."*
  ([proposal](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_EXT_graphics_pipeline_library.adoc))
  With 4 library types that is 4 `VkPipelineCache` objects, not 1.
* **Persistence.** Save via `vkGetPipelineCacheData` at `shutdown()` (before
  `vkDestroyDevice` at `vulkan_renderer.cpp:196`), load via
  `VkPipelineCacheCreateInfo::pInitialData` on the next run. Note the spec's retrieval rule: two
  calls with the same parameters must return the same data *"unless a command that modifies the
  contents of the cache is called between them"* — so serialise save against in-flight compiles.
* **Header check before load.** Parse the 32-byte `VkPipelineCacheHeaderVersionOne`; if
  `vendorID` / `deviceID` / `pipelineCacheUUID` do not match the current device, discard the file
  rather than feeding it in. Feeding mismatched data in is not an error — the spec says invalid
  cache data *"will result in the provided pipeline cache data being ignored"* — so a mismatch is
  silent, which is precisely why the check must be explicit and logged.
* **Size bound.** Cap `vkGetPipelineCacheData` output (the spec explicitly supports this:
  *"Applications can limit the amount of data retrieved from a pipeline cache object"*). Start at
  32 MiB per cache, configurable from `RendererConfig::shaderCacheSizeMb` (`renderer_config.h:60`).
* **Save on a background write**, temp file + `rename()` over the live one — the same policy as
  the shader disk cache ([`shader-system.md`](shader-system.md) §4.5), for the same reason.
* **`onMemoryPressure`/`onThermalThrottling`** (`vulkan_renderer.cpp:911-920`) currently trims only
  textures. L0 blobs and L1 pipelines are the next-largest reclaimable category; add a trim level
  that drops the disk blob first, then evicts L1 pipelines that nothing references.

### L1 — process-wide state vector → pipeline

Replaces the string-keyed `Impl::pipeline_cache` (`shader_manager.cpp:34`).

```cpp
// renderer/common/shader_manager.h
//
// The KEY is backend-agnostic (integers only, so this header stays free of
// vulkan.h — see the include list at CMakeLists.txt:20-27). Its CONSTRUCTION
// from a VkGraphicsPipelineCreateInfo lives in the backend:
// renderer/vulkan/vulkan_shader_manager.cpp.
struct PipelineStateKey {
    // Fixed-size, memcmp-comparable, no pointers. ANGLE is explicit that this
    // property is what makes hashing safe: "No gaps or padding at the end
    // ensures that hashing and memcmp checks will not run" (vk_cache_utils.h).
    uint64_t vertex_module;        // base shader handle
    uint64_t fragment_module;
    uint64_t compute_module;
    uint32_t vertex_spec_hash;     // specialization values, hashed (§4.5)
    uint32_t fragment_spec_hash;
    uint32_t compute_spec_hash;
    uint64_t render_pass;          // opaque uint64_t, never VkRenderPass in this header
    uint32_t               subpass;
    uint32_t               vertex_layout_id; // id of VulkanVertexLayout variant
    uint32_t               flags;            // VkPipelineCreateFlags
    uint8_t  topology;
    uint8_t  cull_mode;
    uint8_t  front_face;
    uint8_t  blend_enable;
    uint8_t  color_write_mask[4];
    uint8_t  dynamic_state_mask;  // which of viewport/scissor are dynamic
    uint16_t reserved;
    uint32_t descriptor_set_layout_ids[8];
    uint32_t push_constant_hash;
};
```

Design rules, all of them load-bearing:

* **Hash the state you actually pass to `vkCreateGraphicsPipelines`, not a caller-supplied
  string.** Today's `getOrCreatePipeline(key, creator)` trusts the caller's string
  (`shader_manager.cpp:215`), so two call sites that build identical state under different names
  produce two pipelines. Build the key from the `VkGraphicsPipelineCreateInfo` fields that are
  **not** `VK_DYNAMIC_STATE_*` — i.e. in `vulkan_shader_manager.cpp`, which is the only place that
  has the `Vk*` types. ANGLE keeps the hashable descriptor in `vk_cache_utils.h` (backend-local)
  for the same reason.
* **Exclude everything dynamic.** `make_viewport_scissor_state()`
  (`vulkan_shader_manager.cpp:239-250`) leaves `pViewports`/`pScissors` null because both are
  dynamic, and says so explicitly: *"That keeps one pipeline valid for every surface size instead
  of forcing a rebuild on resize"*. Viewport and scissor therefore **must not** be in the key.
  This is the single most valuable property of the current pipeline state, and it is why resize
  does not need a pipeline rebuild at all — see the invalidation table.
* **`PipelineLayoutDesc` is part of the key**, because `build_pipeline_layout()` creates fresh
  `VkDescriptorSetLayout`s and a `VkPipelineLayout` per pipeline today
  (`vulkan_shader_manager.cpp:181-237`) and those objects are baked in. Two pipelines that differ
  only in descriptor layout are different pipelines.
* **Refcount entries.** `PipelineEntry` owns `VkPipeline`, `VkPipelineLayout` and the set layouts;
  the LRU cannot evict a referenced entry, and `VulkanShaderManager`'s destructor
  (`:354-379`) already destroys them in the right order (set layouts → layout → pipeline).

### L2 — transition table

The short-term memory in front of L1: for the currently bound pipeline, remember the last few
`(state-delta, pipeline)` pairs so a "same program, one uniform changed" switch costs a map hit
instead of an L1 hash.

**This is the level with the worst risk/reward in the whole design, and the reason is specific
to Minecraft.** The access pattern that makes a transition table pay is locality: the same
program drawn over and over with small state changes. Minecraft's actual pattern is close to the
opposite — biome × light level × weather × block type × shader-pack pass produces a
**many-to-many** scatter where consecutive draws share a program only by accident. A
transition table under many-to-many degrades to "an extra hash plus a table you must keep valid",
which is strictly worse than plain L1.

Therefore: **build L2 only after measuring the reuse ratio** (§5, Stage 4). If
`distinct_programs_per_frame / draw_calls_per_frame` is low (say < 0.2), L1 alone is the right
answer and L2 should be dropped permanently.

The hook point already exists: `StateManager::bindPipeline()` (`state_manager.h:27`) →
`VulkanStateManager::onBindPipeline()` (`vulkan_state_manager.h:157`), with
`VulkanStateManager::PipelineResolver` (`:97-107`) already resolving a pipeline handle to
`{VkPipeline, VkPipelineLayout}`. L2 is a memo in front of `PipelineResolver`, not a new
mechanism.

### L3 — currently bound handle

`StateManager::getBoundPipeline()` (`state_manager.h:38`) plus
`VulkanStateManager::bound_pipeline_layout()` (`vulkan_state_manager.h:171`). Two jobs:

1. Skip redundant `vkCmdBindPipeline` when the resolved handle is already bound.
2. **Be the swap-in point for the background compiler** (§5, Stage 2): when an optimized
   pipeline finishes, the L1 entry's handle changes, and the next `bindPipeline` for that key
   picks up the new handle for free. ANGLE does exactly this: *"Eventually, future calls to
   `PipelineHelper::getPreferredPipeline` would end up scheduling the task and observing its
   termination. At that point, the previous handle (from linked pipelines) is replaced by the
   handle created by the thread (a monolithic pipeline)."*

`VulkanStateManager` already caches `pipeline_layout_` *separately* from the pipeline
(`:185-188`: *"Cached so onBindDescriptorSets always names the layout of the pipeline that is
actually bound"*) — which means a naive handle swap would desynchronise layout from pipeline.
The swap-in must update both atomically inside `PipelineResolver`.

### L4 — render pass compatibility (not a cache, but a cache correctness dependency)

`create_render_pass()` destroys the old pass and builds a new one on every `create_swapchain()`
(`vulkan_renderer.cpp:530-534`), while every pipeline bakes `create_info.renderPass` in
(`vulkan_shader_manager.cpp:667`). Fix with a small refcounted render-pass cache keyed by the
attachment description:

```cpp
// key = { format, samples, loadOp, storeOp, stencilOps, initialLayout,
//         finalLayout, refCount }
// (i.e. exactly the VkAttachmentDescription + the subpass description)
```

* On `create_swapchain()`, look up by the new `swapchain_format_` / `swapchain_extent_`-independent
  description. If it hits, keep the existing `VkRenderPass` and **do not** destroy it.
* Increment the refcount for each live `VkFramebuffer`; decrement in `destroy_swapchain()`.
* Only destroy when the refcount reaches zero — and never before `vkDeviceWaitIdle()`
  (`vulkan_renderer.cpp:586` and `:610` already do this correctly).

This is why resize needs **no** pipeline invalidation at all: format is unchanged on a rotate or
resize, so the key hits, `render_pass_` is unchanged, and every L1 key stays valid.

---

## 4. Specialization constants

### 4.1 Where they legally apply

Only at pipeline creation, through `VkSpecializationInfo`
([spec](https://docs.vulkan.org/spec/latest/chapters/pipelines.html)):

> Specialization constants are a mechanism whereby constants in a SPIR-V module can have their
> constant value specified at the time the `VkPipeline` is created.

`VulkanShaderManager` already models this correctly: the base records name/value pairs and the
backend consumes them in `onCreateGraphicsPipeline`/`onCreateComputePipeline` via
`build_specialization()` (`vulkan_shader_manager.cpp:87-116`), and the header explains why
(`vulkan_shader_manager.h:117-122`). Keep that split.

Also note, because it removes a common worry: *"It is legal for a SPIR-V module with
specializations to be compiled into a pipeline where no specialization information was
provided"* — unspecialised defaults are legal, so a shader can be created once and specialised
later per pipeline.

### 4.2 The rule that decides everything: what may be a spec constant

ANGLE states it plainly in its pipeline doc:

> Simultaneously, ANGLE keeps the number of specialization constants to a minimum, to avoid
> recreating pipelines on state that might realistically change during the lifetime of the
> application.
> — [PipelineCreation.md](https://github.com/google/angle/blob/main/src/libANGLE/renderer/vulkan/doc/PipelineCreation.md)

Therefore:

| Value | Spec constant? | Reason |
|---|---|---|
| Android pre-rotation angle | **Yes** | Changes at most twice per session (rotate the device). This is the first use case (§4.3). |
| Swapchain format / render pass id | **No** | Not a shader constant. |
| Descriptor set layout ids | **No** | Pipeline-layout state, not shader state. |
| Vertex layout / stride | **No** | Baked into `VkPipelineVertexInputStateCreateInfo`, not the shader. |
| **Light level** | **No** | 16 levels × biomes = pipeline explosion. Must be a uniform. |
| **Biome id** | **No** | Same. One `int` uniform is free; one pipeline per biome is not. |
| Weather / time of day | **No** | Same. |
| Shader-pack feature flags | Only if the count is tiny | Each distinct value is a distinct pipeline. Prefer uniforms; a compile-time `#define` is worse — it changes the source, so it changes the shader cache key ([`shader-system.md`](shader-system.md) §4). |

The generalisation: **a specialization constant is for a value that changes at *activity* or
*orientation* granularity, never at *content* granularity.** Minecraft content granularity is
per-block-per-frame; that is a uniform buffer.

### 4.3 First use case: Android pre-rotation

The problem. `create_swapchain()` sets
`createInfo.preTransform = capabilities.currentTransform` (`vulkan_renderer.cpp:459`). The spec:

> `preTransform` is a `VkSurfaceTransformFlagBitsKHR` value describing the transform, relative to
> the presentation engine's natural orientation, applied to the image content prior to
> presentation. **If it does not match the `currentTransform` value returned by
> `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`, the presentation engine will transform the image
> content as part of the presentation operation.**
> — [spec, VkSwapchainCreateInfo](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/chapters/VK_KHR_surface/wsi.adoc)

Reading that precisely: with `preTransform = currentTransform`, the compositor does **not**
rotate for you. The image is handed over in the device's native orientation, and the application
must render pre-rotated. If `currentTransform` is not identity and we render in unrotated window
coordinates, SurfaceFlinger performs an extra full-screen rotation blit every frame — a real,
measurable cost on tile-based mobile GPUs, and one that also undoes any MSAA resolve you paid for.

ANGLE's version of the same requirement, and the clearest statement of it anywhere:

> Use the surface's transform. For many platforms, this will always be identity (ANGLE does not
> need to do any pre-rotation). However, when `surfaceCaps.currentTransform` is not identity, the
> device has been rotated away from its natural orientation. In such a case, ANGLE must rotate all
> rendering in order to avoid the compositor (e.g. SurfaceFlinger on Android) performing an
> additional rotation blit. In addition, ANGLE must create the swapchain with
> `VkSwapchainCreateInfoKHR::preTransform` set to the value of `surfaceCaps.currentTransform`.
> — [ANGLE SurfaceVk.cpp](https://github.com/google/angle/blob/main/src/libANGLE/renderer/vulkan/SurfaceVk.cpp)

ANGLE also notes that for 90°/270° the aspect ratio changes, so the swapchain extents, viewport,
scissor, render area and any depth attachment must all be swapped, and then `gl_Position` is
rotated in the vertex shader "the rendering will look the same as if no pre-rotation had been
done".

**The plan**, three parts:

1. **Stash the transform.** Add `VkSurfaceTransformFlagBitsKHR surface_transform_` to
   `VulkanRenderer`, set in `create_swapchain()` next to `preTransform` (`vulkan_renderer.cpp:459`).
   Expose it through the new public accessor block alongside `device()`/`renderPass()`
   (see [`shader-system.md`](shader-system.md) §0).
2. **Declare one spec constant in the vertex shader.**
   ```glsl
   // SpecId 0 = identity, 1 = rotate 90, 2 = rotate 180, 3 = rotate 270.
   layout(constant_id = 0) const int uPreRotation;
   ```
   `OpSpecConstant` ids are assigned by the *declaration order* glslang emits, which is why this
   needs reflection, not guessing (§4.4). Put the actual rotation in `gl_Position` right after the
   MVP multiply:
   ```glsl
   if (uPreRotation == 1) { gl_Position.xy = vec2(-gl_Position.y,  gl_Position.x); }
   else if (uPreRotation == 2) { gl_Position.x  = -gl_Position.x; gl_Position.y = -gl_Position.y; }
   else if (uPreRotation == 3) { gl_Position.xy = vec2( gl_Position.y, -gl_Position.x); }
   ```
   ANGLE gates the equivalent code behind exactly this mechanism: *"The translator outputs some
   feature code conditional to Vulkan specialization constants, which are resolved at draw-time.
   For example, for emulating Dithering and Android surface rotation."*
3. **Swap the fixed-function state.** For 90°/270°, swap `swapchain_extent_` into the
   `imageExtent` used for `vkCreateSwapchainKHR`, swap the width/height in
   `create_swapchain_framebuffers()` (`vulkan_renderer.cpp:520-521`), and swap the viewport/scissor
   in `VulkanStateManager::setViewport`/`setScissor`. The render pass is unaffected (one colour
   attachment, no depth — `make_color_blend_state()`, `vulkan_shader_manager.cpp:252-267`), which
   is one of the reasons the L4 render-pass cache is cheap here.

**Interaction with the cache key.** The rotation angle is a pipeline variant, so it lives in
`PipelineStateKey::vertex_spec_hash` (§3) — *not* in the shader-module key, and *not* in the ESSL
text cache key. Concretely: one `VkShaderModule`, four pipelines. That is the entire point of
using a spec constant instead of four `#define`d shader variants, which would have produced four
shader cache entries, four `glslang` invocations and four disk artifacts.

One subtlety worth deciding now: the rotation is a property of the *swapchain*, not of the
material. So it is best modelled as one value applied at pipeline-creation time rather than as
per-draw state — there is no way to change it without a new pipeline, and there should not be.
`VulkanRenderer::onResize` already takes `frame_mutex_` (`:794`) and
`recreate_swapchain()` does `vkDeviceWaitIdle()` (`:610`), which is the right (and only) point to
observe a transform change.

### 4.4 Fixing specialization-constant id resolution before adding any

This is a silent-wrong-rendering bug, not a robustness nicety.

`VulkanShaderManager::SpecializationConstantIdResolver` exists precisely because *"Vulkan
addresses constants numerically, so resolving names needs a reflection pass (SPIRV-Tools /
spirv-cross / glslang reflection) that is not linked into this build"*
(`vulkan_shader_manager.h:71-77`). The fallback (`vulkan_shader_manager.cpp:100-104`) assigns ids
in insertion order and logs a warning.

The spec says what a wrong id does:

> If a `constantID` value is not a specialization constant ID used in the shader, that map entry
> does not affect the behavior of the pipeline.
> — [spec, Specialization Constants](https://docs.vulkan.org/spec/latest/chapters/pipelines.html)

So a misresolved id produces a pipeline that **compiles successfully and renders incorrectly**.
There is no validation error, no `VkResult` failure, and no crash. Only `vkCmdSetViewport` aside,
the frame is just wrong. This is why pre-rotation must not ship before the resolver does.

SPIRV-Cross already has the reflection we need, and it is free — `spirv-cross-c` is already
linked for the ESSL path:

```cpp
// renderer/common/shader_translator.cpp -- run once per translated module, on the
// worker thread that already has the SPIR-V in hand.
bool resolve_spec_constant_ids(const std::vector<uint32_t>& spirv,
                               std::unordered_map<std::string, uint32_t>& out) {
    spvc_context ctx = nullptr;
    if (spvc_context_create(&ctx) != SPVC_SUCCESS) return false;

    spvc_parsed_ir ir = nullptr;
    spvc_compiler  c  = nullptr;
    if (spvc_context_parse_spirv(ctx, spirv.data(), spirv.size(), &ir) != SPVC_SUCCESS ||
        spvc_context_create_compiler(ctx, SPVC_BACKEND_NONE, ir,
                                     SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &c) != SPVC_SUCCESS) {
        spvc_context_destroy(ctx);
        return false;
    }

    const spvc_specialization_constant* constants = nullptr;
    size_t count = 0;
    if (spvc_compiler_get_specialization_constants(c, &constants, &count) != SPVC_SUCCESS) {
        spvc_context_destroy(ctx);
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        const char* name = spvc_compiler_get_name(c, constants[i].id);
        if (name != nullptr) {
            out.emplace(name, constants[i].constant_id);
        }
    }
    spvc_context_destroy(ctx);
    return true;
}
```

(`spvc_compiler_get_specialization_constants`, `spvc_compiler_get_name`:
[SPIRV-Cross `spirv_cross_c.h`](https://github.com/KhronosGroup/SPIRV-Cross/blob/master/spirv_cross_c.h).
Note `SPVC_BACKEND_NONE` — *"This backend can only perform reflection, no compiler options are
supported"* — which is exactly right and is why `SPIRV_CROSS_ENABLE_REFLECT=OFF` in the sibling
doc's CMake does not block this.)

Wiring:

1. Resolve during translation, in the worker thread, and store the numeric ids on the module.
2. `VulkanShaderManager` gets `std::unordered_map<std::string, uint32_t> spec_constant_ids_` in
   `ShaderModuleData` (next to the existing `specialization_constants`, `:123`), and
   `onAddSpecializationConstant` writes the resolved id there.
3. `build_specialization()` stops guessing. If a name is missing from the resolved map, **fail the
   pipeline** (`LOGE` + `return false`) unless an explicit `allow_guessed_spec_constant_ids`
   config flag is on. Never silently fall back in release builds.

This also unblocks the `VertexShader`/`FragmentShader` id spaces being merged: with reflection,
ids are per-module and unambiguous, so two shaders can both declare `SpecId 0` for different
things without collision.

### 4.5 Spec values in the key

`PipelineStateKey` carries `*_spec_hash` because different values are different pipelines. Compute
it as a 32-bit FNV-1a over `(sorted (constantID, value) pairs)`, sorted by `constantID` so the
hash is order-independent (the `build_specialization` entries array order is not
semantically meaningful, and `onAddSpecializationConstant` appends in call order —
`vulkan_shader_manager.cpp:814-831`).

---

## 5. Staged plan

Each stage has an entry condition and an exit measurement. **No stage starts before the previous
stage's exit measurement is in hand.** The instrumentation for Stage 0's measurement is the thing
that makes every later stage decidable, so it goes in first.

### Stage 0 — correctness (do this first, no measurement required)

Everything here is a bug fix or a prerequisite. None of it is an optimisation.

| # | Change | Where | Definition of done |
|---|---|---|---|
| 0.1 | Implement `create_pipeline_cache()`; call it from `create_logical_device()` after `vkCreateDevice` | `vulkan_renderer.h:210`, `vulkan_renderer.cpp:319-365` | `pipeline_cache_ != VK_NULL_HANDLE` after `initialize()`; logs its handle |
| 0.2 | Refcounted render-pass cache (§3, L4) | `vulkan_renderer.cpp:530-576` | After 20 rotations + resizes, no `VkPipeline` is invalidated and no pipeline is destroyed |
| 0.3 | Replace the string-keyed `Impl::pipeline_cache` with `PipelineStateKey` | `shader_manager.cpp:34`, `:215-229` | Two `createGraphicsPipeline` calls with identical state return the same handle |
| 0.4 | Fix `destroyPipeline`'s erase-first-match-then-`break` | `shader_manager.cpp:188-193` | Destroying a pipeline removes *all* cache entries referring to it |
| 0.5 | Make `getOrCreatePipeline` atomic (promise per key) | `shader_manager.cpp:215-229` | A 2-thread stress test creates exactly one pipeline per state key |
| 0.6 | Resolve spec-constant ids by reflection; make the guess an error | `vulkan_shader_manager.cpp:87-116`, `:814-831` | A shader with two spec constants in non-declaration order gets the right ids; a deliberately wrong name fails the pipeline instead of rendering wrong |
| 0.7 | Honour `enablePipelineCaching` / `enableAsyncShaderCompilation` | `renderer_config.h:44`, `:50` | Setting `enablePipelineCaching=false` disables L0–L2 and the behaviour is observable |
| 0.8 | Increment `FrameStats::shader_compilations` / `pipeline_compilations` | `renderer_base.h:156-157` | `nativeInitialize`/frame stats report non-zero after a first frame |
| 0.9 | Spec-constant + `PipelineStateKey` support for `uPreRotation` (§4.3) | new shader + `VulkanRenderer::surface_transform_` | Rotating the device shows no visible rotation, and `currentTransform != identity` shows no compositor blit |

**Exit measurement for Stage 0** (this is the number every later stage is judged against):

* Wall time around **every** `vkCreateGraphicsPipelines` / `vkCreateComputePipelines` call
  (`std::chrono::steady_clock`), bucketed per stage and per render pass.
* **Name the pipelines.** `onSetPipelineDebugName` already exists and is wired to
  `vkSetDebugUtilsObjectNameEXT` (`vulkan_shader_manager.cpp:803-812`), so a name like
  `chunk/terrain.opaque` lands in `VK_EXT_debug_utils` output and shows up in any Vulkan capture
  or system trace — which is how the time gets attributed to a specific material rather than to
  "some pipeline". Set the name from `PipelineStateKey` at creation, not from the caller.
* Report `pipeline_compilations` per frame: **p50, p99 and max must be 0 in steady state.** If
  they are not, there is an L1 key bug, not a performance problem. Fix that before Stage 2.

Reference devices for every measurement: Adreno 600/700, Mali Valhall, PowerVR Rogue — the three
vendors in README §"Supported GPUs", and the three that behave differently on pipeline creation.
Do not tune on one.

### Stage 1 — L0 persistence across runs

* **What:** save/load the `vkGetPipelineCacheData` blob with the header check (§3, L0).
* **Entry measurement (Stage 0's numbers):** cold-start total compile time, and the count of
  distinct pipelines created in the first 60 s.
* **Exit measurement:** same-session second run has **≥80% fewer** `vkCreateGraphicsPipelines`
  calls and **≥50% lower** cumulative create time, with the file size reported. If the hit rate
  is under 50%, the L1 key is unstable (something dynamic leaked into the key) — fix the key
  before tuning the blob.
* **Do not proceed if:** cumulative first-run compile time is already under 200 ms. Then the
  whole problem is small and Stages 2-4 are waste.

### Stage 2 — rate-limited background compiler with transparent swap-in

* **What:** one worker thread; a job per (state key, first request); a time budget per frame
  (start with **one** create per frame); results published into L1, picked up by L3 on the next
  `bindPipeline`.
* **Why not more threads:** ANGLE runs exactly one — *"Currently, only one pipeline creation job
  is allowed at a time. Additionally, posting these jobs is rate limited. This is primarily
  because the app is functional with reasonable efficiency with linked pipelines, so ANGLE avoids
  racing to provide monolithic pipelines as fast as possible."* Driver pipeline creation is
  serialised inside the driver anyway; more threads buy contention, not throughput.
* **Entry measurement:** Stage 0/1's per-call compile times, and **p99 frame time during a world
  load / shader-pack switch**.
* **Exit measurement:** no frame exceeds **2× the steady-state p99** while a background compile
  is in flight, and the hitch disappears from a 100-world-load trace. If a single create still
  exceeds the whole budget (some drivers do 50 ms+), that is when Stage 3 becomes mandatory
  rather than optional.
* **Invalidation:** jobs are keyed by `PipelineStateKey`; a job whose key is invalidated (§6) is
  dropped, not joined-and-applied. Never publish a pipeline built against a destroyed render
  pass — the L4 refcount in Stage 0 is what makes this checkable.

### Stage 3 — `VK_EXT_graphics_pipeline_library` split

* **What:** compile
  [vertex-input interface × pre-rasterization shaders × fragment shader × fragment output
  interface](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_EXT_graphics_pipeline_library.adoc),
  with a **separate `VkPipelineCache` per library type** (§3, L0). Fast-link for latency, then a
  background link with `VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT` and swap.
* **ANGLE's decomposition** (the model): at link time it pre-creates the *shader* library; at draw
  time it creates the *vertex input* and *fragment output* libraries, which are *"relatively
  cheap, and as they are shared between programs, there are also few of them"*, and links the
  three.
* **Entry measurement, all three required:**
  1. `graphicsPipelineLibrary` feature is enabled **and** the device advertises
     `graphicsPipelineLibraryFastLinking`. Without the property, fast linking is not cheap and
     this stage is a regression.
  2. Measured monolithic create time per pipeline type. If the fragment-shader library dominates
     (expected), the split pays; if vertex input dominates, it does not.
  3. Android version coverage of the extension in the field. If it is rare, ship the monolithic
     path as the default and keep this behind a capability check.
* **Exit measurement:** fast-link time under **2 ms** for the first frame a pipeline is needed,
  and — the one that actually matters — GPU frame time with fast-linked pipelines within
  **10%** of monolithic. The proposal's own guidance is to keep fast-linked pipeline work under
  ~10% of a frame and to swap in optimized pipelines as soon as they are ready; it warns the
  penalty *"could be as bad as a 50% penalty in the general case, with outliers performing even
  worse"*.
* **Dependency note:** the extension's libraries must be kept alive for the life of the pipelines
  that link them — *"The application must maintain the lifetime of a pipeline library based on the
  pipelines that link with it."* That means L1 entries hold library handles, and the LRU cannot
  evict a library that a live pipeline references.

### Stage 4 — transition table (L2)

* **Entry measurement, and this is the whole gate:** the reuse histogram from a real session —
  `distinct PipelineStateKey values per frame` ÷ `draw calls per frame`. Minecraft's
  biome × light-level × weather access pattern is many-to-many, so this ratio may well be high.
* **Exit measurement:** frame time improvement attributable to skipping `vkCmdBindPipeline`, on a
  frame where consecutive draws share a program. If the ratio from the entry measurement is above
  ~0.2, **drop this stage permanently** — L1 already gives the answer and L2 only adds invalidation
  surface.

### Stage 5 — pipeline derivatives (only if everything above is done and measured)

`VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT` / `DERIVATIVE_BIT`
([spec, Pipeline Derivatives](https://docs.vulkan.org/spec/latest/chapters/pipelines.html)):
*"A pipeline derivative is a child pipeline created from a parent pipeline, where the child and
parent are expected to have much commonality."*

Only justified once Stage 2's per-call numbers show which single state field dominates
(`basePipelineIndex` needs the parent to appear earlier in the same create array, and the parent
must have `ALLOW_DERIVATIVES`). Do not start here: derivatives need a *stable* parent chosen at
creation time, and our pipeline set is variant-driven, so the parent choice is itself a cache-key
problem.

---

## 6. Invalidation rules

Explicit and exhaustive. "Everything else is a cache hit" is the point.

| Event | L0 driver cache | L1 state map | L2 transition table | L3 bound handle | L4 render pass |
|---|---|---|---|---|---|
| Device created | fresh (no `pInitialData` unless a matching blob exists) | empty | empty | `VK_NULL_HANDLE` | create refcount 0 |
| Device lost / `shutdown()` | destroyed before `vkDestroyDevice` (`vulkan_renderer.cpp:192-199`) | destroyed in `ShaderManager::shutdown()` (`:55-68`) | dropped | cleared | all destroyed |
| **Surface resize / recreate** (`onResize` `:790`, `recreate_swapchain` `:603`) | **kept** — pipeline state does not change | **kept** — viewport/scissor are dynamic (`vulkan_shader_manager.cpp:239-250`) | **kept** | unchanged | refcount++, key hit on `VkAttachmentDescription`, existing pass reused |
| Swapchain **format** changes (HDR/SDR toggle) | kept | **all entries invalidated** (format is in the render-pass key) | cleared | cleared | new refcount 0 entry, old destroyed after `vkDeviceWaitIdle` |
| `set_binding_policy()` called (`vulkan_shader_manager.cpp:381`) | **kept** | **all invalidated** — layouts are baked into `PipelineLayoutDesc` | cleared | cleared | unchanged |
| `set_specialization_constant_id_resolver()` called (`:386`) | kept | **all invalidated** — resolved ids change the `VkSpecializationInfo` | cleared | cleared | unchanged |
| A spec-constant **value** changes | kept | key miss → new entry (4 entries max for `uPreRotation`) | cleared for that key | rebound | unchanged |
| `destroyPipeline(handle)` (`shader_manager.cpp:179`) | kept (L0 is the driver's business) | entry removed; **all** keys referring to it removed | entries for those keys dropped | cleared if it was bound | refcount-- |
| `destroyShader(handle)` (`shader_manager.cpp:107`) | kept | n/a | n/a | n/a | n/a |
| Shader destroyed while a live pipeline references it | **refused today** (`:111-117`) — keep this | — | — | — | — |
| `ShaderManager::shutdown()` | kept until device destroy | cleared | cleared | cleared | — |
| LRU evicts an unreferenced pipeline | kept | entry removed, `VkPipeline`/`Layout`/set-layouts destroyed | any entry for it dropped | cleared if bound | n/a |
| **New process / driver update** | blob loaded only if `vendorID`+`deviceID`+`pipelineCacheUUID` match; otherwise **discarded and deleted** | empty | empty | empty | fresh |
| `RendererConfig::enablePipelineCaching = false` | destroyed | empty (all misses) | disabled | cleared | unchanged |
| Memory pressure (`onMemoryPressure` `:911`) | blob dropped first | evict unreferenced, LRU order | cleared | cleared | unchanged |

Two rules that are easy to get wrong and are worth calling out:

* **Destroying a `VkShaderModule` after its pipeline exists is legal.** Vulkan pipelines hold
  their own reference to the compiled code. So the module cache and the pipeline cache have
  *different* lifetimes and must not share an eviction policy: the LRU may drop modules that live
  pipelines still reference, and must never drop a pipeline that a live frame slot will bind.
  `ShaderManager::destroyShader()`'s refusal (`:111-117`) is therefore a *module-cache* rule, not
  a pipeline rule — do not "relax" it and do not extend it to pipelines.
* **A background job's key can be invalidated while the job runs** (render pass replaced,
  binding policy changed). The job must be dropped at completion, not published. Checking the
  `PipelineStateKey` against the current generation at publish time is the whole mechanism — hence
  a monotonically increasing `generation_` counter bumped by every invalidating row above.

---

## 7. Do not implement yet

Each of these is real, is documented, and is the wrong next move. Reasons are concrete, not
taste.

| Do not build | Why not, yet |
|---|---|
| **`VK_KHR_pipeline_binary` / `VkPipelineBinaryKHR`** | Per-device opaque blobs, so disk usage scales with (pipeline count × device count) and every blob must be header-checked on load. It saves *nothing* Stage 1 does not already save — Stage 1 already persists the driver's own compiled form via `VkPipelineCache`. Revisit only if Stage 1's measured hit rate is poor **and** Stage 3 lands. |
| **`VK_EXT_shader_object`** | Not a pipeline-cache concern: it replaces pipeline objects with independently bindable per-stage objects, which changes the binding model wholesale. `RendererFeature::BindlessTextures` (`renderer_base.h:22`) and `RendererFeature::DescriptorIndexing` (`:23`) presuppose pipelines; the render pass here has one colour attachment and no input attachments (`vulkan_shader_manager.cpp:252-267`), so there is no fragment-shader-fetch win to chase. Revisit when there is more than one render pass. |
| **Mesh shaders** | README §Roadmap puts it at v1.1, and `ShaderStage::Mesh`/`Task` exist in `shader_manager.h:26-27` but nothing generates such SPIR-V. Adreno 700+/Mali Valhall coverage is narrow; the first `TBuiltInResource` fields for mesh limits are exactly the ones MobileGlues had to add by hand (`glsl_for_es.cpp:147-155`). |
| **An LFU/ML eviction policy** | We do not have the reuse histogram yet. LRU is one line and its failure mode (thrashing a hot pipeline) is visible in Stage 0's counters. Optimising an eviction policy against a workload we have not measured is how you end up with a policy tuned to the test scene. |
| **A cross-process shared-memory pipeline cache** | Tempting for launcher integration, but it needs a file lock, a version handshake, and crash recovery for a blob the spec says may be silently ignored on mismatch. The in-process LRU + a per-app-private blob gets most of the benefit with none of that. Revisit only if a launcher process (Pojav-style) is in scope. |
| **Shader-level `VK_EXT_graphics_pipeline_library` "shaders only" warm-up at startup** | ANGLE does warm the cache at link time, but against *placeholder* pipelines it knows will be used. Copper Oxide has no Minecraft asset/shader inventory yet (README: *"no asset/texture pipeline"*), so there is nothing to warm with. This becomes valuable the day the block/shader inventory exists — not before. |
| **A bespoke SPIR-V optimiser or `spirv-opt` pipeline** | `ENABLE_OPT=OFF` is deliberate: it is what lets glslang configure without vendoring SPIRV-Tools. ANGLE notes it does not use SPIRV-Tools for optimisation either and calls it a possible future improvement. Driver-side optimisation quality beats anything we would bolt on, and this is a size/build-time regression on the critical path. |
| **Pipeline cache sharing between backends** | There is nothing to share: the GLES backend has no pipeline objects at all (`GLESShaderManager::Pipeline::handle` is a backend-agnostic id mapping to a linked `GLuint` program, `gles_shader_manager.h:5-13`). A cross-backend "cache" would be a naming mistake. |

---

## 8. Sources

Upstream facts asserted in this document, with the URL that establishes each:

* Pipeline compilation cost; the 4-stage `VK_EXT_graphics_pipeline_library` split; monolithic-vs-linked tradeoff and the ~50% penalty; rate-limited single-job background compilation; "keep spec constants minimal"; `getPreferredPipeline` swap-in — <https://github.com/google/angle/blob/main/src/libANGLE/renderer/vulkan/doc/PipelineCreation.md>
* Module creation vs pipeline creation split; pre-rotation resolved through specialization-constant-conditional generated code — <https://github.com/google/angle/blob/main/src/libANGLE/renderer/vulkan/doc/ShaderModuleCompilation.md>
* `currentTransform`/`preTransform` and the SurfaceFlinger rotation blit; 90°/270° extent, viewport, scissor and render-area swapping — <https://github.com/google/angle/blob/main/src/libANGLE/renderer/vulkan/SurfaceVk.cpp>
* `VkPipelineCacheHeaderVersionOne` (32 bytes, LSB-first) and "results of pipeline compiles may depend on the vendor ID, device ID, driver version"; `vkGetPipelineCacheData`/VK_INCOMPLETE; "internally synchronized, and the same pipeline cache object can be used in multiple threads simultaneously"; invalid cache data is silently ignored; specialization constants applied at `VkPipeline` creation, defaults when unspecialised, and a wrong `constantID` being silently ignored; `VK_PIPELINE_CREATE_*` flags including `LINK_TIME_OPTIMIZATION_BIT_EXT` and `LIBRARY_BIT_KHR`; Pipeline Derivatives — <https://docs.vulkan.org/spec/latest/chapters/pipelines.html>
* `VK_GRAPHICS_PIPELINE_LIBRARY_*` bits, `graphicsPipelineLibraryFastLinking`, "avoid mixing different stage types in the same VkPipelineCache", library lifetime requirement, LINK_TIME_OPTIMIZATION vs fast-link guidance and the <10% advice — <https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_EXT_graphics_pipeline_library.adoc>
* `preTransform` semantics and "the presentation engine will transform the image content as part of the presentation operation" — <https://github.com/KhronosGroup/Vulkan-Docs/blob/main/chapters/VK_KHR_surface/wsi.adoc>
* `spvc_compiler_get_specialization_constants`, `spvc_compiler_get_name`, `SPVC_BACKEND_NONE` ("can only perform reflection, no compiler options are supported") — <https://github.com/KhronosGroup/SPIRV-Cross/blob/master/spirv_cross_c.h>
* `GraphicsPipelineDesc::hash(GraphicsPipelineSubset)` (`Complete` / `Shaders`), `PipelineCacheAccess` wrapping a `VkPipelineCache` with a mutex, `CreateMonolithicPipelineTask`, and the "no gaps or padding at the end ensures that hashing and memcmp checks" note on cache keys — <https://github.com/google/angle/blob/main/src/libANGLE/renderer/vulkan/vk_cache_utils.h>