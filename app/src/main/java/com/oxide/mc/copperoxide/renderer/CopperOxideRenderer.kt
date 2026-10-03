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
import android.os.Handler
import android.os.Looper
import java.util.concurrent.CompletableFuture
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

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

        // How long shutdown() waits for the render thread to notice the stop
        // request. A frame in flight at up to 5s frame timeout can exceed this,
        // which is why shutdown logs rather than blocking indefinitely.
        private const val RENDER_THREAD_JOIN_TIMEOUT_MS = 6_000L

        /**
         * Distinguishes renderer instances within one process.
         *
         * Native code keeps a single process-global renderer slot, so without a
         * tag a second CopperOxideRenderer - the activity, or an instrumentation
         * test running alongside it - silently takes over the slot and the first
         * instance's shutdown() then destroys a renderer it no longer owns.
         */
        private val nextOwnerTag = AtomicLong(1)
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

    // Identifies this instance to the native side. Zero means never initialized.
    private val ownerTag: Long = nextOwnerTag.getAndIncrement()

    // Native renderer state
    private var nativeHandle: Long = 0

    @Volatile
    private var initialized = false

    @Volatile
    private var currentBackend = RendererBackend.UNKNOWN

    /** Backend the renderer actually selected at runtime. */
    fun currentBackend(): RendererBackend = currentBackend

    // Rendering coroutine
    // The render loop runs on its own dedicated thread, NOT on a coroutine
    // dispatcher. A coroutine may resume on a different thread between
    // iterations, and an EGL or Vulkan context is current on exactly one
    // thread: eglMakeCurrent on a second thread fails with EGL_BAD_ACCESS, which
    // is not recoverable and silently stops every frame. This was observed on a
    // CI emulator, where the loop spun without ever rendering a frame.
    private val mainHandler = Handler(Looper.getMainLooper())

    private var renderThread: Thread? = null

    @Volatile
    private var renderLoopRunning = false

    // Work submitted by other threads to run inside the frame, on the render
    // thread, with the graphics context current. This is not a convenience: a
    // GL call from any other thread is undefined behaviour, and on Vulkan the
    // frame's command buffer is only open between beginFrame and endFrame. A
    // renderer that owns a context has to offer a way to use it.
    private val renderWork = LinkedBlockingQueue<RenderTask>()

    private class RenderTask(
        val block: () -> Unit,
        val done: CompletableFuture<Unit>,
    )
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
        if (initialized) {
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
            config.thermalThrottleThreshold,
            ownerTag
        )

        if (result) {
            initialized = true
            currentBackend = RendererBackend.entries.getOrElse(nativeGetBackend(ownerTag)) { RendererBackend.UNKNOWN }
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
        val thread = Thread({ renderLoop() }, "CopperOxide-Render")
        thread.priority = Thread.MAX_PRIORITY
        renderLoopRunning = true
        renderThread = thread
        thread.start()
    }

    private fun renderLoop() {
        try {
            while (renderLoopRunning && initialized) {
                val frameStart = System.nanoTime()

                // beginFrame() returns false when the backend could not acquire
                // an image, which happens during a swapchain recreation and on
                // every frame on a context where presenting is impossible. That
                // is not an error, but it must not be reported as a frame: a
                // callback that fires whether or not anything rendered tells a
                // caller nothing and hides a renderer that never draws.
                val rendered = nativeBeginFrame(ownerTag)
                if (rendered) {
                    // Render frame here - this is where Minecraft would submit draw calls
                    onRenderFrame()

                    // Submitted work runs here, inside the frame and with the
                    // context current.
                    drainRenderWork()

                    nativeEndFrame(ownerTag)
                    nativePresent(ownerTag)
                } else {
                    // The backend could not acquire an image. Work submitted for
                    // this frame is still run, so a caller waiting on it is not
                    // left blocked until the next successful acquire; the GL
                    // managers refuse the calls instead of issuing them with no
                    // context current.
                    drainRenderWork()
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
                if (rendered && callback != null &&
                    frameEnd - lastStatsEmitNanos >= STATS_INTERVAL_NANOS
                ) {
                    lastStatsEmitNanos = frameEnd
                    val stats = getFrameStats()
                    // The callback is dispatched rather than invoked: it runs on
                    // the main thread, which is where a UI observer expects it,
                    // and the render thread must never block on the UI.
                    mainHandler.post { callback(stats) }
                }
            }
        } finally {
            renderLoopRunning = false
        }
    }

    /**
     * Runs [block] on the render thread, inside a frame, with the graphics
     * context current, and returns once it has finished.
     *
     * Every resource creation and draw call must go through this. On OpenGL ES a
     * call from any other thread is undefined behaviour, and on Vulkan the command
     * buffer the frame is recorded into only exists between beginFrame and
     * endFrame. A renderer that owns a context has to offer a way to use it.
     *
     * Returns false if the renderer is not running or the block did not complete
     * within [timeoutMs]. A block that throws reports false rather than
     * propagating: the alternative is taking the render thread, and every
     * subsequent frame, down with it.
     */
    @androidx.annotation.WorkerThread
    fun runOnRenderThread(timeoutMs: Long = 5_000L, block: () -> Unit): Boolean {
        val thread = renderThread
        if (thread == null || !renderLoopRunning) {
            Log.w(TAG, "runOnRenderThread: the render thread is not running")
            return false
        }
        val done = CompletableFuture<Unit>()
        if (!renderWork.offer(RenderTask(block, done), timeoutMs, TimeUnit.MILLISECONDS)) {
            Log.w(TAG, "runOnRenderThread: the render thread did not accept work within ${timeoutMs}ms")
            return false
        }
        return try {
            done.get(timeoutMs, TimeUnit.MILLISECONDS)
            true
        } catch (e: InterruptedException) {
            // Restore the flag so the caller's own interrupt handling still sees it.
            Thread.currentThread().interrupt()
            false
        } catch (e: Exception) {
            Log.e(TAG, "runOnRenderThread: the block did not complete", e)
            false
        }
    }

    private fun drainRenderWork() {
        while (true) {
            val task = renderWork.poll() ?: return
            try {
                task.block()
                task.done.complete(Unit)
            } catch (t: Throwable) {
                // The block runs on the render thread; letting it escape would
                // kill the loop and every frame after it.
                Log.e(TAG, "render thread task failed", t)
                task.done.completeExceptionally(t)
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
     * True between a successful [initialize] and [shutdown].
     *
     * Reads a @Volatile flag rather than calling into native code, so it is safe
     * to use from a test or lifecycle callback on another thread.
     */
    fun isInitialized(): Boolean = initialized

    /**
     * Get current frame statistics
     */
    fun getFrameStats(): FrameStats {
        return FrameStats(
            frameNumber = nativeGetFrameNumber(ownerTag),
            frameTimeMs = nativeGetFrameTimeMs(ownerTag),
            cpuTimeMs = nativeGetCpuTimeMs(ownerTag),
            gpuTimeMs = nativeGetGpuTimeMs(ownerTag),
            drawCalls = nativeGetDrawCalls(ownerTag),
            gpuMemoryUsed = nativeGetGpuMemoryUsed(ownerTag),
            cpuMemoryUsed = nativeGetCpuMemoryUsed(ownerTag)
        )
    }

    /**
     * Get GPU information
     */
    fun getGpuInfo(): GpuInfo {
        return GpuInfo(
            rendererString = nativeGetGpuRendererString(ownerTag),
            vendorString = nativeGetGpuVendorString(ownerTag),
            versionString = nativeGetGpuVersionString(ownerTag),
            vendor = GpuVendor.entries.getOrElse(nativeGetGpuVendor(ownerTag)) { GpuVendor.UNKNOWN },
            architecture = GpuArchitecture.entries.getOrElse(nativeGetGpuArchitecture(ownerTag)) { GpuArchitecture.UNKNOWN },
            supportsVulkan = currentBackend == RendererBackend.VULKAN
        )
    }

    /**
     * Check if a feature is supported
     */
    fun supportsFeature(feature: RendererFeature): Boolean {
        // The bit value is the wire format; ordinals only match by coincidence.
        return nativeSupportsFeature(feature.bit, ownerTag)
    }

    /**
     * Check if an extension is supported
     */
    fun isExtensionSupported(extension: String): Boolean {
        return nativeIsExtensionSupported(extension, ownerTag)
    }

    /**
     * Handle surface changes
     */
    fun onSurfaceChanged(width: Int, height: Int) {
        surfaceWidth = width
        surfaceHeight = height
        nativeOnSurfaceChanged(width, height, ownerTag)
    }

    /**
     * Handle surface destruction
     */
    fun onSurfaceDestroyed() {
        nativeOnSurfaceDestroyed(ownerTag)
        surface = null
    }

    /**
     * Handle memory pressure
     */
    fun onMemoryPressure(level: Int) {
        nativeOnMemoryPressure(level, ownerTag)
    }

    /**
     * Handle thermal throttling
     */
    fun onThermalThrottling(temperatureRatio: Float) {
        nativeOnThermalThrottling(temperatureRatio, ownerTag)
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
        nativeWaitIdle(ownerTag)
    }

    /**
     * Shutdown the renderer
     */
    override fun close() {
        shutdown()
    }

    @androidx.annotation.WorkerThread
    fun shutdown() {
        // Stop and join the render thread BEFORE tearing down the native
        // renderer: a frame that is still in flight would otherwise call into a
        // destroyed renderer, and the EGL context would be released while the
        // thread that owns it is still running.
        renderLoopRunning = false
        // Unblock anything waiting on submitted work; it will never run now.
        renderWork.clear()
        val thread = renderThread
        renderThread = null
        if (thread != null) {
            thread.interrupt()
            thread.join(RENDER_THREAD_JOIN_TIMEOUT_MS)
            if (thread.isAlive) {
                Log.w(TAG, "render thread did not stop within ${RENDER_THREAD_JOIN_TIMEOUT_MS}ms")
            }
        }
        if (initialized) {
            nativeShutdown(ownerTag)
            initialized = false
            Log.i(TAG, "Renderer shutdown")
        }
    }

    @OnLifecycleEvent(Lifecycle.Event.ON_DESTROY)
    fun onDestroy() {
        shutdown()
    }

    // Native methods
    external private fun nativeInitialize(surface: Surface, preferredBackend: Int, enableValidation: Boolean, enableDebugMarkers: Boolean, enableProfiling: Boolean, enableMultithreaded: Boolean, enableAsyncShaderCompilation: Boolean, enableAsyncResourceLoading: Boolean, enableResourcePooling: Boolean, enableCommandBufferReuse: Boolean, enableStateCaching: Boolean, enableDrawCallBatching: Boolean, enablePipelineCaching: Boolean, enableDescriptorCaching: Boolean, enableTextureStreaming: Boolean, enableTextureCompression: Boolean, enableMipmapGeneration: Boolean, maxFramesInFlight: Int, maxCommandBuffersPerFrame: Int, maxDescriptorSets: Int, maxPushConstantsSize: Int, textureCacheSizeMb: Int, shaderCacheSizeMb: Int, bufferPoolSizeMb: Int, frameTimeoutMs: Int, vsyncEnabled: Boolean, targetFps: Int, lowLatencyMode: Boolean, batterySaverMode: Boolean, thermalThrottlingAware: Boolean, thermalThrottleThreshold: Float, ownerTag: Long): Boolean

    external private fun nativeShutdown(ownerTag: Long)
    external private fun nativeBeginFrame(ownerTag: Long): Boolean
    external private fun nativeEndFrame(ownerTag: Long)
    external private fun nativePresent(ownerTag: Long)
    external private fun nativeOnSurfaceCreated(surface: Surface, ownerTag: Long)
    external private fun nativeOnSurfaceChanged(width: Int, height: Int, ownerTag: Long)
    external private fun nativeOnSurfaceDestroyed(ownerTag: Long)
    external private fun nativeOnMemoryPressure(level: Int, ownerTag: Long)
    external private fun nativeOnThermalThrottling(temperatureRatio: Float, ownerTag: Long)
    external private fun nativeGetBackend(ownerTag: Long): Int
    external private fun nativeIsInitialized(ownerTag: Long): Boolean
    external private fun nativeWaitIdle(ownerTag: Long)

    // Frame stats
    external private fun nativeGetFrameNumber(ownerTag: Long): Long
    external private fun nativeGetFrameTimeMs(ownerTag: Long): Double
    external private fun nativeGetCpuTimeMs(ownerTag: Long): Double
    external private fun nativeGetGpuTimeMs(ownerTag: Long): Double
    external private fun nativeGetDrawCalls(ownerTag: Long): Int
    external private fun nativeGetGpuMemoryUsed(ownerTag: Long): Long
    external private fun nativeGetCpuMemoryUsed(ownerTag: Long): Long
    external private fun nativeResetFrameStats(ownerTag: Long)

    // GPU info
    external private fun nativeGetGpuRendererString(ownerTag: Long): String
    external private fun nativeGetGpuVendorString(ownerTag: Long): String
    external private fun nativeGetGpuVersionString(ownerTag: Long): String
    external private fun nativeGetGpuVendor(ownerTag: Long): Int
    external private fun nativeGetGpuArchitecture(ownerTag: Long): Int

    // Feature queries
    external private fun nativeSupportsFeature(feature: Int, ownerTag: Long): Boolean
    external private fun nativeIsExtensionSupported(extension: String, ownerTag: Long): Boolean

    // -----------------------------------------------------------------------
    // Manager system. nativeAreManagersReady is the honest check that the
    // backend finished wiring its managers; every accessor below degrades
    // safely when it returns false rather than pretending to have succeeded.
    // -----------------------------------------------------------------------
    external private fun nativeAreManagersReady(ownerTag: Long): Boolean

    external private fun nativeCreateBuffer(size: Long, usage: Int, ownerTag: Long): Long
    external private fun nativeDestroyBuffer(handle: Long, ownerTag: Long)
    external private fun nativeUpdateBuffer(handle: Long, offset: Long, data: ByteArray, ownerTag: Long): Boolean

    external private fun nativeCreateTexture2D(width: Int, height: Int, format: Int, usage: Int, mipLevels: Int, ownerTag: Long): Long
    external private fun nativeUploadTexture(handle: Long, mipLevel: Int, data: ByteArray, ownerTag: Long): Boolean
    external private fun nativeDestroyTexture(handle: Long, ownerTag: Long)

    external private fun nativeCreateShader(stage: Int, source: String, defines: Array<String>, ownerTag: Long): Long
    external private fun nativeDestroyShader(handle: Long, ownerTag: Long)
    external private fun nativeCreateGraphicsPipeline(vertexShader: Long, fragmentShader: Long, ownerTag: Long): Long
    external private fun nativeDestroyPipeline(handle: Long, ownerTag: Long)

    external private fun nativeBindPipeline(pipeline: Long, ownerTag: Long)
    external private fun nativeBindVertexBuffers(firstBinding: Int, buffers: LongArray, offsets: IntArray, ownerTag: Long)
    external private fun nativeBindIndexBuffer(buffer: Long, indexType: Int, ownerTag: Long)
    external private fun nativeSetViewport(x: Float, y: Float, width: Float, height: Float, ownerTag: Long)
    external private fun nativeSetScissor(x: Int, y: Int, width: Int, height: Int, ownerTag: Long)
    external private fun nativeBindFramebuffer(framebuffer: Long, ownerTag: Long)
    external private fun nativeDraw(vertexCount: Int, instanceCount: Int, firstVertex: Int, firstInstance: Int, ownerTag: Long)
    external private fun nativeDrawIndexed(indexCount: Int, instanceCount: Int, firstIndex: Int, vertexOffset: Int, firstInstance: Int, ownerTag: Long)

    // -----------------------------------------------------------------------
    // Resource lifetime
    // -----------------------------------------------------------------------

    /** True once the backend has constructed every manager the draw path needs. */
    fun areManagersReady(): Boolean = nativeAreManagersReady(ownerTag)

    /** Resets the per-frame counters reported by [getFrameTimeMs] and friends. */
    fun resetFrameStats() = nativeResetFrameStats(ownerTag)

    /**
     * Why the most recent shader compilation failed, or an empty string when it
     * succeeded.
     *
     * [createShader] reports failure by returning an invalid handle, which on its
     * own cannot distinguish "the compiler rejected this source" from "no
     * compiler is linked into this build" from "the backend refused the module".
     * This is what a caller logs or shows a user.
     */
    fun lastShaderError(): String = nativeLastShaderError(ownerTag)

    /**
     * Compilation and cache counters, for the whole process.
     *
     * `compiled` counts full GLSL to SPIR-V compilations and `memoryHits` counts
     * shaders served from the cache. Comparing them is how a caller tells a
     * working cache from an absent one — and the counters are readable whether or
     * not this renderer is initialized, because the cache is process-wide and a
     * shader may have been compiled through [compileToSpirv] alone.
     */
    fun shaderCompileStats(): ShaderCompileStats {
        val values = nativeShaderCompileStats()
        return ShaderCompileStats(
            compiled = values[0],
            memoryHits = values[1],
            diskHits = values[2],
            failures = values[3],
            totalMs = values[4],
        )
    }

    /**
     * Opens the on-disk SPIR-V cache under [directory]. Process-wide, and usable
     * before a renderer exists.
     */
    fun openShaderCache(directory: String) = nativeOpenShaderCache(directory)

    /**
     * Points `#include` resolution at a directory on disk, typically a game
     * install's shader directory.
     *
     * Minecraft writes its includes as `#include <minecraft:fog.glsl>`, where
     * `minecraft:` is a URI scheme rather than a folder, so the scheme is
     * stripped and the rest resolved under [root]. A `..` segment is refused: a
     * shader has no legitimate reason to read outside its own asset tree.
     *
     * A file is included at most once per translation unit, so ordinary include
     * guards work and a cycle terminates instead of recursing.
     *
     * Pass an empty string to remove the resolver, after which any include is
     * reported as unresolvable rather than being silently dropped.
     */
    fun setShaderIncludeRoot(root: String) = nativeSetShaderIncludeRoot(root)

    /**
     * Compiles GLSL to SPIR-V without creating a renderer, a device or a shader
     * module.
     *
     * Needs no GPU at all, which is what makes the translation path testable
     * anywhere rather than only on a device that happens to have Vulkan. Two
     * legitimate uses beyond testing: warming the SPIR-V cache during a loading
     * screen, and checking a precompiled `.spv` with [validateSpirv].
     *
     * The returned SPIR-V is what [createShader] hands to the Vulkan backend, and
     * it goes through the same process-wide cache. Calling this and then
     * [createShader] with the same source therefore compiles once, not twice.
     *
     * [error] is empty on success and otherwise carries the compiler's own
     * line-annotated diagnostics.
     */
    fun compileToSpirv(
        stage: ShaderStage,
        source: String,
        defines: Array<String> = emptyArray(),
    ): SpirvCompilation {
        val packed = nativeCompileToSpirv(stage.code, source, defines)
        // Layout: token, status, validated, compile ms, word count, words.
        val wordCount = packed[4].toInt()
        val spirv = IntArray(wordCount) { packed[5 + it].toInt() }
        val succeeded = packed[1] == 0L
        return SpirvCompilation(
            succeeded = succeeded,
            spirv = spirv,
            validated = packed[2] == 1L,
            compileMs = packed[3],
            // Asked for by token, so a concurrent compile cannot substitute its
            // own message for this one.
            error = if (succeeded) "" else nativeTranslationError(packed[0]),
        )
    }

    /**
     * Runs SPIR-V validation over words the caller already has, for example a
     * `.spv` shipped as an asset.
     *
     * Returns false for a module that is structurally not SPIR-V, or that fails
     * validation, or when this build has no validator linked.
     */
    fun validateSpirv(spirv: IntArray): Boolean = nativeValidateSpirv(spirv)

    external private fun nativeCompileToSpirv(
        stageCode: Int,
        source: String,
        defines: Array<String>,
    ): LongArray

    external private fun nativeTranslationError(token: Long): String

    external private fun nativeValidateSpirv(spirv: IntArray): Boolean

    external private fun nativeLastShaderError(ownerTag: Long): String

    external private fun nativeShaderCompileStats(): LongArray

    external private fun nativeOpenShaderCache(directory: String)

    external private fun nativeSetShaderIncludeRoot(root: String)

    /**
     * Opaque handle to a GPU resource. Zero means the resource could not be
     * created; every method below treats zero as a no-op instead of crashing.
     */
    class ResourceHandle internal constructor(@JvmField val value: Long) {
        val isValid: Boolean get() = value != 0L

        override fun toString(): String = "ResourceHandle(0x${java.lang.Long.toHexString(value)})"
    }

    /**
     * Buffer usage flags.
     *
     * These are bit flags mapped to each backend's own constants. An enum with
     * an explicit code is used rather than `const val` or an ordinal so that
     * reordering the entries can never silently change what reaches JNI.
     */
    enum class BufferUsage(val code: Int) {
        None(0),
        Vertex(1),
        Index(2),
        Uniform(4),
        Storage(8),
        TransferSrc(16),
        TransferDst(32),
        Indirect(64),
        ;

        companion object {
            /** Bitwise-or of the given flags. */
            fun of(vararg flags: BufferUsage): Int = flags.fold(0) { acc, flag -> acc or flag.code }
        }
    }

    /**
     * Texture format codes.
     *
     * The values are Copper Oxide's own, not `GLenum` or `VkFormat`, so the same
     * number means the same thing on both backends.
     */
    enum class TextureFormat(val code: Int) {
        None(0),
        R8(1),
        Rg8(2),
        Rgb8(3),
        Rgba8(4),
        Srgb8Alpha8(5),
        Rgba16f(8),
        R32f(10),
        Rgba32f(13),
        Depth16(16),
        Depth24Stencil8(19),
        Astc4x4(24),
        Etc2Rgba8(36),
    }

    /** Texture usage flags. */
    enum class TextureUsage(val code: Int) {
        None(0),
        Sampled(1),
        ColorAttachment(2),
        DepthAttachment(4),
        Storage(8),
        ;

        companion object {
            fun of(vararg flags: TextureUsage): Int = flags.fold(0) { acc, flag -> acc or flag.code }
        }
    }

    /** Shader stage, matching the native `ShaderStage` enum ordinal. */
    enum class ShaderStage(val code: Int) {
        Vertex(0),
        Fragment(1),
        Compute(2),
        Geometry(3),
    }

    /** Index width passed to [bindIndexBuffer] and [drawIndexed]. */
    enum class IndexType(val code: Int) {
        Uint16(0),
        Uint32(1),
    }

    // -----------------------------------------------------------------------
    // Resource lifetime
    // -----------------------------------------------------------------------

    /**
     * Creates a GPU buffer.
     *
     * Returns an invalid handle when the renderer is not initialized or the
     * backend has no buffer manager, so a caller that forgets to check
     * [isInitialized] degrades to "nothing is drawn" rather than a crash.
     */
    fun createBuffer(sizeBytes: Long, usage: BufferUsage = BufferUsage.Vertex): ResourceHandle {
        require(sizeBytes > 0) { "sizeBytes must be positive, was $sizeBytes" }
        return ResourceHandle(nativeCreateBuffer(sizeBytes, usage.code, ownerTag))
    }

    fun destroyBuffer(buffer: ResourceHandle) {
        if (buffer.isValid) nativeDestroyBuffer(buffer.value, ownerTag)
    }

    /** Uploads [data] to [buffer] at [offset]. */
    fun updateBuffer(buffer: ResourceHandle, offset: Long, data: ByteArray): Boolean {
        if (!buffer.isValid || data.isEmpty()) return false
        return nativeUpdateBuffer(buffer.value, offset, data, ownerTag)
    }

    /** Creates a 2D texture. Returns an invalid handle when creation fails. */
    fun createTexture2D(
        width: Int,
        height: Int,
        format: TextureFormat = TextureFormat.Rgba8,
        usage: Int = TextureUsage.Sampled.code,
        mipLevels: Int = 1
    ): ResourceHandle {
        require(width > 0 && height > 0) { "texture extent must be positive" }
        return ResourceHandle(nativeCreateTexture2D(width, height, format.code, usage, mipLevels, ownerTag))
    }

    /** Uploads tightly packed pixels to one mip level of [texture]. */
    fun uploadTexture(texture: ResourceHandle, mipLevel: Int, data: ByteArray): Boolean {
        if (!texture.isValid || data.isEmpty()) return false
        return nativeUploadTexture(texture.value, mipLevel, data, ownerTag)
    }

    fun destroyTexture(texture: ResourceHandle) {
        if (texture.isValid) nativeDestroyTexture(texture.value, ownerTag)
    }

    /**
     * Compiles [source] as a GLSL shader for [stage].
     *
     * Returns an invalid handle when the source does not compile or the backend
     * has no shader compiler. The OpenGL ES backend compiles GLSL directly; the
     * Vulkan backend has no translator wired up yet and reports failure instead
     * of pretending to have produced a module.
     */
    fun createShader(stage: ShaderStage, source: String, defines: Array<String> = emptyArray()): ResourceHandle =
        ResourceHandle(nativeCreateShader(stage.code, source, defines, ownerTag))

    fun destroyShader(shader: ResourceHandle) {
        if (shader.isValid) nativeDestroyShader(shader.value, ownerTag)
    }

    /**
     * Creates a pipeline from an existing vertex and fragment shader.
     *
     * On Vulkan this is a real `VkPipeline`; on OpenGL ES, which has no pipeline
     * objects, it is a linked `GLuint` program. The caller API is the same.
     */
    fun createGraphicsPipeline(vertexShader: ResourceHandle, fragmentShader: ResourceHandle): ResourceHandle =
        ResourceHandle(nativeCreateGraphicsPipeline(vertexShader.value, fragmentShader.value, ownerTag))

    fun destroyPipeline(pipeline: ResourceHandle) {
        if (pipeline.isValid) nativeDestroyPipeline(pipeline.value, ownerTag)
    }

    // -----------------------------------------------------------------------
    // Recording
    // -----------------------------------------------------------------------

    fun bindPipeline(pipeline: ResourceHandle) = nativeBindPipeline(pipeline.value, ownerTag)

    fun bindVertexBuffer(binding: Int, buffer: ResourceHandle, offset: Int = 0) =
        nativeBindVertexBuffers(binding, longArrayOf(buffer.value), intArrayOf(offset), ownerTag)

    fun bindVertexBuffers(
        buffers: LongArray,
        offsets: IntArray = IntArray(buffers.size),
        firstBinding: Int = 0
    ) {
        require(buffers.size == offsets.size) { "buffers and offsets must be the same length" }
        nativeBindVertexBuffers(firstBinding, buffers, offsets, ownerTag)
    }

    fun bindIndexBuffer(buffer: ResourceHandle, indexType: IndexType = IndexType.Uint16) =
        nativeBindIndexBuffer(buffer.value, indexType.code, ownerTag)

    fun setViewport(x: Float, y: Float, width: Float, height: Float) =
        nativeSetViewport(x, y, width, height, ownerTag)

    fun setScissor(x: Int, y: Int, width: Int, height: Int) = nativeSetScissor(x, y, width, height, ownerTag)

    fun bindFramebuffer(framebuffer: ResourceHandle) = nativeBindFramebuffer(framebuffer.value, ownerTag)

    fun draw(
        vertexCount: Int,
        instanceCount: Int = 1,
        firstVertex: Int = 0,
        firstInstance: Int = 0
    ) = nativeDraw(vertexCount, instanceCount, firstVertex, firstInstance, ownerTag)

    fun drawIndexed(
        indexCount: Int,
        instanceCount: Int = 1,
        firstIndex: Int = 0,
        vertexOffset: Int = 0,
        firstInstance: Int = 0
    ) = nativeDrawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance, ownerTag)
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
        val Release = RendererConfig(
            enableValidation = false,
            enableDebugMarkers = false,
            enableProfiling = true,
            targetFps = 60
        )
    }

    /**
     * True when every value is in a range the native side can honour.
     *
     * Native `RendererConfig::validate` performs the same check, so a config that
     * passes here is one the C++ layer will not reject at initialize time.
     */
    fun validate(): Boolean =
        maxFramesInFlight in 1..8 &&
            maxCommandBuffersPerFrame in 1..1024 &&
            maxDescriptorSets in 1..1_000_000 &&
            maxPushConstantsSize in 1..4096 &&
            textureCacheSizeMb in 0..8192 &&
            shaderCacheSizeMb in 0..8192 &&
            bufferPoolSizeMb in 0..8192 &&
            frameTimeoutMs in 1..120_000 &&
            targetFps in 1..240 &&
            thermalThrottleThreshold in 0.0f..1.0f

    /**
     * Returns a copy with every out-of-range value moved back into range.
     *
     * Clamping rather than rejecting is deliberate: a caller that builds a config
     * from device properties can hand over nonsense, and refusing to start is a
     * worse failure than starting with a safe value. [validate] still gates, so
     * a config that was never clamped is still rejected.
     */
    fun clampToValidRanges(): RendererConfig = copy(
        maxFramesInFlight = maxFramesInFlight.coerceIn(1, 8),
        maxCommandBuffersPerFrame = maxCommandBuffersPerFrame.coerceIn(1, 1024),
        maxDescriptorSets = maxDescriptorSets.coerceIn(1, 1_000_000),
        maxPushConstantsSize = maxPushConstantsSize.coerceIn(1, 4096),
        textureCacheSizeMb = textureCacheSizeMb.coerceIn(0, 8192),
        shaderCacheSizeMb = shaderCacheSizeMb.coerceIn(0, 8192),
        bufferPoolSizeMb = bufferPoolSizeMb.coerceIn(0, 8192),
        frameTimeoutMs = frameTimeoutMs.coerceIn(1, 120_000),
        targetFps = targetFps.coerceIn(1, 240),
        thermalThrottleThreshold = thermalThrottleThreshold.coerceIn(0.0f, 1.0f)
    )
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
/**
 * The outcome of a [CopperOxideRenderer.compileToSpirv] call.
 *
 * [validated] means spirv-val ran and passed, which is different from "this
 * build has no validator": the first is a claim about the module, the second is
 * a claim about the binary.
 */
data class SpirvCompilation(
    val succeeded: Boolean,
    val spirv: IntArray,
    val validated: Boolean,
    val compileMs: Long,
    val error: String,
) {
    /** The SPIR-V magic number, little endian, that word 0 must carry. */
    val looksLikeSpirv: Boolean get() = spirv.size >= 5 && spirv[0] == 0x07230203

    override fun equals(other: Any?): Boolean =
        this === other ||
            (
                other is SpirvCompilation &&
                    succeeded == other.succeeded &&
                    spirv.contentEquals(other.spirv) &&
                    validated == other.validated &&
                    compileMs == other.compileMs &&
                    error == other.error
                )

    override fun hashCode(): Int {
        var result = succeeded.hashCode()
        result = 31 * result + spirv.contentHashCode()
        result = 31 * result + validated.hashCode()
        result = 31 * result + compileMs.hashCode()
        result = 31 * result + error.hashCode()
        return result
    }
}

/**
 * Shader compilation and cache counters.
 *
 * See [CopperOxideRenderer.shaderCompileStats].
 */
data class ShaderCompileStats(
    val compiled: Long = 0,
    val memoryHits: Long = 0,
    val diskHits: Long = 0,
    val failures: Long = 0,
    val totalMs: Long = 0,
)

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