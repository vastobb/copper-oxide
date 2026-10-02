package com.oxide.mc.copperoxide.renderer

import android.content.Context
import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.GLES32
import android.util.Log
import android.view.Surface
import androidx.annotation.Keep
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleObserver
import androidx.lifecycle.OnLifecycleEvent
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.withContext

/**
 * Copper Oxide Renderer - High-performance Minecraft Java Edition rendering translation layer
 * Supports Vulkan and OpenGL ES backends with automatic selection and GPU-specific optimizations
 */
@Keep
open class CopperOxideRenderer(
    private val context: Context,
    private val config: RendererConfig = RendererConfig.Default
) : LifecycleObserver, AutoCloseable {

    companion object {
        private const val TAG = "CopperOxideRenderer"
        private const val STATS_INTERVAL_NANOS = 250_000_000L
        private var sNativeLoaded = false

        @JvmStatic
        fun loadNativeLibrary() {
            if (!sNativeLoaded) {
                System.loadLibrary("copper-oxide")
                sNativeLoaded = true
                Log.i(TAG, "Native library loaded")
            }
        }

        @JvmStatic
        fun isNativeLoaded(): Boolean = sNativeLoaded
    }

    // Native renderer state
    private var nativeHandle: Long = 0

    @Volatile
    private var isInitialized = false

    @Volatile
    private var currentBackend = RendererBackend.UNKNOWN

    /** Backend the renderer actually selected at runtime. */
    fun currentBackend(): RendererBackend = currentBackend

    // Rendering coroutine
    private val renderScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private var renderJob: Job? = null
    private var lastStatsEmitNanos = 0L
    private var targetFps = config.targetFps
    private var frameTimeNanos = 1_000_000_000L / config.targetFps.coerceAtLeast(1)

    // Surface
    private var surface: Surface? = null
    private var surfaceTexture: SurfaceTexture? = null
    private var surfaceWidth = 0
    private var surfaceHeight = 0

    // Callbacks
    @Volatile
    private var frameCallback: ((FrameStats) -> Unit)? = null

    @Volatile
    private var errorCallback: ((String) -> Unit)? = null

    init {
        CopperOxideRenderer.loadNativeLibrary()
    }

    /**
     * Initialize the renderer with a surface
     */
    fun initialize(surface: Surface): Boolean {
        if (isInitialized) {
            Log.w(TAG, "Renderer already initialized")
            return true
        }

        this.surface = surface
        // native surface is passed straight through

        val result = nativeInitialize(
            surface,
            config.preferredBackend.ordinal,
            config.enableValidation,
            config.enableDebugMarkers,
            config.enableProfiling,
            config.enableMultithreadedRendering,
            config.enableAsyncShaderCompilation,
            config.enableAsyncResourceLoading,
            config.enableResourcePooling,
            config.enableCommandBufferReuse,
            config.enableStateCaching,
            config.enableDrawCallBatching,
            config.enablePipelineCaching,
            config.enableDescriptorCaching,
            config.enableTextureStreaming,
            config.enableTextureCompression,
            config.enableMipmapGeneration,
            config.maxFramesInFlight,
            config.maxCommandBuffersPerFrame,
            config.maxDescriptorSets,
            config.maxPushConstantsSize,
            config.textureCacheSizeMb,
            config.shaderCacheSizeMb,
            config.bufferPoolSizeMb,
            config.frameTimeoutMs,
            config.vsyncEnabled,
            config.targetFps,
            config.lowLatencyMode,
            config.batterySaverMode,
            config.thermalThrottlingAware,
            config.thermalThrottleThreshold
        )

        if (result) {
            isInitialized = true
            currentBackend = RendererBackend.entries.getOrElse(nativeGetBackend()) { RendererBackend.UNKNOWN }
            Log.i(TAG, "Renderer initialized with backend: $currentBackend")
            startRenderLoop()
        } else {
            Log.e(TAG, "Failed to initialize renderer")
            errorCallback?.invoke("Failed to initialize renderer")
        }

        return result
    }

    /**
     * Start the render loop
     */
    private fun startRenderLoop() {
        renderJob = renderScope.launch {
            while (isInitialized && !Thread.currentThread().isInterrupted) {
                val frameStart = System.nanoTime()

                if (nativeBeginFrame()) {
                    // Render frame here - this is where Minecraft would submit draw calls
                    onRenderFrame()

                    nativeEndFrame()
                    nativePresent()
                }

                // Frame pacing
                val frameEnd = System.nanoTime()
                val sleepTime = frameTimeNanos - (frameEnd - frameStart)

                if (sleepTime > 0) {
                    try {
                        Thread.sleep(sleepTime / 1_000_000, (sleepTime % 1_000_000).toInt())
                    } catch (e: InterruptedException) {
                        // Restore the flag so the loop condition sees the cancellation.
                        Thread.currentThread().interrupt()
                        break
                    }
                }

                // Telemetry is throttled: seven JNI calls plus a main-thread
                // dispatch every frame is pure overhead for a 60 Hz signal.
                val callback = frameCallback
                if (callback != null && frameEnd - lastStatsEmitNanos >= STATS_INTERVAL_NANOS) {
                    lastStatsEmitNanos = frameEnd
                    val stats = getFrameStats()
                    withContext(Dispatchers.Main) {
                        callback(stats)
                    }
                }
            }
        }
    }

    /**
     * Called each frame - override to submit Minecraft draw calls
     */
    protected open fun onRenderFrame() {
        // Submit Minecraft draw commands here
    }

    /**
     * Get current frame statistics
     */
    fun getFrameStats(): FrameStats {
        return FrameStats(
            frameNumber = nativeGetFrameNumber(),
            frameTimeMs = nativeGetFrameTimeMs(),
            cpuTimeMs = nativeGetCpuTimeMs(),
            gpuTimeMs = nativeGetGpuTimeMs(),
            drawCalls = nativeGetDrawCalls(),
            gpuMemoryUsed = nativeGetGpuMemoryUsed(),
            cpuMemoryUsed = nativeGetCpuMemoryUsed()
        )
    }

    /**
     * Get GPU information
     */
    fun getGpuInfo(): GpuInfo {
        return GpuInfo(
            rendererString = nativeGetGpuRendererString(),
            vendorString = nativeGetGpuVendorString(),
            versionString = nativeGetGpuVersionString(),
            vendor = GpuVendor.entries.getOrElse(nativeGetGpuVendor()) { GpuVendor.UNKNOWN },
            architecture = GpuArchitecture.entries.getOrElse(nativeGetGpuArchitecture()) { GpuArchitecture.UNKNOWN },
            supportsVulkan = currentBackend == RendererBackend.VULKAN
        )
    }

    /**
     * Check if a feature is supported
     */
    fun supportsFeature(feature: RendererFeature): Boolean {
        // The bit value is the wire format; ordinals only match by coincidence.
        return nativeSupportsFeature(feature.bit)
    }

    /**
     * Check if an extension is supported
     */
    fun isExtensionSupported(extension: String): Boolean {
        return nativeIsExtensionSupported(extension)
    }

    /**
     * Handle surface changes
     */
    fun onSurfaceChanged(width: Int, height: Int) {
        surfaceWidth = width
        surfaceHeight = height
        nativeOnSurfaceChanged(width, height)
    }

    /**
     * Handle surface destruction
     */
    fun onSurfaceDestroyed() {
        nativeOnSurfaceDestroyed()
        surface = null
    }

    /**
     * Handle memory pressure
     */
    fun onMemoryPressure(level: Int) {
        nativeOnMemoryPressure(level)
    }

    /**
     * Handle thermal throttling
     */
    fun onThermalThrottling(temperatureRatio: Float) {
        nativeOnThermalThrottling(temperatureRatio)
    }

    /**
     * Set frame callback
     */
    fun setFrameCallback(callback: (FrameStats) -> Unit) {
        frameCallback = callback
    }

    /**
     * Set error callback
     */
    fun setErrorCallback(callback: (String) -> Unit) {
        errorCallback = callback
    }

    /**
     * Set target FPS
     */
    fun setTargetFps(fps: Int) {
        targetFps = fps.coerceAtLeast(1)
        frameTimeNanos = 1_000_000_000L / targetFps
    }

    /**
     * Enable/disable VSync
     */
    fun setVSyncEnabled(enabled: Boolean) {
        // Would need native call to update config
    }

    /**
     * Wait for GPU idle
     */
    fun waitIdle() {
        nativeWaitIdle()
    }

    /**
     * Shutdown the renderer
     */
    override fun close() {
        shutdown()
    }

    @androidx.annotation.WorkerThread
    fun shutdown() {
        // Join the render coroutine before tearing down the native renderer:
        // cancelling without joining lets a frame call into a destroyed renderer.
        val job = renderJob
        renderJob = null
        if (job != null) {
            job.cancel()
            runBlocking {
                withTimeoutOrNull(2000L) { job.join() }
            }
        }
        if (isInitialized) {
            nativeShutdown()
            isInitialized = false
            Log.i(TAG, "Renderer shutdown")
        }
    }

    @OnLifecycleEvent(Lifecycle.Event.ON_DESTROY)
    fun onDestroy() {
        shutdown()
    }

    // Native methods
    external private fun nativeInitialize(
        surface: Surface,
        preferredBackend: Int,
        enableValidation: Boolean,
        enableDebugMarkers: Boolean,
        enableProfiling: Boolean,
        enableMultithreaded: Boolean,
        enableAsyncShaderCompilation: Boolean,
        enableAsyncResourceLoading: Boolean,
        enableResourcePooling: Boolean,
        enableCommandBufferReuse: Boolean,
        enableStateCaching: Boolean,
        enableDrawCallBatching: Boolean,
        enablePipelineCaching: Boolean,
        enableDescriptorCaching: Boolean,
        enableTextureStreaming: Boolean,
        enableTextureCompression: Boolean,
        enableMipmapGeneration: Boolean,
        maxFramesInFlight: Int,
        maxCommandBuffersPerFrame: Int,
        maxDescriptorSets: Int,
        maxPushConstantsSize: Int,
        textureCacheSizeMb: Int,
        shaderCacheSizeMb: Int,
        bufferPoolSizeMb: Int,
        frameTimeoutMs: Int,
        vsyncEnabled: Boolean,
        targetFps: Int,
        lowLatencyMode: Boolean,
        batterySaverMode: Boolean,
        thermalThrottlingAware: Boolean,
        thermalThrottleThreshold: Float
    ): Boolean

    external private fun nativeShutdown()
    external private fun nativeBeginFrame(): Boolean
    external private fun nativeEndFrame()
    external private fun nativePresent()
    external private fun nativeOnSurfaceCreated(surface: Surface)
    external private fun nativeOnSurfaceChanged(width: Int, height: Int)
    external private fun nativeOnSurfaceDestroyed()
    external private fun nativeOnMemoryPressure(level: Int)
    external private fun nativeOnThermalThrottling(temperatureRatio: Float)
    external private fun nativeGetBackend(): Int
    external private fun nativeIsInitialized(): Boolean
    external private fun nativeWaitIdle()

    // Frame stats
    external private fun nativeGetFrameNumber(): Long
    external private fun nativeGetFrameTimeMs(): Double
    external private fun nativeGetCpuTimeMs(): Double
    external private fun nativeGetGpuTimeMs(): Double
    external private fun nativeGetDrawCalls(): Int
    external private fun nativeGetGpuMemoryUsed(): Long
    external private fun nativeGetCpuMemoryUsed(): Long
    external private fun nativeResetFrameStats()

    // GPU info
    external private fun nativeGetGpuRendererString(): String
    external private fun nativeGetGpuVendorString(): String
    external private fun nativeGetGpuVersionString(): String
    external private fun nativeGetGpuVendor(): Int
    external private fun nativeGetGpuArchitecture(): Int

    // Feature queries
    external private fun nativeSupportsFeature(feature: Int): Boolean
    external private fun nativeIsExtensionSupported(extension: String): Boolean
}

/**
 * Renderer configuration
 */
data class RendererConfig(
    val preferredBackend: RendererBackend = RendererBackend.AUTO,
    val enableValidation: Boolean = false,
    val enableDebugMarkers: Boolean = true,
    val enableProfiling: Boolean = true,
    val enableMultithreadedRendering: Boolean = true,
    val enableAsyncShaderCompilation: Boolean = true,
    val enableAsyncResourceLoading: Boolean = true,
    val enableResourcePooling: Boolean = true,
    val enableCommandBufferReuse: Boolean = true,
    val enableStateCaching: Boolean = true,
    val enableDrawCallBatching: Boolean = true,
    val enablePipelineCaching: Boolean = true,
    val enableDescriptorCaching: Boolean = true,
    val enableTextureStreaming: Boolean = true,
    val enableTextureCompression: Boolean = true,
    val enableMipmapGeneration: Boolean = true,
    val maxFramesInFlight: Int = 3,
    val maxCommandBuffersPerFrame: Int = 16,
    val maxDescriptorSets: Int = 8192,
    val maxPushConstantsSize: Int = 256,
    val textureCacheSizeMb: Int = 256,
    val shaderCacheSizeMb: Int = 64,
    val bufferPoolSizeMb: Int = 128,
    val frameTimeoutMs: Int = 5000,
    val vsyncEnabled: Boolean = true,
    val targetFps: Int = 60,
    val lowLatencyMode: Boolean = false,
    val batterySaverMode: Boolean = false,
    val thermalThrottlingAware: Boolean = true,
    val thermalThrottleThreshold: Float = 0.85f
) {
    companion object {
        val Default = RendererConfig()
        val Performance = RendererConfig(
            enableValidation = false,
            enableProfiling = false,
            targetFps = 120,
            lowLatencyMode = true
        )
        val BatterySaver = RendererConfig(
            targetFps = 30,
            batterySaverMode = true,
            maxFramesInFlight = 2,
            textureCacheSizeMb = 128,
            bufferPoolSizeMb = 64
        )
        val Debug = RendererConfig(
            enableValidation = true,
            enableDebugMarkers = true,
            enableProfiling = true,
            targetFps = 60
        )
    }
}

/**
 * Renderer backend type
 */
enum class RendererBackend {
    UNKNOWN,
    VULKAN,
    OPENGL_ES,
    AUTO
}

/**
 * Renderer feature flags
 */
enum class RendererFeature(
    val bit: Int
) {
    NONE(0),
    COMPUTE_SHADERS(1),
    GEOMETRY_SHADERS(2),
    TESSELLATION_SHADERS(4),
    INDIRECT_DRAW(8),
    MULTI_DRAW_INDIRECT(16),
    DRAW_INDIRECT_COUNT(32),
    BINDLESS_TEXTURES(64),
    BINDLESS_SAMPLERS(128),
    DESCRIPTOR_INDEXING(256),
    SHADER_DRAW_PARAMETERS(512),
    SUBGROUP_OPERATIONS(1024),
    MESH_SHADERS(2048),
    TASK_SHADERS(4096),
    RAY_TRACING(8192),
    VARIABLE_RATE_SHADING(16384),
    CONSERVATIVE_RASTERIZATION(32768),
    DEPTH_BOUNDS_TEST(65536),
    SAMPLE_LOCATIONS(131072),
    FRAGMENT_STORES_AND_ATOMICS(262144),
    IMAGE_WRITE_WITHOUT_FORMAT(524288),
    STORAGE_IMAGE_EXTENDED_FORMATS(1048576),
    UNIFORM_BUFFER_STANDARD_LAYOUT(2097152),
    SCALAR_BLOCK_LAYOUT(4194304),
    IMAGELESS_FRAMEBUFFER(8388608),
    TIMELINE_SEMAPHORE(16777216),
    BUFFER_DEVICE_ADDRESS(33554432),
    HOST_QUERY_RESET(67108864),
    DYNAMIC_RENDERING(134217728),
    SYNCHRONIZATION_2(268435456),
    MAINTENANCE_FEATURES(536870912),
    ALL(-1)
}

/**
 * GPU vendor
 */
enum class GpuVendor {
    UNKNOWN,
    ADRENO,
    MALI,
    POWERVR,
    APPLE,
    NVIDIA,
    AMD,
    INTEL,
    BROADCOM,
    VIVANTE,
    VERISILICON
}

/**
 * GPU architecture
 */
enum class GpuArchitecture {
    UNKNOWN,
    ADRENO_600,
    ADRENO_700,
    ADRENO_800,
    MALI_MIDGARD,
    MALI_BIFROST,
    MALI_VALHALL,
    MALI_G715,
    POWERVR_ROGUE,
    POWERVR_FURIAN,
    POWERVR_BXM
}

/**
 * GPU information
 */
data class GpuInfo(
    val rendererString: String,
    val vendorString: String,
    val versionString: String,
    val vendor: GpuVendor,
    val architecture: GpuArchitecture,
    val supportsVulkan: Boolean
)

/**
 * Frame statistics
 */
data class FrameStats(
    val frameNumber: Long,
    val frameTimeMs: Double,
    val cpuTimeMs: Double,
    val gpuTimeMs: Double,
    val drawCalls: Int,
    val gpuMemoryUsed: Long,
    val cpuMemoryUsed: Long
) {
    val fps: Double get() = if (frameTimeMs > 0) 1000.0 / frameTimeMs else 0.0
    val frameTimeUs: Long get() = (frameTimeMs * 1000).toLong()
}