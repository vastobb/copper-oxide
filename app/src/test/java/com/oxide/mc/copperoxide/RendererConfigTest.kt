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
        val bits = RendererFeature.entries.associate { it to it.bit }

        assertEquals(
            "only NONE may use bit 0, which the native side reads as 'unset'",
            listOf(RendererFeature.NONE),
            bits.filterValues { it == 0 }.keys.toList(),
        )
        assertEquals(
            "ALL is the documented 'every feature' sentinel",
            -1,
            bits.getValue(RendererFeature.ALL),
        )

        // Every feature except the two sentinels is a single bit, so adding one
        // in the middle of the enum cannot re-map the ones after it.
        bits.filterKeys { it != RendererFeature.NONE && it != RendererFeature.ALL }
            .forEach { (feature, bit) ->
                assertTrue("$feature must use a positive bit, was $bit", bit > 0)
                assertEquals("$feature bit $bit must be a single bit", 0, bit and (bit - 1))
            }

        assertEquals(
            "real feature bits must be unique so a query maps back to one feature",
            bits.size - 2,
            bits.filterKeys { it != RendererFeature.NONE && it != RendererFeature.ALL }
                .values.toSet().size,
        )
    }

    @Test
    fun `resource handles report zero as invalid`() {
        val invalid = CopperOxideRenderer.ResourceHandle(0L)
        assertFalse(invalid.isValid)
        assertTrue(CopperOxideRenderer.ResourceHandle(1L).isValid)
    }

    @Test
    fun `buffer usage flags combine and stay within the backend bit width`() {
        val combined = CopperOxideRenderer.BufferUsage.of(
            CopperOxideRenderer.BufferUsage.VERTEX,
            CopperOxideRenderer.BufferUsage.TRANSFER_DST,
        )
        assertTrue(
            "VERTEX bit must survive the combination",
            combined and CopperOxideRenderer.BufferUsage.VERTEX.code != 0,
        )
        assertTrue(
            "TRANSFER_DST bit must survive the combination",
            combined and CopperOxideRenderer.BufferUsage.TRANSFER_DST.code != 0,
        )
        assertEquals("no other bit may be set", 0, combined and CopperOxideRenderer.BufferUsage.INDEX.code)
        assertTrue("usage must fit in the 32-bit native field", combined <= Int.MAX_VALUE)
    }

    @Test
    fun `usage codes are distinct powers of two so they can be combined`() {
        val buffers = CopperOxideRenderer.BufferUsage.entries.filter { it != CopperOxideRenderer.BufferUsage.NONE }
        assertEquals("buffer usage codes must be unique", buffers.size, buffers.map { it.code }.toSet().size)
        buffers.forEach {
            assertEquals("${it.name} code must be a single bit", 0, it.code and (it.code - 1))
        }

        val textureUsage =
            CopperOxideRenderer.TextureUsage.entries.filter { it != CopperOxideRenderer.TextureUsage.NONE }
        assertEquals("texture usage codes must be unique", textureUsage.size, textureUsage.map { it.code }.toSet().size)
        textureUsage.forEach {
            assertEquals("${it.name} code must be a single bit", 0, it.code and (it.code - 1))
        }
    }

    @Test
    fun `texture format codes are distinct and non-zero`() {
        val formats = CopperOxideRenderer.TextureFormat.entries
            .filter { it != CopperOxideRenderer.TextureFormat.NONE }
        assertEquals("every texture format must be distinct", formats.size, formats.map { it.code }.toSet().size)
        formats.forEach { assertNotEquals("${it.name} must have a real code", 0, it.code) }
        assertEquals(
            "NONE must be the only zero code, or the native side cannot detect it",
            0,
            CopperOxideRenderer.TextureFormat.entries.count { it.code == 0 },
        )
    }

    @Test
    fun `shader stage and index type codes match the native enums`() {
        // These are passed straight to JNI and cast to the native enums, so the
        // codes are an ABI and must not be derived from the Kotlin ordinal.
        assertEquals(0, CopperOxideRenderer.ShaderStage.VERTEX.code)
        assertEquals(1, CopperOxideRenderer.ShaderStage.FRAGMENT.code)
        assertEquals(2, CopperOxideRenderer.ShaderStage.COMPUTE.code)
        assertEquals(0, CopperOxideRenderer.IndexType.UINT16.code)
        assertEquals(1, CopperOxideRenderer.IndexType.UINT32.code)
        assertEquals(
            "stage codes must match the declared order",
            CopperOxideRenderer.ShaderStage.entries.map { it.code },
            CopperOxideRenderer.ShaderStage.entries.map { it.ordinal },
        )
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