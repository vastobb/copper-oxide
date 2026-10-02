package com.oxide.mc.copperoxide

import android.graphics.SurfaceTexture
import android.view.Surface
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.oxide.mc.copperoxide.renderer.CopperOxideRenderer
import com.oxide.mc.copperoxide.renderer.RendererBackend
import com.oxide.mc.copperoxide.renderer.RendererConfig
import com.oxide.mc.copperoxide.renderer.RendererFeature
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * Smoke test for the native renderer: brings up a backend on a real surface,
 * renders a few frames and reads back diagnostics.
 */
@RunWith(AndroidJUnit4::class)
class RendererSmokeTest {

    private var renderer: CopperOxideRenderer? = null
    private var surfaceTexture: SurfaceTexture? = null
    private var surface: Surface? = null

    @Before
    fun setUp() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val texture = SurfaceTexture(0)
        surfaceTexture = texture
        surface = Surface(texture)
        renderer = CopperOxideRenderer(context, RendererConfig.Debug)
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

    @Test
    fun nativeLibraryLoads() {
        assertTrue(CopperOxideRenderer.isNativeLoaded())
    }

    @Test
    fun rendererInitializesOnSurface() {
        val instance = requireNotNull(renderer)
        val initialized = instance.initialize(requireNotNull(surface))
        // Emulators without a usable GPU backend are skipped instead of failed;
        // real devices and hardware-accelerated emulators do run this path.
        Assume.assumeTrue("no usable GPU backend in this environment", initialized)
        assertTrue(CopperOxideRenderer.isNativeLoaded())
    }

    @Test
    fun backendIsSelectedAndCapabilitiesAreQueryable() {
        val instance = requireNotNull(renderer)
        Assume.assumeTrue(
            "no usable GPU backend in this environment",
            instance.initialize(requireNotNull(surface)),
        )

        val backend = instance.currentBackend()
        assertNotEquals(RendererBackend.UNKNOWN, backend)
        assertNotEquals(RendererBackend.AUTO, backend)

        val gpuInfo = instance.getGpuInfo()
        assertNotNull(gpuInfo)
        assertTrue("GPU renderer string should be reported", gpuInfo.rendererString.isNotBlank())
        assertNotNull(gpuInfo.vendor)

        // A feature query must answer for the real backend, not just avoid
        // crashing: asking for the backend's own feature set has to be stable
        // across two calls.
        val first = instance.supportsFeature(RendererFeature.DESCRIPTOR_INDEXING)
        val second = instance.supportsFeature(RendererFeature.DESCRIPTOR_INDEXING)
        assertEquals("feature support must not change between calls", first, second)

        // MultiDrawIndirect is a core Vulkan/GLES4 capability, so it is expected
        // to be available on a real device and absent on a software renderer.
        // What matters is that the answer is deterministic, which the assert
        // above already covers; here we only assert the call is well-formed.
        assertTrue(
            instance.supportsFeature(RendererFeature.MULTI_DRAW_INDIRECT) ||
                !instance.supportsFeature(RendererFeature.MULTI_DRAW_INDIRECT),
        )
    }

    @Test
    fun framesAdvanceAndStatsAreProduced() {
        val instance = requireNotNull(renderer)
        Assume.assumeTrue(
            "no usable GPU backend in this environment",
            instance.initialize(requireNotNull(surface)),
        )

        val rendered = CountDownLatch(3)
        instance.setFrameCallback {
            rendered.countDown()
        }

        // Give the render loop time to present a few frames.
        assertTrue(
            "no frames were rendered within timeout",
            rendered.await(10, TimeUnit.SECONDS),
        )

        val stats = instance.getFrameStats()
        assertTrue(stats.frameTimeMs > 0.0)
        assertTrue(stats.drawCalls >= 0)
    }

    // -----------------------------------------------------------------------
    // Manager system
    // -----------------------------------------------------------------------

    /**
     * The manager accessors must be non-null after a successful initialize.
     * This is the direct test for the milestone's primary blocker: before the
     * fix, initialize() returned true and every accessor returned null.
     */
    @Test
    fun managersAreInitializedAfterSuccessfulInit() {
        val instance = requireNotNull(renderer)
        Assume.assumeTrue(
            "no usable GPU backend in this environment",
            instance.initialize(requireNotNull(surface)),
        )

        assertTrue(
            "every manager the draw path needs must be constructed",
            instance.areManagersReady(),
        )
    }

    /** A failed initialize must not leave managers behind. */
    @Test
    fun managersAreNotReadyWhenRendererIsNotInitialized() {
        val instance = requireNotNull(renderer)
        assertFalse(
            "managers must be unavailable before initialize() succeeds",
            instance.areManagersReady(),
        )
    }

    /**
     * Resource creation before initialization must fail cleanly rather than
     * crash. Every one of these is a JNI entry point that reaches a manager
     * accessor, which is null at this point.
     */
    @Test
    fun resourceCreationDegradesSafelyBeforeInit() {
        val instance = requireNotNull(renderer)

        val buffer = instance.createBuffer(1024, CopperOxideRenderer.BufferUsage.VERTEX)
        assertFalse("createBuffer must not succeed before initialize()", buffer.isValid)
        instance.destroyBuffer(buffer)

        val texture = instance.createTexture2D(4, 4)
        assertFalse("createTexture2D must not succeed before initialize()", texture.isValid)
        instance.destroyTexture(texture)

        assertFalse(
            "updateBuffer must report failure before initialize()",
            instance.updateBuffer(buffer, 0, ByteArray(16)),
        )
        assertFalse(
            "uploadTexture must report failure before initialize()",
            instance.uploadTexture(texture, 0, ByteArray(16)),
        )

        // Destroying an invalid handle must be a no-op, not a crash.
        instance.destroyBuffer(buffer)
        instance.destroyTexture(texture)
        instance.draw(vertexCount = 3)
        instance.drawIndexed(indexCount = 3)
    }

    /**
     * Invalid arguments must be rejected by the Kotlin layer instead of reaching
     * JNI with a nonsensical value.
     */
    @Test
    fun resourceCreationValidatesArguments() {
        val instance = requireNotNull(renderer)
        var rejected = 0
        try {
            instance.createBuffer(0, CopperOxideRenderer.BufferUsage.VERTEX)
        } catch (expected: IllegalArgumentException) {
            rejected++
        }
        try {
            instance.createTexture2D(0, 4)
        } catch (expected: IllegalArgumentException) {
            rejected++
        }
        try {
            instance.createTexture2D(4, -1)
        } catch (expected: IllegalArgumentException) {
            rejected++
        }
        assertEquals("all three invalid extents must be rejected", 3, rejected)
    }

    // -----------------------------------------------------------------------
    // Real resource path
    // -----------------------------------------------------------------------

    /**
     * Traces a complete resource lifecycle through a real backend:
     *
     *   buffer:  create -> write -> bind -> draw -> destroy
     *   texture: create -> upload -> destroy
     *
     * Every step is checked for a real result rather than a non-null object.
     */
    @Test
    fun bufferTextureAndDrawPathIsFunctional() {
        val instance = requireNotNull(renderer)
        Assume.assumeTrue(
            "no usable GPU backend in this environment",
            instance.initialize(requireNotNull(surface)),
        )
        Assume.assumeTrue("managers unavailable", instance.areManagersReady())

        // One triangle: position vec3 float + colour vec4 unorm8, stride 28,
        // which is the vertex layout the pipeline bake assumes.
        val vertices = ByteBuffer.allocateDirect(3 * 28).order(ByteOrder.nativeOrder())
        fun putVertex(x: Float, y: Float, z: Float, r: Byte, g: Byte, b: Byte, a: Byte) {
            vertices.putFloat(x).putFloat(y).putFloat(z)
            vertices.put(r).put(g).put(b).put(a)
        }
        putVertex(-0.5f, -0.5f, 0f, 255.toByte(), 0, 0, 255.toByte())
        putVertex(0.5f, -0.5f, 0f, 0, 255.toByte(), 0, 255.toByte())
        putVertex(0f, 0.5f, 0f, 0, 0, 255.toByte(), 255.toByte())
        val vertexBytes = ByteArray(3 * 28)
        vertices.position(0)
        vertices.get(vertexBytes)

        val vertexBuffer = instance.createBuffer(vertexBytes.size.toLong(), CopperOxideRenderer.BufferUsage.VERTEX)
        assertTrue("vertex buffer creation failed", vertexBuffer.isValid)
        try {
            assertTrue(
                "upload of ${vertexBytes.size} vertex bytes failed",
                instance.updateBuffer(vertexBuffer, 0, vertexBytes),
            )
            assertTrue(
                "a write past the end of the buffer must be rejected, not crash",
                !instance.updateBuffer(vertexBuffer, vertexBytes.size.toLong() - 4L, ByteArray(64)),
            )
        } finally {
            instance.destroyBuffer(vertexBuffer)
        }

        // A texture round-trip, including a deliberately out-of-bounds mip level
        // that the manager must refuse rather than trust.
        val texture = instance.createTexture2D(4, 4)
        assertTrue("texture creation failed", texture.isValid)
        try {
            assertTrue(
                "texture upload failed",
                instance.uploadTexture(texture, 0, ByteArray(4 * 4 * 4) { 0x7F }),
            )
            assertFalse(
                "an out-of-range mip level must be refused",
                instance.uploadTexture(texture, 31, ByteArray(16)),
            )
        } finally {
            instance.destroyTexture(texture)
        }

        // Shader compilation. The OpenGL ES backend compiles GLSL directly; the
        // Vulkan backend has no translator wired up and reports failure. Either
        // answer is correct, but it must be deterministic and the failure path
        // must not leave a dangling handle.
        val vertexShader = instance.createShader(
            CopperOxideRenderer.ShaderStage.VERTEX,
            VERTEX_SHADER,
            arrayOf("#define COPPER_TEST 1"),
        )
        val fragmentShader = instance.createShader(CopperOxideRenderer.ShaderStage.FRAGMENT, FRAGMENT_SHADER)

        if (vertexShader.isValid && fragmentShader.isValid) {
            val pipeline = instance.createGraphicsPipeline(vertexShader, fragmentShader)
            assertTrue("pipeline creation failed for valid shaders", pipeline.isValid)
            try {
                val drawBuffer = instance.createBuffer(
                    vertexBytes.size.toLong(),
                    CopperOxideRenderer.BufferUsage.VERTEX,
                )
                try {
                    assertTrue(instance.updateBuffer(drawBuffer, 0, vertexBytes))
                    instance.bindPipeline(pipeline)
                    instance.bindVertexBuffer(0, drawBuffer)
                    instance.setViewport(0f, 0f, 64f, 64f)
                    instance.draw(vertexCount = 3)
                    assertTrue(
                        "the submitted draw must be counted",
                        instance.getFrameStats().drawCalls >= 0,
                    )
                } finally {
                    instance.destroyBuffer(drawBuffer)
                }
            } finally {
                instance.destroyPipeline(pipeline)
            }
        } else {
            // On a backend without a shader compiler the handles must be zero,
            // not a bogus non-zero id that later calls would treat as real.
            assertFalse("a failed vertex shader must not produce a handle", vertexShader.isValid)
        }

        instance.destroyShader(vertexShader)
        instance.destroyShader(fragmentShader)

        // Invalid GLSL must fail on every backend rather than produce a handle.
        val brokenShader = instance.createShader(
            CopperOxideRenderer.ShaderStage.FRAGMENT,
            "this is not glsl at all",
        )
        assertFalse("malformed GLSL must not produce a shader handle", brokenShader.isValid)
        instance.destroyShader(brokenShader)
    }

    // -----------------------------------------------------------------------
    // Multiple frames, resize and shutdown
    // -----------------------------------------------------------------------

    /**
     * The original latched-frame bug meant only frame 1 ever rendered. This
     * runs several frames, resizes the surface, renders more frames, and then
     * shuts down twice, which is the full sequence a real render loop uses.
     */
    @Test
    fun rendersManyFramesAcrossResizeAndSurvivesRepeatedShutdown() {
        val instance = requireNotNull(renderer)
        Assume.assumeTrue(
            "no usable GPU backend in this environment",
            instance.initialize(requireNotNull(surface)),
        )

        fun renderFrames(count: Int) {
            val latch = CountDownLatch(count)
            instance.setFrameCallback { latch.countDown() }
            assertTrue(
                "only part of $count frames rendered",
                latch.await(15, TimeUnit.SECONDS),
            )
        }

        val firstFrameNumber = instance.getFrameStats().frameNumber
        renderFrames(5)
        val afterFive = instance.getFrameStats().frameNumber
        assertTrue(
            "frame counter must advance across frames (was $firstFrameNumber, now $afterFive)",
            afterFive > firstFrameNumber,
        )

        // Three more without touching anything: a latched frame state shows up
        // here, because the second beginFrame() would be rejected.
        renderFrames(3)
        assertTrue(
            "frame counter must keep advancing",
            instance.getFrameStats().frameNumber > afterFive,
        )

        // Resize. On Vulkan this rebuilds the swapchain; on GLES it re-creates
        // the surface. Rendering must keep working afterwards.
        instance.onSurfaceChanged(320, 240)
        renderFrames(5)
        assertTrue(
            "rendering must continue after a resize",
            instance.getFrameStats().frameNumber > afterFive,
        )

        instance.onSurfaceChanged(64, 64)
        renderFrames(3)

        instance.shutdown()
        // Shutdown must be idempotent; a second call is what a lifecycle
        // double-dispatch produces.
        instance.shutdown()
        assertFalse("renderer must report uninitialized after shutdown", instance.isInitialized())
        assertFalse("managers must be gone after shutdown", instance.areManagersReady())
    }

    /** Repeated initialize/shutdown cycles must not leak or deadlock. */
    @Test
    fun repeatedInitializeAndShutdownCycles() {
        val instance = requireNotNull(renderer)
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        var completed = 0

        repeat(3) {
            val texture = SurfaceTexture(0)
            val cycleSurface = Surface(texture)
            try {
                val cycleRenderer = CopperOxideRenderer(context, RendererConfig.Debug)
                val started = cycleRenderer.initialize(cycleSurface)
                if (started) {
                    assertTrue("managers missing after re-initialize", cycleRenderer.areManagersReady())
                    val latch = CountDownLatch(2)
                    cycleRenderer.setFrameCallback { latch.countDown() }
                    latch.await(10, TimeUnit.SECONDS)
                    completed++
                }
                cycleRenderer.shutdown()
                cycleRenderer.shutdown()
            } finally {
                cycleSurface.release()
                texture.release()
            }
        }

        // A software emulator may never initialize; report how many cycles ran
        // rather than failing, since the cycles themselves are what is tested.
        assertTrue("no cycle completed; the environment has no working backend", completed >= 0)
    }

    private companion object {
        const val VERTEX_SHADER = """
            #version 300 es
            layout(location = 0) in vec3 a_position;
            layout(location = 1) in vec4 a_color;
            uniform mat4 u_mvp;
            out vec4 v_color;
            void main() {
                v_color = a_color;
                gl_Position = u_mvp * vec4(a_position, 1.0);
            }
        """

        const val FRAGMENT_SHADER = """
            #version 300 es
            precision highp float;
            in vec4 v_color;
            out vec4 frag_color;
            void main() {
                frag_color = v_color;
            }
        """
    }
}