# Copper Oxide Renderer

**High-performance Minecraft Java Edition rendering translation layer for Android**

Copper Oxide is a modern, modular rendering translation layer that enables Minecraft: Java Edition to run on Android devices with high performance. It supports both Vulkan and OpenGL ES backends with automatic GPU-specific optimizations.

## Features

### 🎯 Core Rendering
- **Dual Backend Support**: Vulkan 1.1+ and OpenGL ES 3.0+ backends
- **Automatic Backend Selection**: Intelligent GPU capability detection and optimal backend selection
- **GPU-Specific Optimizations**: Adreno, Mali, PowerVR, Apple, NVIDIA, AMD, Intel, Broadcom, Vivante, VeriSilicon
- **Modern Rendering Pipeline**: Dynamic rendering, synchronization2, timeline semaphores, descriptor indexing

### ⚡ Performance Optimizations
- **Multithreaded Rendering**: Command buffer recording on background threads
- **Async Shader Compilation**: Non-blocking SPIR-V compilation with caching
- **Efficient Resource Management**: Buffer/texture/shader pooling with lifetime tracking
- **Draw Call Optimization**: Batching, instancing, indirect draw support
- **Pipeline/Descriptor Caching**: Reduced state change overhead
- **Zero-Copy Texture Uploads**: Efficient buffer-to-texture transfers
- **Frame Pacing & VSync**: Low-latency and adaptive performance modes

### 🔧 Compatibility
- **Sodium Compatibility**: Full support for Sodium optimization mod
- **Iris/Shader Pack Compatibility**: GLSL→SPIR-V translation for shader packs
- **Fabric/Forge/NeoForge Rendering**: Compatible with major mod loaders
- **Vanilla & Legacy MC**: Supports modern and legacy Minecraft versions
- **Resource Pack Support**: Custom textures, models, shaders
- **Driver Workarounds**: Automatic detection and mitigation of GPU driver bugs

### 📱 Android Integration
- **Surface Lifecycle Management**: Proper EGL/Vulkan surface handling
- **Memory Pressure Handling**: Automatic resource trimming
- **Thermal Awareness**: Dynamic quality scaling based on device temperature
- **Battery Efficient Mode**: Reduced GPU frequency and frame rate
- **ARM64 Optimized**: NEON intrinsics, optimal memory alignment

### 🛠 Developer Experience
- **Clean JNI API**: Type-safe Kotlin wrapper with coroutines
- **Comprehensive Diagnostics**: Frame timing, GPU/CPU metrics, memory stats
- **Debug Rendering Mode**: Visual debugging overlays
- **Crash-Safe Cleanup**: Proper resource destruction on errors
- **Extensive Logging**: Configurable verbosity with structured output

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Copper Oxide Renderer                     │
├─────────────────────────────────────────────────────────────┤
│  Kotlin API (CopperOxideRenderer)                           │
│  ├── Lifecycle Management                                    │
│  ├── Configuration                                           │
│  ├── Frame Statistics                                        │
│  ├── GPU Capability Queries                                  │
│  └── Feature Detection                                       │
├─────────────────────────────────────────────────────────────┤
│  JNI Bridge (jni_bridge.cpp)                                │
│  ├── Renderer Factory & Registry                            │
│  ├── Surface Lifecycle                                       │
│  ├── Memory/Thermal Events                                   │
│  └── Stats/Info Queries                                      │
├─────────────────────────────────────────────────────────────┤
│  Renderer Backends                                           │
│  ├── Vulkan Backend (vulkan_renderer.cpp)                   │
│  │   ├── VMA Memory Allocator                               │
│  │   ├── Descriptor Management                              │
│  │   ├── Pipeline Cache                                     │
│  │   └── Async Compute Support                              │
│  └── OpenGL ES Backend (gles_renderer.cpp)                  │
│      ├── EGL 1.5 Context Management                         │
│      ├── GLES 3.2 Feature Set                               │
│      └── Extension Handling                                 │
├─────────────────────────────────────────────────────────────┤
│  Common Infrastructure (renderer/common/)                   │
│  ├── Buffer/Texture/Shader/Framebuffer Managers             │
│  ├── State/Command/Sync Managers                            │
│  ├── Resource Pool & Profiler                               │
│  └── GPU Capability Detection                               │
├─────────────────────────────────────────────────────────────┤
│  Platform Layer (platform/android/)                         │
│  ├── Android Platform Integration                           │
│  ├── EGL/Vulkan Surface Management                          │
│  ├── Asset/Shader Loading                                   │
│  └── System Callbacks                                       │
└─────────────────────────────────────────────────────────────┘
```

## Requirements

- **Android**: 8.0+ (API 26)
- **Architecture**: ARM64 (arm64-v8a)
- **GPU**: Vulkan 1.1+ or OpenGL ES 3.0+
- **RAM**: 2GB+ recommended (4GB+ for modded)

## Supported GPUs (Tested)

| Vendor | Architecture | Vulkan | GLES | Status |
|--------|-------------|--------|------|--------|
| Qualcomm | Adreno 600 | ✅ | ✅ | ✅ Optimized |
| Qualcomm | Adreno 700 | ✅ | ✅ | ✅ Optimized |
| Qualcomm | Adreno 800 | ✅ | ✅ | ✅ Optimized |
| ARM | Mali Midgard | ⚠️ | ✅ | ✅ Supported |
| ARM | Mali Bifrost | ⚠️ | ✅ | ✅ Supported |
| ARM | Mali Valhall | ✅ | ✅ | ✅ Optimized |
| ARM | Mali G715 | ✅ | ✅ | ✅ Optimized |
| Imagination | PowerVR Rogue | ⚠️ | ✅ | ✅ Supported |
| Imagination | PowerVR Furian | ⚠️ | ✅ | ✅ Supported |
| Imagination | PowerVR BXM | ⚠️ | ✅ | ✅ Supported |

## Building

### Prerequisites

- Android Studio Koala (2024.1.2)+
- JDK 17
- Android SDK 35
- NDK r27b
- CMake 3.22.1

### Build Commands

```bash
# Clone repository
git clone https://github.com/vastobb/copper-oxide.git
cd copper-oxide

# Build debug APK
./gradlew assembleDebug

# Build release APK
./gradlew assembleRelease

# Build release AAB (for Play Store)
./gradlew bundleRelease

# Run tests
./gradlew test
./gradlew connectedAndroidTest
```

### CI/CD

GitHub Actions automatically builds on every push to `main` and creates releases on version tags (`v*`).

## Integration

### As a Library (AAR)

```kotlin
// In your launcher's build.gradle.kts
dependencies {
    implementation("com.oxide.mc:copper-oxide:1.0.0")
}
```

### In Your Launcher

```kotlin
// Initialize renderer
val renderer = CopperOxideRenderer(context, RendererConfig.Performance)
renderer.setFrameCallback { stats ->
    // Update UI with frame stats
    Log.d("MC", "FPS: ${stats.fps}, Draw Calls: ${stats.drawCalls}")
}

renderer.setErrorCallback { error ->
    Log.e("MC", "Renderer error: $error")
}

// Start rendering with a Surface
val surface = surfaceView.holder.surface
if (renderer.initialize(surface)) {
    // Renderer is ready - Minecraft can now submit draw calls
}
```

### Native Integration (C++)

```cpp
#include <copper-oxide/renderer_base.h>
#include <copper-oxide/vulkan/vulkan_renderer.h>
#include <copper-oxide/gles/gles_renderer.h>

copper::RendererConfig config;
config.preferred_backend = copper::RendererBackend::Auto;
// ... configure

auto renderer = copper::RendererRegistry::instance().create_renderer(copper::RendererBackend::Auto);
renderer->initialize(config, native_window);
```

## Configuration

### RendererConfig Presets

```kotlin
// Maximum performance
RendererConfig.Performance

// Battery saving
RendererConfig.BatterySaver

// Debug/development
RendererConfig.Debug

// Custom
RendererConfig(
    preferredBackend = RendererBackend.VULKAN,
    targetFps = 120,
    lowLatencyMode = true,
    enableProfiling = true
)
```

### Feature Detection

```kotlin
if (renderer.supportsFeature(RendererFeature.BINDLESS_TEXTURES)) {
    // Use bindless texture rendering path
}

if (renderer.isExtensionSupported("VK_KHR_dynamic_rendering")) {
    // Use dynamic rendering
}
```

## Shader System

Copper Oxide includes a complete shader translation pipeline:

- **GLSL → SPIR-V**: Automatic compilation via glslang
- **Desktop GLSL → GLSL ES**: For OpenGL ES backend (via SPIRV-Cross)
- **Shader Caching**: Compiled SPIR-V cached to disk
- **Async Compilation**: Non-blocking background compilation
- **Precompilation**: Warm up shaders during loading screens

### Shader Compatibility

```glsl
// Minecraft shader example - automatically handled
#version 450
layout(set = 0, binding = 0) uniform sampler2D uTexture;
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = texture(uTexture, vTexCoord);
}
```

## Performance Targets

| Metric | Target |
|--------|--------|
| Frame Time (Vanilla 1.21) | < 16.67ms (60 FPS) |
| Frame Time (Modded) | < 33.33ms (30 FPS) |
| CPU Overhead | < 2ms/frame |
| GPU Memory | < 512MB (vanilla) |
| Startup Time | < 2s |
| Shader Compile (cached) | < 50ms |
| Memory Allocations/frame | 0 (steady state) |

## Roadmap

- [ ] **v1.1**: Mesh shader support (Adreno 700+/Mali Valhall)
- [ ] **v1.2**: Ray tracing support (Vulkan RT extensions)
- [ ] **v1.3**: Variable rate shading (foveated rendering)
- [ ] **v1.4**: ARM64EC Windows support (for emulation)
- [ ] **v2.0**: Native Vulkan renderer (like VulkanMod)

## Research & Inspiration

Copper Oxide incorporates techniques from:

- **MobileGlues**: GLSL→GLSL ES translation, shader caching
- **PojavLauncher/Amethyst**: GL4ES, LTW, Zink integration patterns
- **Zalith Launcher**: Kopper Zink, VulkanMod integration
- **Mesa/Zink**: OpenGL-on-Vulkan translation
- **ANGLE**: OpenGL-to-Vulkan translation layer
- **Sodium/Iris**: Modern MC rendering pipeline compatibility
- **VulkanMod**: Native Vulkan renderer architecture

## License

MIT License - See [LICENSE](LICENSE) for details.

## Author

**oxide-mc** - High-performance rendering for Minecraft on Android

---

*Copper Oxide: Where copper meets oxide - turning rendering challenges into performance gains.* 🔥