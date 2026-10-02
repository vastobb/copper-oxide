package com.oxide.mc.copperoxide

import com.oxide.mc.copperoxide.renderer.RendererBackend
import com.oxide.mc.copperoxide.renderer.RendererConfig
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** JVM-side tests for renderer configuration presets. */
class RendererConfigTest {

    @Test
    fun defaultPrefersAutomaticBackendSelection() {
        val config = RendererConfig.Default
        assertEquals(RendererBackend.AUTO, config.preferredBackend)
        assertEquals(60, config.targetFps)
        assertEquals(3, config.maxFramesInFlight)
    }

    @Test
    fun performancePresetTargetsHighFrameRate() {
        val config = RendererConfig.Performance
        assertEquals(120, config.targetFps)
        assertTrue(config.lowLatencyMode)
        assertFalse(config.enableProfiling)
    }

    @Test
    fun batterySaverPresetReducesWork() {
        val config = RendererConfig.BatterySaver
        assertEquals(30, config.targetFps)
        assertTrue(config.batterySaverMode)
        assertEquals(2, config.maxFramesInFlight)
    }

    @Test
    fun debugPresetEnablesValidation() {
        assertTrue(RendererConfig.Debug.enableValidation)
        assertTrue(RendererConfig.Debug.enableDebugMarkers)
    }
}