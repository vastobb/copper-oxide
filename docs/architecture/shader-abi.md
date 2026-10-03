# Copper Oxide shader ABI

What Copper Oxide accepts as a shader, what it compiles it to, and what it does
not support. Written for the author of a shader that has to run on Copper Oxide,
on either backend.

This document describes the contract as implemented. Where a limitation is
temporary it says so and points at what would remove it.

---

## 1. The two backends take different things

| Backend | What you hand it | What compiles it |
|---|---|---|
| OpenGL ES 3.x | GLSL ES source | The device driver, via `glShaderSource` / `glCompileShader` |
| Vulkan | GLSL source | Copper Oxide, on the device, via shaderc |

This is not a convenience split. **Vulkan has no GLSL dialect.** A Vulkan shader
module is a SPIR-V binary, and SPIR-V is not something you can read or write by
hand. So on Vulkan, GLSL is *input to a compiler that runs on the phone*, and
only the resulting SPIR-V reaches the driver.

Consequences worth stating plainly:

* A shader that works on one backend may need a small change for the other. The
  input dialect is desktop GLSL on Vulkan and GLSL ES on OpenGL ES.
* Vulkan compilation costs CPU time on first use. The cache (§5) exists so you
  pay it once.
* OpenGL ES never goes through the compiler at all. Nothing about the GLES path
  changed when the compiler was added.

---

## 2. Supported source dialect

| Property | Vulkan | OpenGL ES |
|---|---|---|
| `#version` | `#version 450` (also 460, 330, 140) | `#version 300 es` |
| Target dialect | Vulkan 1.0 semantics, SPIR-V 1.3 | ES 3.0 semantics |
| Descriptor bindings | auto-assigned if absent | not applicable |
| Vertex attribute locations | **must be explicit** | may be implicit |

`#version 450` is the baseline because that is what modern desktop GLSL is and
what most shader packs are written against.

Older versions are accepted by the compiler. They are not *tested*: `#version 120`
era code uses `texture2D`, `gl_FragData` and `varying`, which need rewriting
before they compile. That rewriting is not implemented. See §7.

### Why vertex attribute locations must be explicit on Vulkan

Copper Oxide bakes its vertex format into the pipeline:

| Location | Type | Offset |
|---|---|---|
| 0 | `vec3` position, `R32G32B32_SFLOAT` | 0 |
| 1 | `vec4` colour, `R8G8B8A8_UNORM` | 12 |

Stride 28. A shader that reads vertex data must therefore declare
`layout(location = 0) in vec3 a_position;` and so on. Automatic location
assignment is deliberately **disabled**: it would hand out whatever location the
compiler chose and silently mismatch the vertex layout, which is far worse than
a link error telling you to add the qualifier.

---

## 3. Supported stages

| Stage | Vulkan | OpenGL ES |
|---|---|---|
| Vertex | yes | yes |
| Fragment | yes | yes |
| Compute | yes | ES 3.1+ only |
| Geometry | yes | ES 3.2+ only |
| Tessellation | **no** | **no** |
| Mesh, Task | **no** | **no** |
| Ray tracing | **no** | **no** |

Tessellation, mesh, task and ray-tracing stages are rejected *before* the
compiler runs, with the message naming the stage. That is deliberate:

* Mesh, task and ray tracing need `VK_EXT_mesh_shader` and
  `VK_KHR_ray_tracing_pipeline`. Copper Oxide probes for neither and enables
  neither, so a module for one could never be used.
* Tessellation is legal GLSL and legal Vulkan, but Copper Oxide has no
  tessellation pipeline path or shader interface wired up, so a module for one
  would be a module nothing could consume.

A rejection here is a refusal to build something that would die later inside the
driver, not a gap in the compiler.

---

## 4. Uniform and resource conventions

### Descriptors (Vulkan)

Plain GLSL carries no descriptor binding, and Vulkan requires one for every
resource. Copper Oxide therefore enables **automatic binding assignment**, so this
works:

```glsl
layout(set = 0, binding = 0, std140) uniform Camera {
    mat4 mvp;
} camera;
```

and this does too — the binding is assigned for you:

```glsl
uniform mat4 mvp;
```

What Copper Oxide does **not** yet do:

* There is no public API to allocate or update a descriptor set. A shader that
  declares resources will compile and pipeline, but there is no way to give it
  their contents from the public API yet. This is the next blocker after the
  compiler.
* There is no push-constant upload path in the public API.
* Descriptor type inference is not implemented: the pipeline layout assumes
  binding 0 is a uniform buffer and everything above it is a combined image
  sampler. That assumption is documented in `vulkan_shader_manager.cpp` and is
  expected to be replaced by reflection.

**Workaround today**: write resource-free shaders. A shader that reads only
`gl_VertexIndex`, `gl_FragCoord` and its own varyings runs end to end, which is
what the instrumented draw test does.

### Uniforms (OpenGL ES)

Compiled by the driver, so the ordinary ES rules apply. `uniform` blocks, plain
`uniform` and samplers all work as they would on any ES 3.x context.

---

## 5. Defines

`createShader(stage, source, defines = [])` takes preprocessor definitions. Two
forms are accepted:

| Form | Becomes |
|---|---|
| `"A=1"` | `#define A=1` |
| `"#define A 1"` | passed through verbatim |

Both are inserted **after the `#version` line**, because GLSL requires
`#version` to be the first thing on the first line. The same list behaves
identically on both backends.

One directive is also inserted, on Vulkan only:

```glsl
#extension GL_ARB_separate_shader_objects : require
```

Vulkan shader objects give a stage's inputs and outputs **separate** location
namespaces. That is what lets a vertex stage read `location = 0` and write
`location = 0` — the whole mechanism by which varyings reach the fragment stage.
Plain GLSL shares one namespace between them, so without this a shader gets:

```
'location' : overlapping use of location 0
```

Minecraft's own shaders declare this extension for the same reason. Declaring it
yourself is harmless; a repeated `#extension` with the same behaviour is legal.
It is available from GLSL 140, which is already the floor for Vulkan SPIR-V, so
it cannot reject a shader that would otherwise have compiled.

Defines are part of the cache key. Two shaders that differ only by defines are
two cache entries and two compilations, which is correct: they are two shaders.

---

## 6. Includes

`#include <name>` and `#include "name"` are resolved before compilation, by
Copper Oxide rather than by the compiler. Two reasons:

* A GLSL front end reads `#version` out of the **raw** text, before any
  preprocessing, so a `#version` arriving through an include is a hard parse
  error. Resolving first lets the included file's own `#version` be stripped,
  which is required and is exactly what Minecraft's `include/` directory needs.
* Minecraft writes its includes as `#include <minecraft:fog.glsl>`, where
  `minecraft:` is a URI scheme. A filesystem cannot answer that, so the scheme is
  stripped and the rest resolved under the directory given to
  `setShaderIncludeRoot()`. A `..` segment is refused: a shader has no
  legitimate reason to read outside its own asset tree.

A file is spliced **at most once** per translation unit. Textual concatenation
of a guarded header into two includers produces a duplicate definition, which is
a compile error, so splicing once is what makes ordinary include guards work — and
it terminates a cycle as a side effect.

An include that cannot be resolved is a reported error, never silently dropped.

---

## 7. Caching

Compiled SPIR-V is cached in memory, and optionally on disk under a directory
you supply via `openShaderCache()`.

The cache key is a content hash over: the stage, the source, the normalised
define set, the target API, and a digest of the compiler identity. Consequences:

* The same source always hits the same entry.
* Reordering or duplicating defines does **not** change the key.
* Changing the compiler, or its version, changes the key, so an artifact built by
  an older toolchain is never reused.
* Changing the source, the stage or the defines changes the key.

`shaderCompileStats()` reports compilations and hits so you can tell a working
cache from an absent one.

---

## 8. Known gaps

Stated as gaps rather than left to be discovered:

| Gap | Effect | What would close it |
|---|---|---|
| No descriptor set allocation or update | A shader declaring resources cannot be given data | Descriptor pool + layout binding API |
| No push-constant upload | Push constants cannot be set from the API | Same work as descriptors |
| No SPIR-V reflection | Descriptor types are inferred by a fixed heuristic | SPIRV-Cross reflection over the module |
| No legacy GLSL rewriting | `#version 120`/`130` shader packs fail to compile | A rewrite pass before the compiler |
| No `#version 110`/`120` shaders | Pre-1.17 shader packs fail: glslang refuses to target Vulkan SPIR-V below 140 | A legacy lifter (`varying`→`in`, `texture2D`→`texture`, loose uniforms→UBO) |
| No SPIR-V → ESSL path | A shader written for Vulkan cannot run on GLES | SPIRV-Cross `spvc_compile` to GLSL ES |
| No shader-pack macro layer | Iris/Sodium `#define IRIS_*` conventions are not provided | A macro provider driven by device and buffer type |

---

## 9. Test shaders

Every shader in this repository's tests was written clean-room for Copper Oxide.
Minecraft's own shaders and every published shader pack are proprietary or
copyleft, and none of them may be vendored into an MIT project. See
`THIRD_PARTY_NOTICES.md`.

The tests currently cover: a vertex stage deriving its position from
`gl_VertexIndex`, a fragment stage writing one output, a compute stage, define
injection, cache hits and misses, and four distinct failure modes. What they do
not yet cover is anything resembling a real Minecraft shader — no textures, no
fog, no lighting — because that depends on the descriptor gap above being
closed first.