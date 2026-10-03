package com.oxide.mc.copperoxide

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.oxide.mc.copperoxide.renderer.CopperOxideRenderer
import com.oxide.mc.copperoxide.renderer.SpirvCompilation
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * GLSL to SPIR-V, validated, with no GPU involved.
 *
 * [VulkanShaderDrawTest] covers the rest of the Vulkan path, but it cannot run
 * here: the CI emulator's guest graphics API is OpenGL ES 3.0 via SwiftShader, so
 * there is no Vulkan device and every test in it is skipped. A suite that skips
 * proves nothing, and a green job over nine skips looks exactly like a pass.
 *
 * Compilation needs no device, no context and no surface, so it is tested
 * directly. None of these tests can skip: if the compiler is missing, broken, or
 * produces something that is not SPIR-V, this suite fails. That is the property
 * the Vulkan suite cannot give.
 *
 * Everything here uses the production path:
 * `ShaderManager::createShaderFromGLSL` -> `translateGlslToSpirv` ->
 * `ShaderTranslator::translate` -> validate -> cache, minus the two steps that
 * genuinely need a device (creating a `VkShaderModule` and a pipeline).
 */
@RunWith(AndroidJUnit4::class)
class ShaderTranslationTest {

    private lateinit var renderer: CopperOxideRenderer

    @Before
    fun setUp() {
        renderer = CopperOxideRenderer(InstrumentationRegistry.getInstrumentation().targetContext)
    }

    @After
    fun tearDown() {
        renderer.setShaderIncludeRoot("")
        renderer.shutdown()
    }

    // -----------------------------------------------------------------------
    // The core claim: GLSL really is compiled to SPIR-V
    // -----------------------------------------------------------------------

    /**
     * A Minecraft-shaped fragment shader compiles to a real, valid SPIR-V module.
     *
     * Clean-room, but deliberately shaped like what Minecraft's own shaders
     * use, because that is the workload: a named `std140` uniform block, a
     * sampler with no explicit binding, located inputs and outputs, a fog mix
     * and an alpha test. See docs/architecture/shader-abi.md.
     *
     * Asserted in order of what could be wrong: it compiled, it is SPIR-V, the
     * header is sane, and the validator accepted it.
     */
    @Test
    fun minecraftShapedFragmentShaderCompilesToValidSpirv() {
        val result = renderer.compileToSpirv(
            CopperOxideRenderer.ShaderStage.Fragment,
            FRAGMENT,
            arrayOf("MC_VERSION=12104"),
        )

        assertTrue("the shader did not compile: ${result.error}", result.succeeded)
        assertTrue("the result is not SPIR-V", result.looksLikeSpirv)
        assertTrue("a module this small is suspicious: ${result.spirv.size} words", result.spirv.size > 20)
        // Word 1 packs the SPIR-V version in the high 16 bits. Vulkan 1.0
        // semantics means SPIR-V 1.0 to 1.3; anything above that needs a
        // capability Copper Oxide does not require a device to have.
        val version = result.spirv[1] ushr 16
        assertTrue("unexpected SPIR-V version $version", version in 1..3)
        assertTrue("spirv-val did not run or did not pass: ${result.error}", result.validated)
    }

    @Test
    fun minecraftShapedVertexShaderCompilesToValidSpirv() {
        val result = renderer.compileToSpirv(
            CopperOxideRenderer.ShaderStage.Vertex,
            VERTEX,
            arrayOf("MC_VERSION=12104"),
        )

        assertTrue("the vertex shader did not compile: ${result.error}", result.succeeded)
        assertTrue("the result is not SPIR-V", result.looksLikeSpirv)
        assertTrue("spirv-val did not run or did not pass: ${result.error}", result.validated)
    }

    /**
     * A define must reach the preprocessor.
     *
     * `COPPER_TEST_BRANCH` is only referenced inside a conditional, so the
     * shader compiles either way - but the generated SPIR-V differs, which is
     * what proves the define was actually applied rather than ignored.
     */
    @Test
    fun definesReachThePreprocessor() {
        val withoutDefine = renderer.compileToSpirv(
            CopperOxideRenderer.ShaderStage.Fragment,
            BRANCHING,
        )
        val withDefine = renderer.compileToSpirv(
            CopperOxideRenderer.ShaderStage.Fragment,
            BRANCHING,
            arrayOf("COPPER_TEST_BRANCH 1"),
        )
        val withOtherValue = renderer.compileToSpirv(
            CopperOxideRenderer.ShaderStage.Fragment,
            BRANCHING,
            arrayOf("COPPER_TEST_BRANCH 0"),
        )

        assertTrue(withoutDefine.succeeded)
        assertTrue(withDefine.succeeded)
        assertTrue(withOtherValue.succeeded)
        assertNotEquals(
            "a define had no effect on the generated module",
            withoutDefine.spirv.toList(),
            withDefine.spirv.toList(),
        )
        assertNotEquals(
            "defines with different values produced identical modules",
            withDefine.spirv.toList(),
            withOtherValue.spirv.toList(),
        )
    }

    /** Define ordering must not matter: two call sites that pass the same set in
     *  a different order are asking for the same shader. */
    @Test
    fun defineOrderAndDuplicatesDoNotChangeTheResult() {
        val ordered = renderer.compileToSpirv(
            CopperOxideRenderer.ShaderStage.Fragment,
            BRANCHING,
            arrayOf("COPPER_A 1", "COPPER_B 2"),
        )
        val shuffled = renderer.compileToSpirv(
            CopperOxideRenderer.ShaderStage.Fragment,
            BRANCHING,
            arrayOf("COPPER_B 2", "COPPER_A 1", "COPPER_A 1"),
        )

        assertTrue(ordered.succeeded)
        assertTrue(shuffled.succeeded)
        assertEquals(
            "define order leaked into the output",
            ordered.spirv.toList(),
            shuffled.spirv.toList(),
        )
    }

    @Test
    fun aComputeShaderCompiles() {
        val result = renderer.compileToSpirv(CopperOxideRenderer.ShaderStage.Compute, COMPUTE)
        assertTrue("the compute shader did not compile: ${result.error}", result.succeeded)
        assertTrue(result.looksLikeSpirv)
        assertTrue(result.validated)
    }

    // -----------------------------------------------------------------------
    // Validation is real, not a rubber stamp
    // -----------------------------------------------------------------------

    /**
     * The validator rejects a module that is not one.
     *
     * A validation step that accepts anything would pass every positive test
     * above while proving nothing, so this is the negative case that gives the
     * positives their meaning. Three inputs, each wrong in a different way: not
     * SPIR-V at all, valid magic with a truncated body, and a real module with a
     * corrupted instruction.
     */
    @Test
    fun validationRejectsThingsThatAreNotValidSpirv() {
        val good = renderer.compileToSpirv(CopperOxideRenderer.ShaderStage.Fragment, FRAGMENT)
        assertTrue(good.succeeded)
        assertTrue("the baseline module should validate", renderer.validateSpirv(good.spirv))

        assertFalse(
            "text that is not SPIR-V must be rejected",
            renderer.validateSpirv(intArrayOf(0x68746567, 0x206c6c73)), // "get llsh"
        )

        assertFalse(
            "a SPIR-V header with no body must be rejected",
            renderer.validateSpirv(intArrayOf(0x07230203, 0x00010300, 0, 1, 0)),
        )

        // A real module with a word replaced by an unknown opcode. Structural
        // checks cannot see this; the validator can.
        val corrupted = good.spirv.copyOf()
        corrupted[corrupted.size - 1] = 0xDEADBEEF.toInt()
        assertFalse(
            "a module with an invalid instruction must be rejected",
            renderer.validateSpirv(corrupted),
        )
    }

    // -----------------------------------------------------------------------
    // Failure reporting
    // -----------------------------------------------------------------------

    /** Malformed GLSL fails with a real diagnostic and no SPIR-V. */
    @Test
    fun malformedGlslFailsWithADiagnostic() {
        val result = renderer.compileToSpirv(
            CopperOxideRenderer.ShaderStage.Fragment,
            "this is not glsl at all",
        )

        assertFalse("malformed GLSL must not report success", result.succeeded)
        assertTrue("a failure must leave a diagnostic, got \"${result.error}\"", result.error.isNotBlank())
        assertTrue(
            "the diagnostic should name the stage, got \"${result.error}\"",
            result.error.contains("fragment", ignoreCase = true),
        )
        assertTrue("a failure must not return SPIR-V", result.spirv.isEmpty())
    }

    /**
     * A failure names the line.
     *
     * Only useful if the number refers to the caller's own source: the compiler
     * sees the source with an injected define block spliced in after `#version`,
     * so its line numbers are shifted by exactly that many lines. Getting this
     * wrong points a shader author at the wrong line, which is worse than no
     * line number at all.
     */
    @Test
    fun aFailurePointsAtTheCallersSourceLine() {
        // Three defines, so three lines are injected. The error is on line 7 of
        // the caller's source and must not be reported as line 10.
        val broken = """
            #version 450

            layout(location = 0) out vec4 frag_color;
            void main() {
                this_symbol_does_not_exist();
                frag_color = vec4(1.0);
            }
        """.trimIndent()
        val brokenLine = broken.lines().indexOfFirst { it.contains("this_symbol_does_not_exist") } + 1
        assertEquals(7, brokenLine)

        val result = renderer.compileToSpirv(
            CopperOxideRenderer.ShaderStage.Fragment,
            broken,
            arrayOf("A 1", "B 2", "C 3"),
        )

        assertFalse(result.succeeded)
        assertTrue("expected a line-annotated diagnostic, got \"${result.error}\"", result.error.contains(":"))
        val reported = LINE.findAll(result.error)
            .map { it.groupValues[1].toInt() }
            .firstOrNull()
        assertTrue("no line number in \"${result.error}\"", reported != null)
        assertEquals(
            "the reported line does not match the caller's source",
            brokenLine,
            reported,
        )
    }

    /** A valid shader still compiles after a failure: no poisoned state. */
    @Test
    fun aValidShaderCompilesAfterAFailure() {
        val bad = renderer.compileToSpirv(CopperOxideRenderer.ShaderStage.Fragment, "not glsl")
        assertFalse(bad.succeeded)

        val good = renderer.compileToSpirv(CopperOxideRenderer.ShaderStage.Fragment, FRAGMENT)
        assertTrue("a failure poisoned later compiles: ${good.error}", good.succeeded)
        assertTrue("a success must leave no error text", good.error.isEmpty())
    }

    // -----------------------------------------------------------------------
    // Includes
    // -----------------------------------------------------------------------

    /**
     * An included file must be spliced, and its own `#version` stripped.
     *
     * This is the case that matters for Minecraft: its shipped shaders are
     * almost entirely `#include <minecraft:NAME.glsl>` one-liners over a shared
     * header directory, and each of those headers starts with `#version`. A GLSL
     * front end requires `#version` to be the first directive in the translation
     * unit, so a spliced header that keeps one is a hard parse error - which is
     * exactly what happened before the splicer learned to strip it.
     */
    @Test
    fun anIncludedFileIsSplicedAndItsVersionStripped() {
        val header = java.io.File.createTempFile("copper-shader", ".glsl")
        try {
            header.writeText(SHARED_HEADER)
            renderer.setShaderIncludeRoot(header.parent)

            val result = renderer.compileToSpirv(
                CopperOxideRenderer.ShaderStage.Fragment,
                """
                #version 450
                #include <minecraft:${header.name}>
                layout(location = 0) out vec4 frag_color;
                void main() {
                    frag_color = copper_shared_tint();
                }
                """.trimIndent(),
            )

            assertTrue(
                "a shader with an include did not compile: ${result.error}",
                result.succeeded,
            )
            assertTrue(result.validated)
        } finally {
            header.delete()
        }
    }

    /**
     * A header included twice is spliced once.
     *
     * Textual concatenation of a guarded header into two includers produces a
     * duplicate definition, which is a compile error. Splicing each file at most
     * once per translation unit is what makes ordinary include guards work.
     */
    @Test
    fun aHeaderIncludedTwiceIsSplicedOnce() {
        val header = java.io.File.createTempFile("copper-shader", ".glsl")
        try {
            header.writeText(SHARED_HEADER)
            renderer.setShaderIncludeRoot(header.parent)

            val result = renderer.compileToSpirv(
                CopperOxideRenderer.ShaderStage.Fragment,
                """
                #version 450
                #include <minecraft:${header.name}>
                #include <minecraft:${header.name}>
                layout(location = 0) out vec4 frag_color;
                void main() {
                    frag_color = copper_shared_tint();
                }
                """.trimIndent(),
            )

            assertTrue(
                "including a header twice must be tolerated: ${result.error}",
                result.succeeded,
            )
        } finally {
            header.delete()
        }
    }

    /** An include nobody can resolve is an error, never silently dropped. */
    @Test
    fun anUnresolvableIncludeIsReported() {
        renderer.setShaderIncludeRoot("/nonexistent-copper-shader-root")

        val result = renderer.compileToSpirv(
            CopperOxideRenderer.ShaderStage.Fragment,
            """
            #version 450
            #include <minecraft:definitely_absent.glsl>
            layout(location = 0) out vec4 frag_color;
            void main() {
                frag_color = vec4(1.0);
            }
            """.trimIndent(),
        )

        assertFalse("an unresolvable include must not report success", result.succeeded)
        assertTrue(
            "the include must be named in the diagnostic, got \"${result.error}\"",
            result.error.contains("definitely_absent.glsl") && result.error.contains("include"),
        )
    }

    /** A shader that escapes its own asset root is refused, not read. */
    @Test
    fun anIncludeMayNotEscapeTheAssetRoot() {
        renderer.setShaderIncludeRoot("/")

        val result = renderer.compileToSpirv(
            CopperOxideRenderer.ShaderStage.Fragment,
            """
            #version 450
            #include <../../../../etc/hostname>
            layout(location = 0) out vec4 frag_color;
            void main() {
                frag_color = vec4(1.0);
            }
            """.trimIndent(),
        )

        assertFalse("a path traversal include must be refused", result.succeeded)
        assertTrue(
            "the refusal must not leak the file, got \"${result.error}\"",
            result.error.isNotBlank(),
        )
    }

    // -----------------------------------------------------------------------
    // Caching
    // -----------------------------------------------------------------------

    /**
     * Identical source produces identical SPIR-V, and a second call is a hit.
     *
     * `compiled` only moves on a real compilation, so it is what tells a cache
     * hit from a silent recompile. Recompiling every frame would be invisible in
     * the output and ruinous on a phone.
     */
    @Test
    fun identicalSourceIsCompiledOnceAndThenServedFromTheCache() {
        val first = renderer.compileToSpirv(CopperOxideRenderer.ShaderStage.Fragment, FRAGMENT)
        assertTrue(first.succeeded)
        val before = renderer.shaderCompileStats().compiled

        val second = renderer.compileToSpirv(CopperOxideRenderer.ShaderStage.Fragment, FRAGMENT)
        assertTrue(second.succeeded)
        val after = renderer.shaderCompileStats().compiled

        assertEquals("an identical shader was recompiled", before, after)
        assertEquals(
            "a cache hit must return the same words",
            first.spirv.toList(),
            second.spirv.toList(),
        )
        assertTrue(
            "the cache reported no hit",
            renderer.shaderCompileStats().memoryHits > 0,
        )
    }

    /** A different stage is a different shader and must not be a hit. */
    @Test
    fun theSameTextInADifferentStageIsADifferentShader() {
        // Valid as both a vertex and a fragment stage, so the only difference
        // between the two calls is the stage.
        val shared = """
            #version 450
            layout(location = 0) in vec2 v_uv;
            layout(location = 0) out vec4 frag_color;
            void main() {
                frag_color = vec4(v_uv, 0.0, 1.0);
            }
        """.trimIndent()

        val asFragment = renderer.compileToSpirv(CopperOxideRenderer.ShaderStage.Fragment, shared)
        val asVertex = renderer.compileToSpirv(CopperOxideRenderer.ShaderStage.Vertex, shared)

        assertTrue(asFragment.succeeded)
        assertTrue(asVertex.succeeded)
        assertNotEquals(
            "two different stages produced the same module",
            asFragment.spirv.toList(),
            asVertex.spirv.toList(),
        )
    }

    private companion object {
        /** glslang's "ERROR: 0:LINE:" form. */
        val LINE = Regex(""":(\d+):""")

        // Clean-room shaders written for this project, shaped like Minecraft's
        // own but containing none of its text. Minecraft's assets and shader
        // packs are not permissively licensed and must not be vendored into an
        // MIT project; see THIRD_PARTY_NOTICES.md.
        //
        // #version must start the first line, which is why these are built with
        // trimIndent() and not an indented raw string.

        val FRAGMENT = """
            #version 450

            layout(std140, set = 0, binding = 0) uniform Globals {
                vec4 fog_color;
                vec4 color_modulator;
                vec2 screen_size;
                float fog_start;
                float fog_end;
            } globals;

            layout(set = 0, binding = 1) uniform sampler2D sampler0;

            layout(location = 0) in vec2 v_uv;
            layout(location = 0) in float v_fog;
            layout(location = 0) out vec4 frag_color;

            void main() {
                vec4 texel = texture(sampler0, v_uv) * globals.color_modulator;
                if (texel.a < 0.1) {
                    discard;
                }
                float fog_factor = smoothstep(globals.fog_start, globals.fog_end, v_fog);
                frag_color = mix(texel, globals.fog_color, fog_factor);
            }
        """.trimIndent()

        val VERTEX = """
            #version 450

            layout(std140, set = 0, binding = 0) uniform Globals {
                vec4 fog_color;
                vec4 color_modulator;
                vec2 screen_size;
                float fog_start;
                float fog_end;
            } globals;

            layout(location = 0) in vec3 a_position;
            layout(location = 1) in vec4 a_color;
            layout(location = 2) in vec2 a_texcoord;
            layout(location = 3) in float a_fog;

            layout(location = 0) out vec2 v_uv;
            layout(location = 0) out float v_fog;

            void main() {
                v_uv = a_texcoord * (globals.screen_size.y * 0.00390625);
                v_fog = a_fog;
                gl_Position = vec4(a_position * a_color.a, 1.0);
            }
        """.trimIndent()

        val COMPUTE = """
            #version 450

            layout(local_size_x = 16) in;
            layout(set = 0, binding = 0, std430) readonly buffer Input { float in_values[]; };
            layout(set = 0, binding = 1, std430) writeonly buffer Output { float out_values[]; };

            void main() {
                uint i = gl_GlobalInvocationID.x;
                out_values[i] = in_values[i] * 2.0;
            }
        """.trimIndent()

        /**
         * Only referenced inside a conditional, so this compiles with or without
         * the define. Which makes the generated SPIR-V the only evidence that the
         * define was applied.
         */
        val BRANCHING = """
            #version 450

            #ifdef COPPER_TEST_BRANCH
            const float COPPER_SCALE = 2.0;
            #else
            const float COPPER_SCALE = 1.0;
            #endif

            layout(location = 0) in vec2 v_uv;
            layout(location = 0) out vec4 frag_color;

            void main() {
                frag_color = vec4(v_uv * COPPER_SCALE, 0.0, 1.0);
            }
        """.trimIndent()

        /**
         * Carries its own `#version`, which is what Minecraft's `include/`
         * directory does and what a GLSL front end rejects once the text is
         * spliced into another translation unit.
         */
        val SHARED_HEADER = """
            #version 450

            vec4 copper_shared_tint() {
                return vec4(0.25, 0.5, 0.75, 1.0);
            }
        """.trimIndent()
    }
}
