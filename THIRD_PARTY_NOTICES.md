# Third-party notices

Copper Oxide is MIT licensed (see `LICENSE`). It builds the following
third-party code into `libcopper-oxide.so` on Android. Their licences are
compatible with MIT; the attribution required by each is reproduced here.

---

## shaderc — Apache License 2.0

Fetched at configure time from `https://github.com/google/shaderc` at tag
`v2024.4` and linked statically.

shaderc is Google's packaging of glslang, SPIRV-Tools and SPIRV-Headers behind a
stable API. It is the GLSL to SPIR-V compiler this project uses on the Vulkan
backend.

shaderc's own repository carries no `.gitmodules`; it expects its dependencies
to be populated under `third_party/`. CMakeLists.txt therefore fetches all
three itself, in dependency order, at exactly the revisions shaderc's `DEPS`
file pins for `v2024.4` — so the set is the combination Khronos tests together
rather than three independently chosen versions.

| Project | Revision |
|---|---|
| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers) | `3f17b2af6784bfa2c5aa5dbb8e0e74a607dd8b3b` |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | `4d2f0b40bfe290dea6c6904dafdf7fd8328ba346` |
| [glslang](https://github.com/KhronosGroup/glslang) | `a0995c49ebcaca2c6d3b03efbabf74f3843decdb` |

shaderc incorporates material from the following works, each under its own
licence as recorded in shaderc's `THIRD_PARTY_NOTICES`:

| Component | Licence |
|---|---|
| glslang | BSD-3-Clause |
| SPIRV-Tools | Apache-2.0 |
| SPIRV-Headers | Apache-2.0 |
| abseil-cpp | Apache-2.0 |

### Apache License 2.0 (required notice)

```
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

### BSD-3-Clause (glslang)

```
Copyright (c) 2013 The Khronos Group Inc.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its contributors
  may be used to endorse or promote products derived from this software
  without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

---

## VulkanMemoryAllocator — Apache License 2.0

Fetched at configure time from
`https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz`
and linked statically. Header-only. Covered by the Apache-2.0 notice above.

---

## MobileGL / MobileGlues — LGPL-2.1 / LGPL-3.0

**Not vendored.** MobileGL's architecture informed the manager layout in this
project, but no code was copied. LGPL code cannot be linked into an MIT
library without relicensing the result, so the ideas were implemented
independently.

MobileGL is dual licensed LGPL-2.1 / LGPL-3.0. Anyone wishing to reuse its code
must comply with those licences.

---

## Code that must NOT be vendored into this project

These are referenced in the design notes as architectural references only. None
of their code is present in this repository, and none of it may be added:

| Project | Licence | Why it cannot be used here |
|---|---|---|
| Sodium (CaffeineMC/sodium) | Polyform Shield 1.0.0 | Not an OSI-approved licence; cannot be relicensed or combined with MIT. |
| Iris (IrisShaders/Iris) | LGPL-3.0 | Same relicensing problem as MobileGL. |
| PojavLauncher | GPL-3.0 | Copyleft; would force the whole project to GPL-3.0. |
| gl4es | GPL-3.0 | Copyleft. |
| Zalith | GPL-3.0 | Copyleft. |
| Mesa / Zink | MIT | Licence would permit it, but Mesa is a multi-megabyte desktop driver stack and is not vendored. |
| ANGLE | BSD-3-Clause | Licence would permit it; not vendored because its translation model targets desktop GL, not ES-on-Vulkan. |

Minecraft's own assets and shaders are proprietary and are **not** included.
Every shader in the test suite was written clean-room for this project. See
`docs/architecture/shader-abi.md`.