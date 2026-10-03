#pragma once

// GLSL -> SPIR-V compilation for the Vulkan backend.
//
// WHY this is separate from ShaderManager: compiling GLSL is a pure function of
// (source, defines, stage, target dialect) and needs no GPU, no device and no
// context. Keeping it separate means it can run on a worker thread, be tested
// without a renderer, and be swapped without touching the manager. The GLES
// backend does not use it at all - OpenGL ES compiles GLSL itself - which is why
// ShaderManager asks a backend whether it wants native GLSL before coming here.
//
// The concrete compiler is chosen at build time. Nothing above this header
// names shaderc or glslang, so the rest of the renderer is unaffected by that
// choice.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "shader_manager.h"

namespace copper {

class ShaderCache;

/// Resolves an `#include` to the text it names.
///
/// WHY this exists rather than relying on the compiler: a GLSL front end reads
/// `#version` out of the RAW text, before any preprocessing, so a `#version`
/// delivered through an include - or hidden behind `#ifdef` - is a hard parse
/// error rather than something a flag fixes. Resolving includes here also lets
/// Minecraft's `minecraft:fog.glsl` scheme be translated into something a plain
/// filesystem can answer, and it is the approach Iris uses.
///
/// Returns false when the name cannot be resolved; the caller then reports the
/// include as an error rather than compiling a shader with it silently dropped.
using ShaderIncludeResolver = std::function<bool(const std::string& name, std::string* text)>;

/// Installs the resolver used for `#include`. An empty function removes it, after
/// which an include is reported as unresolvable rather than ignored.
///
/// Process-wide, because the include root is a property of the running
/// application rather than of one shader. Set it during start-up, before the
/// first compile.
void setShaderIncludeResolver(ShaderIncludeResolver resolver);

/// The installed resolver, or an empty function. Read once per compile, so a
/// translation is never affected by a concurrent change.
ShaderIncludeResolver shaderIncludeResolver();

/// What a shader needs from the compiler, and what came back.
struct TranslationRequest {
    ShaderStage stage = ShaderStage::Vertex;
    std::string source;
    std::string entry_point = "main";
    /// Preprocessor definitions. An entry is either a bare macro body ("A=1",
    /// which becomes "#define A=1") or a complete directive ("#define A 1"),
    /// used verbatim. That is the contract GLESShaderManager already honours, so
    /// the same define list behaves identically on both backends.
    std::vector<std::string> defines;
    /// Dialect of the incoming source. "vulkan" means emit SPIR-V for
    /// vkCreateShaderModule; "gl" means emit SPIR-V as an intermediate for a
    /// later SPIR-V -> ESSL step.
    std::string target_api = "vulkan";
    /// Extra directive text emitted in the same legal position as the defines,
    /// immediately after the #version line.
    std::string preamble;
    /// Human-readable name for diagnostics and the cache.
    std::string debug_name;
    /// When false, `#include` is left in the source for the compiler's own
    /// includer. Copper Oxide resolves includes itself by default, because the
    /// compiler's front end cannot handle a `#version` arriving through one.
    bool resolve_includes = true;
};

struct TranslationResult {
    bool success = false;
    /// SPIR-V words, ready for vkCreateShaderModule.
    std::vector<uint32_t> spirv;
    /// Compiler diagnostics. On failure this is the primary message: it carries
    /// glslang's line/column annotated text.
    std::string error;
    /// Index of the first line of TranslationRequest::source the compiler
    /// blamed, or -1 when it named none. Adjusted back out of the internal
    /// string so it indexes the source the caller actually wrote.
    int error_line = -1;
    /// True when SPIR-V validation ran and passed. False when validation was not
    /// available in this build, so a caller can tell "valid" from "not checked".
    bool validated = false;
    /// Wall-clock compile time in milliseconds, for telemetry.
    uint64_t compile_ms = 0;
};

/// Compiles GLSL to SPIR-V and validates the result.
///
/// One instance is shared process-wide; every method is thread-safe. Note that
/// the underlying compiler has process-wide global state, so separate instances
/// do not isolate each other - documented rather than pretended away.
class ShaderTranslator {
public:
    virtual ~ShaderTranslator() = default;

    /// True when a compiler is linked into this build. When false, translate()
    /// fails with a clear message rather than pretending to work.
    virtual bool available() const = 0;

    /// Identifies the compiler and version. Feeds the cache key, so changing the
    /// compiler cannot silently reuse artifacts built by another one.
    virtual std::string toolchainId() const = 0;

    /// True when this build can run spirv-val. Reported separately from
    /// TranslationResult::validated so a caller can distinguish "checked and
    /// good" from "no validator in this build".
    virtual bool hasValidator() const = 0;

    /// Compiles. Never throws; every failure is reported in the result.
    virtual TranslationResult translate(const TranslationRequest& request) = 0;

    /// Validates an already-generated module. Used for SPIR-V that arrived from
    /// somewhere other than translate(), for example a caller-supplied binary.
    virtual TranslationResult validate(const std::vector<uint32_t>& spirv) = 0;

    /// One-time process setup. Safe to call more than once.
    virtual void initialize() = 0;
};

/// The process-wide translator. Created on first use; null means no compiler is
/// linked in, which every caller turns into a clear message.
ShaderTranslator* shaderTranslator();

/// Test seam: replaces the process translator. nullptr restores nothing - the
/// default is created once and cannot be replaced twice. Only for tests.
void setShaderTranslatorForTesting(ShaderTranslator* translator);

/// The process-wide compiled-SPIR-V cache.
///
/// WHY process-wide rather than one per ShaderManager: the key is a content
/// hash, so two managers asking for the same shader want the same artifact, and
/// one copy is both cheaper and the only way a cache hit can happen at all
/// across renderers. It also means a caller that pre-warms the cache through
/// compileToSpirv() and then creates a shader pays for one compilation.
ShaderCache& spirvCache();

/// Compilation and cache counters, process-wide for the same reason.
///
/// Counters rather than a status flag: the difference between "no shader was
/// ever compiled" and "the same one was compiled once and then served from the
/// cache" is the difference between a broken build and a working one, and it is
/// invisible in the output.
struct TranslationStats {
    uint64_t compiled = 0;     ///< full GLSL -> SPIR-V compilations run
    uint64_t memory_hits = 0;  ///< served from the in-memory cache
    uint64_t disk_hits = 0;    ///< served from the on-disk cache
    uint64_t failures = 0;     ///< compiler or validation failures
    uint64_t total_ms = 0;     ///< summed compile time
};
TranslationStats translationStats();

/// Updates the process-wide counters. `cache_hit` short-circuits the other
/// arguments, so a hit cannot also be counted as a compilation.
void recordTranslation(bool success, uint64_t compile_ms, bool cache_hit,
                       bool disk_hit);

/// Name used in diagnostics and cache keys.
const char* shaderStageName(ShaderStage stage);

/// True when the stage can be expressed as GLSL for Vulkan here at all. Mesh,
/// task and ray-tracing stages need extensions Copper Oxide does not enable, so
/// they are rejected before the compiler is invoked rather than producing a
/// confusing parse error.
bool isShaderStageSupported(ShaderStage stage);

} // namespace copper