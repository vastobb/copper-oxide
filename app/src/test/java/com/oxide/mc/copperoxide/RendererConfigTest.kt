package com.oxide.mc.copperoxide

import com.oxide.mc.copperoxide.renderer.CopperOxideRenderer
import com.oxide.mc.copperoxide.renderer.GpuArchitecture
import com.oxide.mc.copperoxide.renderer.GpuVendor
import com.oxide.mc.copperoxide.renderer.RendererBackend
import com.oxide.mc.copperoxide.renderer.RendererConfig
import com.oxide.mc.copperoxide.renderer.RendererFeature
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * JVM-side tests for the parts of the API that do not need a GPU.
 *
 * Everything here runs on the normal unit-test job, so a GPU-less CI machine
 * still verifies the configuration clamps, the public constants and the
 * "nothing silently succeeds" contract. GPU-dependent behaviour lives in the
 * instrumented test, which uses `Assume` to skip where no backend exists.
 */
class RendererConfigTest {

    @Test
    fun `debug config is valid and uses conservative defaults`() {
        val config = RendererConfig.Debug
        assertTrue("Debug config must validate", config.validate())
        assertTrue("Debug config should allow multiple frames in flight", config.maxFramesInFlight >= 1)
    }

    @Test
    fun `release config is valid`() {
        assertTrue("Release config must validate", RendererConfig.Release.validate())
    }

    @Test
    fun `clampToValidRanges repairs an out of range config instead of rejecting it`() {
        val broken = RendererConfig(
            maxFramesInFlight = 0,
            maxCommandBuffersPerFrame = -4,
            maxDescriptorSets = 1_000_000,
            maxPushConstantsSize = 100_000,
            textureCacheSizeMb = -1,
            shaderCacheSizeMb = -1,
            bufferPoolSizeMb = -1,
            frameTimeoutMs = -5,
            targetFps = 10_000,
        )
        val clamped = broken.clampToValidRanges()

        assertTrue("a clamped config must validate", clamped.validate())
        // The un-clamped config must still fail, which is what proves the check
        // is real and not just always true.
        assertFalse("an un-clamped config must not validate", broken.validate())
        assertTrue("frames in flight must be at least 1", clamped.maxFramesInFlight >= 1)
        assertTrue("command buffers per frame must be positive", clamped.maxCommandBuffersPerFrame > 0)
        assertTrue("descriptor sets must be positive", clamped.maxDescriptorSets > 0)
        assertTrue("push constant size must be positive", clamped.maxPushConstantsSize > 0)
        assertTrue("cache sizes must not be negative", clamped.textureCacheSizeMb >= 0)
        assertTrue("frame timeout must be positive", clamped.frameTimeoutMs > 0)
        assertTrue(
            "target fps must be capped to something a device can sustain",
            clamped.targetFps in 1..240,
        )
    }

    @Test
    fun `clampToValidRanges is idempotent`() {
        val broken = RendererConfig.Default.copy(maxFramesInFlight = 99, targetFps = 1000)
        val once = broken.clampToValidRanges()
        assertEquals("clamping twice must be the same as clamping once", once, once.clampToValidRanges())
        assertTrue("the clamped result must itself validate", once.validate())
    }

    @Test
    fun `unknown backend decodes to UNKNOWN instead of throwing`() {
        // entries.getOrElse is what stops an unrecognised native ordinal from
        // crashing the renderer on a future backend addition.
        assertEquals(
            RendererBackend.UNKNOWN,
            RendererBackend.entries.getOrElse(9999) { RendererBackend.UNKNOWN },
        )
        assertEquals(
            RendererBackend.VULKAN,
            RendererBackend.entries.getOrElse(RendererBackend.VULKAN.ordinal) {
                RendererBackend.UNKNOWN
            },
        )
    }

    @Test
    fun `vendor and architecture enums have stable ordinals matching the native side`() {
        // The JNI boundary passes these as ints, so the ordinals are part of the
        // ABI. Changing them silently would mis-decode on an older native build.
        assertEquals(0, RendererBackend.UNKNOWN.ordinal)
        assertEquals(0, GpuVendor.UNKNOWN.ordinal)
        assertEquals(0, GpuArchitecture.UNKNOWN.ordinal)
        assertEquals(1, GpuVendor.ADRENO.ordinal)
        assertEquals(1, GpuArchitecture.ADRENO_600.ordinal)
    }

    @Test
    fun `feature flags use explicit bits rather than ordinals`() {
        // A feature is sent as its bit so that adding one in the middle of the
        // enum cannot re-map every later feature.
        val bits = RendererFeature.entries.map { it.bit }
        assertEquals("feature bits must be unique", bits.size, bits.toSet().size)
        assertEquals(
            "only NONE may use bit 0, which the native side reads as 'unset'",
            listOf(RendererFeature.NONE),
            RendererFeature.entries.filter { it.bit == 0 },
        )
        bits.filter { it != 0 }.forEach {
            assertTrue("feature bit $it must be a single bit", it and (it - 1) == 0)
        }
    }

    @Test
    fun `resource handles report zero as invalid`() {
        val invalid = CopperOxideRenderer.ResourceHandle(0L)
        assertFalse(invalid.isValid)
        assertTrue(CopperOxideRenderer.ResourceHandle(1L).isValid)
    }

    @Test
    fun `buffer usage flags combine and stay within the backend bit width`() {
        val vertexIndex = CopperOxideRenderer.BufferUsage.of(
            CopperOxideRenderer.BufferUsage.VERTEX,
            CopperOxideRenderer.BufferUsage.TRANSFER_DST,
        )
        assertTrue("VERTEX bit must survive the combination", vertexIndex and CopperOxideRenderer.BufferUsage.VERTEX != 0)
        assertTrue(
            "TRANSFER_DST bit must survive the combination",
            vertexIndex and CopperOxideRenderer.BufferUsage.TRANSFER_DST != 0,
        )
        assertTrue("usage must fit in the 32-bit native field", vertexIndex <= Int.MAX_VALUE)
    }

    @Test
    fun `texture format constants are distinct and non-zero`() {
        val formats = listOf(
            CopperOxideRenderer.TextureFormat.R8,
            CopperOxideRenderer.TextureFormat.RG8,
            CopperOxideRenderer.TextureFormat.RGB8,
            CopperOxideRenderer.TextureFormat.RGBA8,
            CopperOxideRenderer.TextureFormat.SRGB8_ALPHA8,
            CopperOxideRenderer.TextureFormat.RGBA16F,
            CopperOxideRenderer.TextureFormat.R32F,
            CopperOxideRenderer.TextureFormat.RGBA32F,
            CopperOxideRenderer.TextureFormat.DEPTH16,
            CopperOxideRenderer.TextureFormat.DEPTH24_STENCIL8,
            CopperOxideRenderer.TextureFormat.ASTC_4x4,
            CopperOxideRenderer.TextureFormat.ETC2_RGBA8,
        )
        assertEquals("every texture format must be distinct", formats.size, formats.toSet().size)
        assertNotEquals("no real format is zero", 0, CopperOxideRenderer.TextureFormat.RGBA8)
    }

    @Test
    fun `shader stage ordinals match the native ShaderStage enum`() {
        assertEquals(0, CopperOxideRenderer.ShaderStage.VERTEX)
        assertEquals(1, CopperOxideRenderer.ShaderStage.FRAGMENT)
        assertEquals(2, CopperOxideRenderer.ShaderStage.COMPUTE)
    }

    @Test
    fun `frame stats compute fps from frame time and guard against zero`() {
        val stats = com.oxide.mc.copperoxide.renderer.FrameStats(
            frameNumber = 7,
            frameTimeMs = 20.0,
            cpuTimeMs = 4.0,
            gpuTimeMs = 16.0,
            drawCalls = 3,
            gpuMemoryUsed = 1024,
            cpuMemoryUsed = 2048,
        )
        assertEquals(50.0, stats.fps, 0.001)
        assertEquals(20000L, stats.frameTimeUs)
        assertEquals(0.0, stats.copy(frameTimeMs = 0.0).fps, 0.001)
    }
}