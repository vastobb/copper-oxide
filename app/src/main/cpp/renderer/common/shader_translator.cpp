#include "shader_translator.h"

#include "shader_cache.h"

#include <android/log.h>

#include <mutex>

#include <mutex>
#include <string>
#include <utility>

#define LOG_TAG "CopperOxide-ShaderTx"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace copper {

const char* shaderStageName(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return "vertex";
        case ShaderStage::Fragment: return "fragment";
        case ShaderStage::Compute: return "compute";
        case ShaderStage::Geometry: return "geometry";
        case ShaderStage::TessellationControl: return "tessellation control";
        case ShaderStage::TessellationEvaluation: return "tessellation evaluation";
        case ShaderStage::Mesh: return "mesh";
        case ShaderStage::Task: return "task";
        case ShaderStage::RayGeneration: return "ray generation";
        case ShaderStage::AnyHit: return "any hit";
        case ShaderStage::ClosestHit: return "closest hit";
        case ShaderStage::Miss: return "miss";
        case ShaderStage::Intersection: return "intersection";
        case ShaderStage::Callable: return "callable";
    }
    return "unknown";
}

bool isShaderStageSupported(ShaderStage stage) {
    // Vertex, fragment, compute and geometry are expressible with what the
    // renderer already creates.
    //
    // Mesh, task and the ray-tracing stages need VK_EXT_mesh_shader and
    // VK_KHR_ray_tracing_pipeline. Copper Oxide probes for and enables neither,
    // so a module for one could never be used. Rejecting here produces "the
    // Vulkan backend does not support a mesh shader" instead of a glslang parse
    // error about the wrong thing - and, more importantly, never produces a
    // shader module that later dies at pipeline creation.
    //
    // Tessellation is legal GLSL and legal Vulkan, but the renderer has no
    // tessellation pipeline path or shader interface wired up, so a module for
    // one would be a module nothing can consume.
    switch (stage) {
        case ShaderStage::Vertex:
        case ShaderStage::Fragment:
        case ShaderStage::Compute:
        case ShaderStage::Geometry:
            return true;
        default:
            return false;
    }
}

// Defined in shaderc_translator.cpp, the only file that knows which compiler is
// linked in. Null would mean "no compiler", and every caller turns that into a
// clear message instead of pretending to translate.
ShaderTranslator* createDefaultTranslator();

namespace {

std::mutex g_include_resolver_mutex;
std::mutex g_translation_stats_mutex;
TranslationStats g_translation_stats;
std::once_flag g_translator_once;
ShaderTranslator* g_translator = nullptr;
ShaderTranslator* g_override = nullptr;
ShaderIncludeResolver g_include_resolver;

} // namespace

ShaderCache& spirvCache() {
    // Deliberately leaked rather than owned by a function-local static: the cache
    // is consulted from render threads and from callers with no renderer at all,
    // and destruction order during process teardown is not worth reasoning about
    // for a few megabytes the OS is about to reclaim anyway.
    static ShaderCache* cache = new ShaderCache();
    return *cache;
}

uint32_t toolchainVersion(ShaderTranslator* translator) {
    const std::string id = translator != nullptr ? translator->toolchainId() : std::string("none");
    uint32_t hash = 2166136261u;
    for (const char character : id) {
        hash ^= static_cast<uint8_t>(character);
        hash *= 16777619u;
    }
    return hash == 0 ? 1u : hash;
}

TranslationStats translationStats() {
    std::lock_guard<std::mutex> lock(g_translation_stats_mutex);
    return g_translation_stats;
}

void recordTranslation(bool success, uint64_t compile_ms, bool cache_hit, bool disk_hit) {
    std::lock_guard<std::mutex> lock(g_translation_stats_mutex);
    if (cache_hit) {
        if (disk_hit) {
            ++g_translation_stats.disk_hits;
        } else {
            ++g_translation_stats.memory_hits;
        }
        return;
    }
    g_translation_stats.total_ms += compile_ms;
    if (success) {
        ++g_translation_stats.compiled;
    } else {
        ++g_translation_stats.failures;
    }
}

void setShaderIncludeResolver(ShaderIncludeResolver resolver) {
    // Not guarded by the once flag: this is meant to be called during start-up,
    // before any compile, and the resolver is only read inside translate().
    std::lock_guard<std::mutex> lock(g_include_resolver_mutex);
    g_include_resolver = std::move(resolver);
}

ShaderIncludeResolver shaderIncludeResolver() {
    std::lock_guard<std::mutex> lock(g_include_resolver_mutex);
    return g_include_resolver;
}

ShaderTranslator* shaderTranslator() {
    std::call_once(g_translator_once, []() {
        if (g_override != nullptr) {
            g_translator = g_override;
        } else {
            g_translator = createDefaultTranslator();
        }
        if (g_translator != nullptr) {
            g_translator->initialize();
            LOGI("shader translator ready: %s", g_translator->toolchainId().c_str());
        } else {
            LOGW("no GLSL to SPIR-V compiler is linked into this build; the Vulkan backend "
                 "cannot accept GLSL source");
        }
    });
    return g_translator;
}

void setShaderTranslatorForTesting(ShaderTranslator* translator) {
    // Not guarded by the once flag: a test sets this before the first
    // shaderTranslator() call, and g_override is only read inside that call.
    g_override = translator;
}

} // namespace copper