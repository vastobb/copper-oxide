package com.oxide.mc.copperoxide.gui

import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.oxide.mc.copperoxide.renderer.CopperOxideRenderer
import com.oxide.mc.copperoxide.renderer.FrameStats
import com.oxide.mc.copperoxide.renderer.GpuInfo
import com.oxide.mc.copperoxide.renderer.RendererBackend
import com.oxide.mc.copperoxide.renderer.RendererConfig
import com.oxide.mc.copperoxide.ui.theme.CopperOxideTheme
import kotlinx.coroutines.delay

/**
 * Minimal diagnostics harness for the Copper Oxide renderer.
 *
 * This activity exists only to give the renderer a surface and expose its
 * diagnostics. Copper Oxide itself is a renderer library; launchers own their
 * own UI and embed the renderer directly.
 */
@OptIn(ExperimentalMaterial3Api::class)
class MainActivity : ComponentActivity() {

    private var renderer: CopperOxideRenderer? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            CopperOxideTheme {
                DiagnosticsScreen()
            }
        }
    }

    override fun onDestroy() {
        renderer?.shutdown()
        renderer = null
        super.onDestroy()
    }

    @Composable
    private fun DiagnosticsScreen() {
        val context = androidx.compose.ui.platform.LocalContext.current
        var surfaceView by remember { mutableStateOf<SurfaceView?>(null) }
        var surfaceHolder by remember { mutableStateOf<SurfaceHolder?>(null) }
        var gpuInfo by remember { mutableStateOf<GpuInfo?>(null) }
        var frameStats by remember { mutableStateOf<FrameStats?>(null) }
        var backend by remember { mutableStateOf<RendererBackend?>(null) }
        var status by remember { mutableStateOf("Renderer not initialized") }

        LaunchedEffect(surfaceHolder) {
            val holder = surfaceHolder ?: return@LaunchedEffect
            if (holder.surface.isValid) {
                val instance = CopperOxideRenderer(
                    context.applicationContext,
                    RendererConfig.Default
                )
                if (instance.initialize(holder.surface)) {
                    renderer = instance
                    backend = instance.currentBackend()
                    gpuInfo = instance.getGpuInfo()
                    status = "Renderer running on ${instance.currentBackend()}"
                } else {
                    instance.shutdown()
                    status = "Renderer initialization failed"
                }
            }
        }

        LaunchedEffect(renderer) {
            while (renderer != null) {
                frameStats = renderer?.getFrameStats()
                delay(500)
            }
        }

        Surface(modifier = Modifier.fillMaxSize()) {
            Column(
                modifier = Modifier.fillMaxSize().padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                Text("Copper Oxide Renderer", style = MaterialTheme.typography.headlineSmall)
                Text(status, style = MaterialTheme.typography.bodyMedium)

                Box(modifier = Modifier.fillMaxWidth().weight(1f, fill = true)) {
                    AndroidView(
                        modifier = Modifier.fillMaxSize(),
                        factory = { ctx ->
                            SurfaceView(ctx).also { view ->
                                surfaceView = view
                                surfaceHolder = view.holder
                            }
                        }
                    )
                }

                GpuCard(gpuInfo)
                StatsCard(frameStats)

                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = {
                        renderer?.shutdown()
                        renderer = null
                        status = "Renderer stopped"
                    }) { Text("Stop") }
                }
            }
        }
    }
}

@Composable
private fun GpuCard(gpuInfo: GpuInfo?) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(12.dp)) {
            Text("GPU", style = MaterialTheme.typography.titleMedium)
            Text(
                text = if (gpuInfo == null) {
                    "No data"
                } else {
                    "${gpuInfo.vendor} ${gpuInfo.architecture}\n${gpuInfo.rendererString}\n" +
                        "GL: ${gpuInfo.versionString}"
                },
                style = MaterialTheme.typography.bodySmall
            )
        }
    }
}

@Composable
private fun StatsCard(frameStats: FrameStats?) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(12.dp)) {
            Text("Frame statistics", style = MaterialTheme.typography.titleMedium)
            if (frameStats == null) {
                Text("No frame data", style = MaterialTheme.typography.bodySmall)
            } else {
                Text(
                    text = "FPS: ${"%.1f".format(frameStats.fps)}\n" +
                        "Frame: ${"%.2f".format(frameStats.frameTimeMs)} ms\n" +
                        "CPU: ${"%.2f".format(frameStats.cpuTimeMs)} ms\n" +
                        "GPU: ${"%.2f".format(frameStats.gpuTimeMs)} ms\n" +
                        "Draw calls: ${frameStats.drawCalls}",
                    style = MaterialTheme.typography.bodySmall
                )
            }
        }
    }
}