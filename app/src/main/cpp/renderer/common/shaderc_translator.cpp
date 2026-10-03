// shaderc-backed GLSL -> SPIR-V compiler.
//
// WHY shaderc rather than glslang wired up by hand: shaderc is Khronos' own
// packaging of glslang + SPIRV-Tools + SPIRV-Headers behind one stable C API,
// and it is what Android's own shader tooling links. Using it means one
// FetchContent and one CMake target instead of composing three projects - and
// the composition is the part that goes wrong, because each of the three wants
// to own SPIRV-Headers and that has to be resolved in exactly one order.
//
// The interface this implements is in shader_translator.h; nothing above it names
// shaderc, so the choice is confined to this file.

#include "shader_translator.h"

#include <android/log.h>
#include <shaderc/shaderc.hpp>

// Real SPIR-V validation via the SPIRV-Tools that shaderc vendors.
//
// WHY a CMake-provided define and not __has_include: shaderc links SPIRV-Tools
// PRIVATE, so its headers are deliberately NOT on a consumer's include path.
// CMakeLists adds them explicitly and sets this define, which makes the
// decision visible and deterministic instead of depending on whatever a
// transitive link happens to expose.
#ifdef COPPER_HAVE_SPIRV_TOOLS
#include <spirv-tools/libspirv.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Set by CMakeLists from the same value FetchContent pins. The fallback exists
// only so this file still compiles if the define is ever dropped.
#ifndef COPPER_SHADERC_TAG
#define COPPER_SHADERC_TAG "unpinned"
#endif

#define LOG_TAG "CopperOxide-ShaderTx"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {
namespace {

// SPIR-V magic, little endian. Checked before anything reaches the driver:
// vkCreateShaderModule rejects a bad module with a message that says nothing
// about the real problem.
constexpr uint32_t kSpirvMagic = 0x07230203u;

// Vulkan version the generated SPIR-V targets.
//
// 1.0 / SPIR-V 1.3, not 1.1 / 1.5, because Copper Oxide must run on a Vulkan 1.0
// device: the renderer pins the Vulkan ABI to 1.0 for Android's libvulkan.so and
// cannot assume 1.1 even though create_instance asks for it. Raising this is a
// one-line change once the renderer reports the device's real apiVersion.
constexpr uint32_t kTargetEnvVersion = shaderc_env_version_vulkan_1_0;
constexpr shaderc_spirv_version kTargetSpirv = shaderc_spirv_version_1_3;

shaderc_shader_kind to_shaderc_kind(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return shaderc_vertex_shader;
        case ShaderStage::Fragment: return shaderc_fragment_shader;
        case ShaderStage::Compute: return shaderc_compute_shader;
        case ShaderStage::Geometry: return shaderc_geometry_shader;
        case ShaderStage::TessellationControl: return shaderc_tess_control_shader;
        case ShaderStage::TessellationEvaluation: return shaderc_tess_evaluation_shader;
        default: return shaderc_vertex_shader;
    }
}

/// How many lines build_source_with_defines inserts.
///
/// Drives the correction applied to a compiler-reported line number, so it has
/// to match that function exactly. The always-present extension line is counted
/// first, and it was not: missing it left every reported line one too low.
int emitted_line_count(const std::vector<std::string>& defines, const std::string& preamble) {
    // The GL_ARB_separate_shader_objects extension, unconditionally.
    int count = 1;
    if (!preamble.empty()) {
        ++count;
    }
    for (const std::string& define : defines) {
        if (!define.empty()) {
            ++count;
        }
    }
    return count;
}

/// Render one define as a #define line.
///
/// Accepts both forms the API has always taken: a bare macro body such as "A=1"
/// and a complete directive such as "#define A 1". A complete directive is
/// passed through untouched.
///
/// The bare form is normalised to "A 1" rather than emitted as "A=1", because
/// GLSL's preprocessor warns about a missing space after the macro name and a
/// warning nobody can act on trains people to ignore diagnostics.
std::string render_define(const std::string& define) {
    if (define.front() == '#') {
        return define;
    }
    const size_t equals = define.find('=');
    // Only a simple object-like macro is rewritten. A function-like macro such
    // as "F(x)=x" must keep its parameter list attached to its name, so the first
    // '=' is only a name/body separator when no '(' precedes it.
    const bool object_like = equals != std::string::npos &&
                             define.find('(') == std::string::npos;
    if (!object_like) {
        return "#define " + define;
    }
    // "A=1" -> "#define A 1". The two spellings mean the same thing to the
    // preprocessor, but only one of them is free of a warning nobody can act
    // on.
    return "#define " + define.substr(0, equals) + " " + define.substr(equals + 1);
}

/// Insert the preamble, the defines and the one extension Vulkan GLSL needs,
/// after the `#version` directive.
///
/// GLSL requires `#version` to be the first thing on the first line, so none of
/// this can be prepended - prepending produces nothing but "#version directive
/// must occur on the first line of the shader". This is the same placement
/// GLESShaderManager uses, deliberately: one define list then behaves
/// identically on both backends.
///
/// A source with no `#version` gets the block at the very top, since there is
/// nothing to be after.
std::string build_source_with_defines(const std::string& source,
                                       const std::vector<std::string>& defines,
                                       const std::string& preamble) {
    std::string block;
    block.reserve(defines.size() * 24 + preamble.size() + 96);

    const auto emit = [&block](const std::string& line) {
        block += line;
        block += '\n';
    };

    // Vulkan shader objects give a stage's inputs and outputs SEPARATE location
    // namespaces, which is what lets a vertex stage read location 0 and write
    // location 0 - the entire mechanism by which varyings reach the fragment
    // stage. Plain GLSL shares one namespace between them, so without this
    // glslang rejects a shader with:
    //
    //   'location' : overlapping use of location 0
    //
    // Every shader Minecraft ships declares this extension for exactly that
    // reason, so requiring it here matches the shaders this has to accept, and a
    // source that already declares it is unaffected - a repeated #extension with
    // the same behaviour is legal.
    //
    // Unconditional rather than conditional on the source using location
    // qualifiers: the extension is available from GLSL 140, which is already the
    // floor for Vulkan SPIR-V, so it cannot reject anything that would otherwise
    // have compiled.
    emit("#extension GL_ARB_separate_shader_objects : require");

    if (!preamble.empty()) {
        // The preamble is directive text from the caller and is emitted with the
        // defines, so it lands in the same legal position.
        emit(preamble);
    }
    for (const std::string& define : defines) {
        if (!define.empty()) {
            emit(render_define(define));
        }
    }

    const size_t first_line_end = source.find('\n');
    if (first_line_end == std::string::npos) {
        return block + source;
    }
    if (source.compare(0, 8, "#version") != 0) {
        return block + source;
    }
    // +1 keeps the original first line and its newline intact, so the original
    // source is a contiguous substring of what is compiled.
    return source.substr(0, first_line_end + 1) + block + source.substr(first_line_end + 1);
}

/// Cheap structural checks that hold regardless of whether the validator is
/// linked in. These are not a substitute for spirv-val; they catch the two
/// mistakes that would otherwise reach the driver: something that is not SPIR-V
/// at all, and a truncated payload.
bool is_structurally_valid_spirv(const std::vector<uint32_t>& spirv) {
    // Five words is the header: magic, version, generator, bound, schema.
    return spirv.size() >= 5 && spirv[0] == kSpirvMagic;
}

/// First source line the compiler blamed, or -1.
///
/// glslang reports "ERROR: 0:LINE: message" and the number is relative to the
/// string it was given - which is the source with the define block inserted. That
/// shifts every line by the number of injected lines, so it is adjusted back out
/// before being reported against the source the caller wrote.
int extract_error_line(const std::string& diagnostics) {
    const size_t marker = diagnostics.find("ERROR: 0:");
    if (marker == std::string::npos) {
        return -1;
    }
    size_t cursor = marker + 9;
    int line = 0;
    bool any_digit = false;
    while (cursor < diagnostics.size() && diagnostics[cursor] >= '0' &&
           diagnostics[cursor] <= '9') {
        line = line * 10 + (diagnostics[cursor] - '0');
        ++cursor;
        any_digit = true;
    }
    if (!any_digit || cursor >= diagnostics.size() || diagnostics[cursor] != ':') {
        return -1;
    }
    return line - 1;  // glslang lines are 1-based
}


/// Remove a `#version` line from an included file.
///
/// Included Minecraft `.glsl` files carry their own `#version`, which desktop GL
/// tolerates and a GLSL front end does not: the directive must be the first thing
/// in the *translation unit*, and once it is spliced it is not. Only the first
/// `#version` of the top-level source survives; the caller's source is left
/// untouched by this.
void strip_version_directive(std::string* text) {
    std::string result;
    result.reserve(text->size());
    size_t cursor = 0;
    while (cursor < text->size()) {
        const size_t line_end = text->find('\n', cursor);
        const size_t next = line_end == std::string::npos ? text->size() : line_end + 1;
        const std::string line = text->substr(cursor, next - cursor);
        cursor = next;
        size_t probe = 0;
        while (probe < line.size() && (line[probe] == ' ' || line[probe] == '\t')) {
            ++probe;
        }
        if (line.compare(probe, 8, "#version") != 0) {
            result.append(line);
        }
    }
    *text = std::move(result);
}

/// Splice `#include` directives into the source.
///
/// The rule that matters: a file is spliced AT MOST ONCE per translation unit.
/// A naive line-concatenation resolver defeats every include guard, because by
/// the time the preprocessor runs the guarded body has already been duplicated
/// into each includer and the second copy is a redefinition error. Splicing once
/// gives the same result as a guard while also terminating cycles.
class IncludeSplicer {
public:
    // By value, not by const reference: the resolver arrives from a function
    // that returns one, so a reference member would dangle the moment the
    // constructor's argument was destroyed.
    explicit IncludeSplicer(ShaderIncludeResolver resolver) : resolver_(std::move(resolver)) {}

    /// Returns false and fills `error` when an include cannot be resolved.
    bool resolve(const std::string& source, std::string* out, std::string* error) {
        out->clear();
        std::vector<std::string> seen;
        if (!splice(source, out, &seen, 0, error)) {
            return false;
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 32;

    bool splice(const std::string& source, std::string* out,
                std::vector<std::string>* seen, int depth, std::string* error) {
        if (depth > kMaxDepth) {
            *error = "the include chain is deeper than " + std::to_string(kMaxDepth) +
                     " levels, which is almost certainly a cycle the once-only rule did not catch";
            return false;
        }

        size_t cursor = 0;
        while (cursor < source.size()) {
            const size_t line_end = source.find('\n', cursor);
            const size_t next = line_end == std::string::npos ? source.size() : line_end + 1;
            const std::string line = source.substr(cursor, next - cursor);
            cursor = next;

            std::string name;
            const bool is_include = parse_include(line, &name);
            if (!is_include) {
                out->append(line);
                continue;
            }

            if (std::find(seen->begin(), seen->end(), name) != seen->end()) {
                // Already spliced: an include guard, or a cycle. Either way the
                // body is present once, which is what both want.
                continue;
            }

            std::string text;
            if (!resolver_ || !resolver_(name, &text)) {
                *error = "cannot resolve the include \"" + name +
                         "\"; no include resolver answered for it";
                return false;
            }
            // Stripped here, from the included file only. It has to be here: the
            // caller's own #version must survive, and a whole-source sweep
            // cannot tell the two apart once the text is one string.
            strip_version_directive(&text);
            seen->push_back(name);
            if (!splice(text, out, seen, depth + 1, error)) {
                return false;
            }
        }
        return true;
    }

    /// Matches `#include <name>` and `#include "name"`, ignoring leading space.
    /// A directive with anything else after it is left alone, so a commented-out
    /// include inside a block comment is still removed with that block by the
    /// compiler's own lexer rather than being spliced here.
    static bool parse_include(const std::string& line, std::string* name) {
        size_t cursor = 0;
        while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t')) {
            ++cursor;
        }
        static const std::string kDirective = "#include";
        if (line.compare(cursor, kDirective.size(), kDirective) != 0) {
            return false;
        }
        cursor += kDirective.size();
        // Whitespace between the directive and the header name is legal and is
        // what everyone actually writes. Not skipping it made every include look
        // unrecognised, so it was passed through to the compiler, which then
        // rejected it with "unexpected include directive" - a failure that pointed
        // at the compiler instead of at the parser.
        while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t')) {
            ++cursor;
        }
        if (cursor >= line.size()) {
            return false;
        }
        const char open = line[cursor];
        if (open != '<' && open != '"') {
            return false;
        }
        const char close = open == '<' ? '>' : '"';
        const size_t close_pos = line.find(close, cursor + 1);
        if (close_pos == std::string::npos) {
            return false;
        }
        *name = line.substr(cursor + 1, close_pos - cursor - 1);
        return !name->empty();
    }

    ShaderIncludeResolver resolver_;
};


/// Rewrite glslang's line numbers into the caller's frame.
///
/// The compiler sees the caller's source with a block of directives injected
/// after `#version`, so every line it names is shifted. Reporting those numbers
/// unchanged would point an author at lines that do not exist in the file they
/// wrote, which is worse than reporting nothing.
///
/// Lines are 1-based and the injection always sits after line 1, so line 1 is
/// the only one whose number is unchanged.
std::string adjust_diagnostic_lines(const std::string& text, const std::string& file_name,
                                    int injected) {
    if (injected <= 0 || file_name.empty()) {
        return text;
    }
    std::string out;
    out.reserve(text.size());
    size_t cursor = 0;
    while (cursor < text.size()) {
        const size_t hit = text.find(file_name + ":", cursor);
        if (hit == std::string::npos) {
            out.append(text, cursor, std::string::npos);
            break;
        }
        out.append(text, cursor, hit - cursor);
        size_t digits = hit + file_name.size() + 1;
        int line = 0;
        bool any_digit = false;
        while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
            line = line * 10 + (text[digits] - '0');
            ++digits;
            any_digit = true;
        }
        const bool is_line = any_digit && digits < text.size() && text[digits] == ':';
        if (!is_line) {
            // Not a "name:line:" reference; copy the file_name through verbatim.
            out.append(file_name);
            cursor = hit + file_name.size();
            continue;
        }
        out.append(file_name);
        out.push_back(':');
        out += std::to_string(line > 1 ? line - injected : 1);
        cursor = digits;
    }
    return out;
}

class ShadercTranslator : public ShaderTranslator {
public:
    ShadercTranslator() = default;

    ShadercTranslator(const ShadercTranslator&) = delete;
    ShadercTranslator& operator=(const ShadercTranslator&) = delete;

    bool available() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return compiler_ != nullptr && compiler_->IsValid();
    }

    void initialize() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (compiler_ != nullptr) {
            return;
        }
        // Held by pointer rather than by value: shaderc::Compiler has a
        // destructor and a move constructor but no copy operations, so a
        // default-constructed-then-assigned member would be a lifetime bug
        // waiting to happen.
        compiler_ = std::make_unique<shaderc::Compiler>();
        if (!compiler_->IsValid()) {
            compiler_.reset();
            LOGE("shaderc failed to initialise; the Vulkan backend cannot accept GLSL");
            return;
        }
        LOGI("shaderc %s ready (Vulkan 1.0 semantics, SPIR-V 1.3)", COPPER_SHADERC_TAG);
    }

    std::string toolchainId() const override {
        // The tag is passed in by CMake from the same variable FetchContent
        // pins, so this string cannot drift away from the version actually
        // built. It is folded into the SPIR-V cache key, so bumping the tag
        // invalidates every artifact the previous compiler produced.
        std::string id = "shaderc-";
        id += COPPER_SHADERC_TAG;
        id += "-vk1.0-spv1.3";
#ifdef COPPER_HAVE_SPIRV_TOOLS
        id += "+spirv-tools";
#else
        id += "+no-validator";
#endif
        return id;
    }

    bool hasValidator() const override {
#ifdef COPPER_HAVE_SPIRV_TOOLS
        return true;
#else
        return false;
#endif
    }


    TranslationResult translate(const TranslationRequest& request) override {
        TranslationResult result;
        const auto started = std::chrono::steady_clock::now();

        if (!isShaderStageSupported(request.stage)) {
            result.error = std::string("the Vulkan backend does not support a ") +
                           shaderStageName(request.stage) + " shader";
            return result;
        }

        // Includes are resolved BEFORE the defines are injected, so a resolved
        // file's own #version is stripped and only the caller's survives.
        std::string preprocessed = request.source;
        if (request.resolve_includes && preprocessed.find("#include") != std::string::npos) {
            IncludeSplicer splicer(shaderIncludeResolver());
            std::string spliced;
            std::string include_error;
            if (!splicer.resolve(preprocessed, &spliced, &include_error)) {
                result.error = include_error;
                LOGE("%s shader include resolution failed: %s", shaderStageName(request.stage),
                     include_error.c_str());
                return result;
            }
            preprocessed = std::move(spliced);
        }

        const std::string source =
            build_source_with_defines(preprocessed, request.defines, request.preamble);

        std::string diagnostics;
        std::string input_file_name;
        std::vector<uint32_t> spirv;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (compiler_ == nullptr) {
                result.error =
                    "no GLSL to SPIR-V compiler is linked into this build, so the Vulkan backend "
                    "cannot accept GLSL source";
                return result;
            }

            // Options are built per compile rather than cached in a member:
            // shaderc::CompileOptions owns a raw handle with no copy
            // protection, so holding one across calls invites a double free.
            // Building it costs one small allocation next to an actual compile.
            shaderc::CompileOptions options;
            options.SetSourceLanguage(shaderc_source_language_glsl);
            options.SetTargetEnvironment(shaderc_target_env_vulkan, kTargetEnvVersion);
            options.SetTargetSpirv(kTargetSpirv);
            // WHY on: plain GLSL carries no descriptor bindings and Vulkan
            // requires one per resource. Without this, a Minecraft-style shader
            // declaring `uniform sampler2D tex` compiles and then fails at
            // pipeline creation with nothing to point at.
            options.SetAutoBindUniforms(true);
            // WHY off: Copper Oxide bakes vertex attribute locations 0 and 1
            // into its pipeline. Auto-assignment would hand out whatever
            // location glslang chose and silently mismatch the vertex layout,
            // which is worse than a link error asking the author for the
            // qualifier.
            options.SetAutoMapLocations(false);
            // No optimisation pass: the driver optimises at pipeline creation,
            // and running one on the device costs startup time for nothing.
            options.SetOptimizationLevel(shaderc_optimization_level_zero);

            // Named after the stage so a diagnostic says which stage failed.
            // "shader:3: error:" leaves a reader guessing; "fragment.glsl:3:
            // error:" does not. debug_name, when given, is the better label and
            // wins.
            input_file_name = request.debug_name.empty()
                                  ? std::string(shaderStageName(request.stage)) + ".glsl"
                                  : request.debug_name;
            const shaderc::SpvCompilationResult compiled = compiler_->CompileGlslToSpv(
                source, to_shaderc_kind(request.stage), input_file_name.c_str(),
                request.entry_point.empty() ? "main" : request.entry_point.c_str(), options);

            if (compiled.GetCompilationStatus() != shaderc_compilation_status_success) {
                diagnostics = compiled.GetErrorMessage();
                if (diagnostics.empty()) {
                    diagnostics = "the compiler rejected the shader without saying why";
                }
            } else {
                const uint32_t* const begin = compiled.cbegin();
                const uint32_t* const end = compiled.cend();
                if (begin == nullptr || end == nullptr || end <= begin) {
                    diagnostics = "the compiler reported success but produced no SPIR-V";
                } else {
                    spirv.assign(begin, end);
                }
            }
        }

        if (!diagnostics.empty()) {
            const int injected =
                emitted_line_count(request.defines, request.preamble);
            const int reported = extract_error_line(diagnostics);
            // glslang counted the injected lines; take them back out so the
            // number indexes the source the caller wrote.
            result.error_line = reported >= 0 ? reported - injected : -1;
            // And rewrite the numbers inside the message, because that text is
            // what a human actually reads.
            result.error = adjust_diagnostic_lines(diagnostics, input_file_name, injected);
            LOGE("%s shader compilation failed: %s", shaderStageName(request.stage),
                 diagnostics.c_str());
            return result;
        }

        if (!is_structurally_valid_spirv(spirv)) {
            result.error = "the compiler produced a payload that is not a SPIR-V module";
            LOGE("%s shader compilation produced invalid SPIR-V (%zu words)",
                 shaderStageName(request.stage), spirv.size());
            return result;
        }

        const TranslationResult validation = validate(spirv);
        if (!validation.success) {
            result.error = "SPIR-V validation failed: " + validation.error;
            LOGE("%s shader SPIR-V validation failed: %s", shaderStageName(request.stage),
                 validation.error.c_str());
            return result;
        }

        result.success = true;
        result.spirv = std::move(spirv);
        result.validated = validation.validated;
        result.compile_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count());
        LOGI("compiled a %s shader to %zu SPIR-V words in %llu ms%s",
             shaderStageName(request.stage), result.spirv.size(),
             static_cast<unsigned long long>(result.compile_ms),
             result.validated ? " (validated)" : " (structural check only)");
        return result;
    }

    TranslationResult validate(const std::vector<uint32_t>& spirv) override {
        TranslationResult result;
        if (!is_structurally_valid_spirv(spirv)) {
            result.error = spirv.empty()
                               ? "the payload is empty"
                               : "the payload does not start with the SPIR-V magic number";
            return result;
        }

#ifdef COPPER_HAVE_SPIRV_TOOLS
        // Validate(binary, size) reports only a bool in this SPIRV-Tools
        // revision. The text it would otherwise print goes to a message
        // consumer, which is installed here so a rejection can say WHY. A
        // validator that can only say "no" is a validator nobody can act on.
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_0);
        std::string message;
        tools.SetMessageConsumer(
            [&message](spv_message_level_t level, const char* /*source*/,
                       const spv_position_t& position, const char* text) {
                if (level == spv_message_level_error || message.empty()) {
                    message = "line " + std::to_string(position.line) + ": " +
                              (text != nullptr ? text : "unspecified validation failure");
                }
            });
        const bool ok = tools.Validate(spirv.data(), spirv.size());
        result.success = ok;
        result.validated = true;
        if (!ok) {
            // Word 1 of a SPIR-V module packs the version in its high 16 bits.
            const uint32_t spirv_version = spirv[1] >> 16;
            result.error = "spirv-val rejected this " + std::to_string(spirv.size()) +
                           "-word module (SPIR-V " + std::to_string((spirv_version >> 8) & 0xff) +
                           "." + std::to_string(spirv_version & 0xff) + ") against Vulkan 1.0: " +
                           (message.empty() ? std::string("no further detail") : message);
        }
        return result;
#else
        // Documented honestly: this build performs a structural check only and
        // `validated` stays false, so nothing above can mistake "not checked"
        // for "checked and good".
        result.success = true;
        result.validated = false;
        result.error = "spirv-val is not linked into this build; only a structural check ran";
        return result;
#endif
    }

private:
    // Guards compiler_: shaderc does not document its compiler as reentrant,
    // and a compile can be reached from any thread that owns a renderer.
    //
    // mutable because available() is const and still has to read compiler_
    // under the lock; std::lock_guard binds a non-const reference, so a
    // non-mutable member would not compile there.
    mutable std::mutex mutex_;
    std::unique_ptr<shaderc::Compiler> compiler_;
};

} // namespace

ShaderTranslator* createDefaultTranslator() {
    static ShadercTranslator* translator = new ShadercTranslator();
    return translator;
}

} // namespace copper