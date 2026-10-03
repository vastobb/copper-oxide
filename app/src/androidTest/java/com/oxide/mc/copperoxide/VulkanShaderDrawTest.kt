package com.oxide.mc.copperoxide

import android.graphics.SurfaceTexture
import android.view.Surface
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.oxide.mc.copperoxide.renderer.CopperOxideRenderer
import com.oxide.mc.copperoxide.renderer.FrameStats
import com.oxide.mc.copperoxide.renderer.RendererBackend
import com.oxide.mc.copperoxide.renderer.RendererConfig
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Assume
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * The Vulkan GLSL path, end to end.
 *
 * [RendererSmokeTest] proves the renderer runs. This proves that *Vulkan* accepts
 * GLSL, which is a different thing: Vulkan has no GLSL dialect, so the source has
 * to be compiled to SPIR-V on the device before a shader module can exist. A
 * renderer that initializes on Vulkan but cannot compile a shader is not a Vulkan
 * renderer.
 *
 * Every step runs through [runOnRenderThread], the only place the graphics
 * context is current and the only place the frame's command buffer is open.
 */
@RunWith(AndroidJUnit4::class)
class VulkanShaderDrawTest {

    private var renderer: CopperOxideRenderer? = null
    private var surfaceTexture: SurfaceTexture? = null
    private var surface: Surface? = null

    @Before
    fun setUp() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val texture = SurfaceTexture(0)
        surfaceTexture = texture
        surface = Surface(texture)
        renderer = CopperOxideRenderer(
            context,
            // Vulkan is requested explicitly. AUTO would silently pick the other
            // backend and this test would pass without compiling anything.
            RendererConfig.Debug.copy(preferredBackend = RendererBackend.VULKAN),
        )
    }

    @After
    fun tearDown() {
        renderer?.shutdown()
        renderer = null
        surface?.release()
        surface = null
        surfaceTexture?.release()
        surfaceTexture = null
    }

    private fun requireVulkan(): CopperOxideRenderer {
        val instance = requireNotNull(renderer)
        Assume.assumeTrue("this environment has no Vulkan driver", instance.initialize(requireNotNull(surface)))
        // Assume has no assumeEquals; the equality is the condition to assume.
        Assume.assumeTrue(
            "Vulkan was requested but the backend selected was ${instance.currentBackend()}",
            instance.currentBackend() == RendererBackend.VULKAN,
        )
        Assume.assumeTrue("managers unavailable", instance.areManagersReady())
        return instance
    }

    // -----------------------------------------------------------------------
    // The whole path
    // -----------------------------------------------------------------------

    /**
     * GLSL -> SPIR-V -> shader module -> pipeline -> bind -> draw -> present,
     * across several frames.
     *
     * The shaders deliberately declare no resources. Descriptor set allocation is
     * not part of this milestone, and a shader that needed one would fail for an
     * unrelated reason, which would make this test lie about what it covers. A
     * resource-free shader still exercises everything the milestone is about:
     * compilation, module creation, pipeline creation, a real draw, and the frame
     * presenting.
     */
    @Test
    fun glslCompilesToSpirvAndReachesARealVulkanDraw() {
        val instance = requireVulkan()

        var vertexShader = 0L
        var fragmentShader = 0L
        var pipeline = 0L
        var drawSubmitted = false
        var compileError = ""

        val compiled = instance.runOnRenderThread {
            vertexShader = instance.createShader(
                CopperOxideRenderer.ShaderStage.Vertex,
                FULLSCREEN_TRIANGLE_VERTEX,
                arrayOf("#define COPPER_VULKAN_TEST 1"),
            ).value
            fragmentShader = instance.createShader(
                CopperOxideRenderer.ShaderStage.Fragment,
                FLAT_COLOUR_FRAGMENT,
            ).value
            compileError = instance.lastShaderError()

            if (vertexShader != 0L && fragmentShader != 0L) {
                pipeline = instance.createGraphicsPipeline(
                    CopperOxideRenderer.ResourceHandle(vertexShader),
                    CopperOxideRenderer.ResourceHandle(fragmentShader),
                ).value
                if (pipeline != 0L) {
                    instance.bindPipeline(CopperOxideRenderer.ResourceHandle(pipeline))
                    // No vertex buffer: the vertex stage derives its position
                    // from gl_VertexIndex.
                    instance.draw(vertexCount = 3)
                    drawSubmitted = true
                }
            }
        }

        assertTrue("work submitted to the render thread did not run", compiled)
        assertTrue("the vertex shader did not compile from GLSL: $compileError", vertexShader != 0L)
        assertTrue("the fragment shader did not compile from GLSL: $compileError", fragmentShader != 0L)
        assertTrue("the pipeline was not created from the translated SPIR-V", pipeline != 0L)
        assertTrue("the draw was not submitted", drawSubmitted)

        // The draw has to survive the frame it was recorded in.
        val rendered = CountDownLatch(5)
        var stats: FrameStats? = null
        instance.setFrameCallback {
            stats = it
            rendered.countDown()
        }
        assertTrue("frames stopped after the shader draw was submitted", rendered.await(15, TimeUnit.SECONDS))

        val observed = requireNotNull(stats)
        assertTrue("the frame counter did not advance", observed.frameNumber > 0)
        assertTrue(
            "the submitted draw was not counted (drawCalls=${observed.drawCalls})",
            observed.drawCalls >= 1,
        )

        instance.destroyPipeline(CopperOxideRenderer.ResourceHandle(pipeline))
        instance.destroyShader(CopperOxideRenderer.ResourceHandle(vertexShader))
        instance.destroyShader(CopperOxideRenderer.ResourceHandle(fragmentShader))
    }

    /** Identical source must be served from the cache, not recompiled. */
    @Test
    fun identicalGlslIsServedFromTheCache() {
        val instance = requireVulkan()
        var firstShader = 0L
        assertTrue(
            "work was not accepted",
            instance.runOnRenderThread {
                firstShader = instance.createShader(
                    CopperOxideRenderer.ShaderStage.Fragment,
                    FLAT_COLOUR_FRAGMENT,
                ).value
            },
        )
        assertTrue("the fragment shader did not compile", firstShader != 0L)

        val before = instance.shaderCompileStats()

        var secondShader = 0L
        assertTrue(
            "work was not accepted",
            instance.runOnRenderThread {
                secondShader = instance.createShader(
                    CopperOxideRenderer.ShaderStage.Fragment,
                    FLAT_COLOUR_FRAGMENT,
                ).value
            },
        )
        assertTrue("the cached shader did not come back", secondShader != 0L)
        assertNotEquals("the cache must not hand back the same handle", firstShader, secondShader)

        val after = instance.shaderCompileStats()
        assertEquals(
            "an identical shader was recompiled instead of served from the cache",
            before.compiled,
            after.compiled,
        )
        assertTrue("the cache reported no hit (hits=${after.memoryHits})", after.memoryHits > before.memoryHits)

        instance.destroyShader(CopperOxideRenderer.ResourceHandle(firstShader))
        instance.destroyShader(CopperOxideRenderer.ResourceHandle(secondShader))
    }

    /** A different define set is a different shader and must not be a hit. */
    @Test
    fun definesChangeTheCacheKey() {
        val instance = requireVulkan()
        var withoutDefine = 0L
        var withDefine = 0L

        assertTrue(
            "work was not accepted",
            instance.runOnRenderThread {
                withoutDefine = instance.createShader(
                    CopperOxideRenderer.ShaderStage.Fragment,
                    FLAT_COLOUR_FRAGMENT,
                ).value
            },
        )
        assertTrue("the shader did not compile", withoutDefine != 0L)
        val before = instance.shaderCompileStats()

        assertTrue(
            "work was not accepted",
            instance.runOnRenderThread {
                withDefine = instance.createShader(
                    CopperOxideRenderer.ShaderStage.Fragment,
                    FLAT_COLOUR_FRAGMENT,
                    arrayOf("#define COPPER_VULKAN_TEST 1"),
                ).value
            },
        )
        assertTrue("the shader did not compile with a define", withDefine != 0L)

        val after = instance.shaderCompileStats()
        assertTrue("adding a define must not reuse the undef'd artifact", after.compiled > before.compiled)

        instance.destroyShader(CopperOxideRenderer.ResourceHandle(withoutDefine))
        instance.destroyShader(CopperOxideRenderer.ResourceHandle(withDefine))
    }

    // -----------------------------------------------------------------------
    // Failure cases
    // -----------------------------------------------------------------------

    /**
     * A malformed shader must fail with a usable message and no handle. The
     * alternative - a non-zero handle and a module that fails later - is how a
     * renderer ends up silently drawing nothing.
     */
    @Test
    fun malformedGlslFailsWithADiagnosticAndNoHandle() {
        val instance = requireVulkan()
        var handle = 0L
        var error = ""

        assertTrue(
            "work was not accepted",
            instance.runOnRenderThread {
                handle = instance.createShader(
                    CopperOxideRenderer.ShaderStage.Fragment,
                    "this is not glsl at all",
                ).value
                error = instance.lastShaderError()
            },
        )

        assertEquals("malformed GLSL must not produce a handle", 0L, handle)
        assertTrue("a compile failure must leave a diagnostic, got \"$error\"", error.isNotBlank())
        assertTrue(
            "the diagnostic should mention the stage, got \"$error\"",
            error.contains("fragment", ignoreCase = true),
        )
        assertTrue("the failure was not counted", instance.shaderCompileStats().failures > 0)
    }

    /** A shader that parses but declares no output is not a linkable fragment shader. */
    @Test
    fun fragmentShaderWithoutAnOutputIsRejected() {
        val instance = requireVulkan()
        var handle = 0L
        var error = ""

        assertTrue(
            "work was not accepted",
            instance.runOnRenderThread {
                handle = instance.createShader(
                    CopperOxideRenderer.ShaderStage.Fragment,
                    """
                    #version 450
                    void main() {
                    }
                    """.trimIndent(),
                ).value
                error = instance.lastShaderError()
            },
        )

        assertEquals("a fragment shader with no output must not produce a handle", 0L, handle)
        assertTrue("a compile failure must leave a diagnostic, got \"$error\"", error.isNotBlank())
    }

    /** A valid shader must still compile after a failure: no poisoned state. */
    @Test
    fun aValidShaderCompilesAfterAFailure() {
        val instance = requireVulkan()
        var good = 0L
        var error = ""

        assertTrue(
            "work was not accepted",
            instance.runOnRenderThread {
                instance.createShader(
                    CopperOxideRenderer.ShaderStage.Fragment,
                    "this is not glsl at all",
                )
                good = instance.createShader(
                    CopperOxideRenderer.ShaderStage.Vertex,
                    FULLSCREEN_TRIANGLE_VERTEX,
                ).value
                error = instance.lastShaderError()
            },
        )

        assertTrue("a valid shader failed after an earlier failure: $error", good != 0L)
        assertTrue("a successful compile must clear the error", error.isBlank())
        instance.destroyShader(CopperOxideRenderer.ResourceHandle(good))
    }

    // -----------------------------------------------------------------------
    // Pipeline integration
    // -----------------------------------------------------------------------

    /**
     * A compute shader compiles - Vulkan requires compute unconditionally - but
     * it must not be usable as a graphics pipeline input, because doing so would
     * die inside the driver rather than reporting a usable error.
     */
    @Test
    fun aComputeShaderCompilesButMakesNoGraphicsPipeline() {
        val instance = requireVulkan()
        var compute = 0L

        assertTrue(
            "work was not accepted",
            instance.runOnRenderThread {
                compute = instance.createShader(
                    CopperOxideRenderer.ShaderStage.Compute,
                    """
                    #version 450
                    layout(local_size_x = 1) in;
                    void main() {
                    }
                    """.trimIndent(),
                ).value
            },
        )
        assertTrue("a compute shader did not compile from GLSL", compute != 0L)

        assertTrue(
            "work was not accepted",
            instance.runOnRenderThread {
                val pipeline = instance.createGraphicsPipeline(
                    CopperOxideRenderer.ResourceHandle(compute),
                    CopperOxideRenderer.ResourceHandle(compute),
                )
                assertFalse("a compute shader must not make a graphics pipeline", pipeline.isValid)
            },
        )
        instance.destroyShader(CopperOxideRenderer.ResourceHandle(compute))
    }

    // -----------------------------------------------------------------------
    // Includes
    // -----------------------------------------------------------------------

    /**
     * A shader that includes another must compile.
     *
     * This is the case that matters for Minecraft: its shipped shaders are almost
     * entirely `#include <minecraft:NAME.glsl>` one-liners over a shared header
     * directory, so an include that does not resolve means no Minecraft shader
     * compiles at all.
     */
    @Test
    fun aShaderThatIncludesAnotherCompiles() {
        val instance = requireVulkan()
        val header = java.io.File.createTempFile("copper-shader", ".glsl")
        try {
            header.writeText(SHARED_HEADER)
            instance.setShaderIncludeRoot(header.parent)

            var handle = 0L
            var error = ""
            assertTrue(
                "work was not accepted",
                instance.runOnRenderThread {
                    handle = instance.createShader(
                        CopperOxideRenderer.ShaderStage.Fragment,
                        """
                        #version 450
                        #include <${header.name}>
                        layout(location = 0) out vec4 frag_color;
                        void main() {
                            frag_color = shared_tint();
                        }
                        """.trimIndent(),
                    ).value
                    error = instance.lastShaderError()
                },
            )
            assertTrue("an included shader did not compile: $error", handle != 0L)
            instance.destroyShader(CopperOxideRenderer.ResourceHandle(handle))
        } finally {
            header.delete()
            instance.setShaderIncludeRoot("")
        }
    }

    /** An include nobody can resolve must fail, not be silently dropped. */
    @Test
    fun anUnresolvableIncludeIsReported() {
        val instance = requireVulkan()
        instance.setShaderIncludeRoot("/nonexistent-copper-shader-root")
        try {
            var handle = 0L
            var error = ""
            assertTrue(
                "work was not accepted",
                instance.runOnRenderThread {
                    handle = instance.createShader(
                        CopperOxideRenderer.ShaderStage.Fragment,
                        """
                        #version 450
                        #include <definitely_not_present.glsl>
                        layout(location = 0) out vec4 frag_color;
                        void main() {
                            frag_color = vec4(1.0);
                        }
                        """.trimIndent(),
                    ).value
                    error = instance.lastShaderError()
                },
            )
            assertEquals("an unresolvable include must not produce a handle", 0L, handle)
            assertTrue(
                "the unresolvable include must be named, got \"$error\"",
                error.contains("include"),
            )
        } finally {
            instance.setShaderIncludeRoot("")
        }
    }

    private companion object {
        // An included file carrying its own #version, which is exactly what
        // Minecraft's include/ directory does and what a GLSL front end rejects
        // once the text is spliced into another translation unit. The splicer has
        // to strip it.
        const val SHARED_HEADER = """
            #version 450

            vec4 shared_tint() {
                return vec4(0.25, 0.5, 0.75, 1.0);
            }
        """

        // trimIndent matters: GLSL requires #version to be the first thing on the
        // first line, and an indented raw string puts whitespace in front of it.
        //
        // Clean-room: written for this test, not copied from Minecraft or any
        // shader pack. See docs/architecture/shader-abi.md.
        val FULLSCREEN_TRIANGLE_VERTEX =
            """
            #version 450

            layout(location = 0) out vec2 v_uv;

            void main() {
                // One oversized triangle covering the viewport. Deriving the
                // position from gl_VertexIndex means no vertex buffer is bound,
                // which is what lets this test cover the shader path without also
                // depending on descriptor set allocation.
                vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
                v_uv = corner;
                gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
            }
            """.trimIndent()

        val FLAT_COLOUR_FRAGMENT =
            """
            #version 450

            layout(location = 0) in vec2 v_uv;
            layout(location = 0) out vec4 frag_color;

            void main() {
                frag_color = vec4(v_uv, 0.5, 1.0);
            }
            """.trimIndent()
    }
}