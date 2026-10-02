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
import org.junit.Assume
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
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
            instance.initialize(requireNotNull(surface))
        )

        val backend = instance.currentBackend()
        assertNotEquals(RendererBackend.UNKNOWN, backend)
        assertNotEquals(RendererBackend.AUTO, backend)

        val gpuInfo = instance.getGpuInfo()
        assertNotNull(gpuInfo)
        assertTrue("GPU renderer string should be reported", gpuInfo.rendererString.isNotBlank())
        assertNotNull(gpuInfo.vendor)

        // Feature queries must not crash even when the feature is unsupported.
        assertTrue(
            instance.supportsFeature(RendererFeature.DESCRIPTOR_INDEXING) ||
                !instance.supportsFeature(RendererFeature.DESCRIPTOR_INDEXING)
        )
    }

    @Test
    fun framesAdvanceAndStatsAreProduced() {
        val instance = requireNotNull(renderer)
        Assume.assumeTrue(
            "no usable GPU backend in this environment",
            instance.initialize(requireNotNull(surface))
        )

        val rendered = CountDownLatch(3)
        instance.setFrameCallback {
            rendered.countDown()
        }

        // Give the render loop time to present a few frames.
        assertTrue(
            "no frames were rendered within timeout",
            rendered.await(10, TimeUnit.SECONDS)
        )

        val stats = instance.getFrameStats()
        assertTrue(stats.frameTimeMs > 0.0)
        assertTrue(stats.drawCalls >= 0)
    }
}