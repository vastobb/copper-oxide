package com.oxide.mc.copperoxide.gui

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import com.oxide.mc.copperoxide.renderer.CopperOxideRenderer
import com.oxide.mc.copperoxide.renderer.FrameStats
import com.oxide.mc.copperoxide.renderer.GpuInfo
import com.oxide.mc.copperoxide.renderer.RenderConfig
import com.oxide.mc.copperoxide.renderer.RendererBackend
import com.oxide.mc.copperoxide.ui.theme.CopperOxideTheme
import com.oxide.mc.copperoxide.ui.theme.ColorPalette
import kotlinx.coroutines.launch

@OptIn(ExperimentalMaterial3Api::class)
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            CopperOxideTheme {
                MainScreen()
            }
        }
    }
}

@Composable
fun MainScreen() {
    val renderer by remember { mutableStateOf<CopperOxideRenderer?>(null) }
    val currentTab by remember { mutableStateOf(0) }
    val gpuInfo by remember { mutableStateOf<GpuInfo?>(null) }
    val frameStats by remember { mutableStateOf<FrameStats?>(null) }
    val showSettings by remember { mutableStateOf(false) }
    val showDiagnostics by remember { mutableStateOf(false) }

    // Initialize renderer
    // In a real app, this would be done with a proper surface from a SurfaceView/TextureView

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Copper Oxide", style = MaterialTheme.typography.headlineSmall) },
                navigationIcon = {
                    IconButton(onClick = { showSettings = true }) {
                        Icon(Icons.Default.Settings, contentDescription = "Settings")
                    }
                },
                actions = {
                    IconButton(onClick = { showDiagnostics = true }) {
                        Icon(Icons.Default.MonitorHeart, contentDescription = "Diagnostics")
                    }
                    IconButton(onClick = { /* Show about */ }) {
                        Icon(Icons.Default.Info, contentDescription = "About")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.primaryContainer,
                    titleContentColor = MaterialTheme.colorScheme.onPrimaryContainer
                )
            )
        },
        floatingActionButton = {
            ExtendedFloatingActionButton(
                onClick = { /* Start/Stop rendering */ },
                icon = { Icon(Icons.Default.PlayArrow, contentDescription = "Start") },
                text = { Text("Start Rendering") },
                expanded = true
            )
        }
    ) { padding ->
        Box(modifier = Modifier.padding(padding).fillMaxSize()) {
            // Main content based on tab
            when (currentTab) {
                0 -> HomeTab(renderer, gpuInfo, frameStats)
                1 -> PerformanceTab(frameStats)
                2 -> CompatibilityTab()
                3 -> SettingsTab()
                4 -> AboutTab()
            }

            // Bottom navigation
            BottomNavigation(
                modifier = Modifier.align(Alignment.BottomCenter)
            ) {
                val tabs = listOf(
                    TabItem(Icons.Default.Home, "Home", 0),
                    TabItem(Icons.Default.Speed, "Performance", 1),
                    TabItem(Icons.Default.CheckCircle, "Compatibility", 2),
                    TabItem(Icons.Default.Tune, "Settings", 3),
                    TabItem(Icons.Default.Info, "About", 4)
                )

                tabs.forEach { tab ->
                    BottomNavigationItem(
                        icon = { Icon(tab.icon, contentDescription = null) },
                        label = { Text(tab.label) },
                        selected = currentTab == tab.index,
                        onClick = { currentTab = tab.index },
                        selectedContentColor = MaterialTheme.colorScheme.primary,
                        unselectedContentColor = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }
    }
}

@Composable
fun HomeTab(
    renderer: CopperOxideRenderer?,
    gpuInfo: GpuInfo?,
    frameStats: FrameStats?
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        // GPU Info Card
        Card(
            modifier = Modifier.fillMaxWidth(),
            elevation = CardDefaults.cardElevation(defaultElevation = 8.dp)
        ) {
            CardContent(
                title = "GPU Information",
                icon = Icons.Default.Memory
            ) {
                if (gpuInfo != null) {
                    GpuInfoCard(gpuInfo)
                } else {
                    PlaceholderCard("Initializing GPU detection...")
                }
            }
        }

        // Renderer Status Card
        Card(
            modifier = Modifier.fillMaxWidth(),
            elevation = CardDefaults.cardElevation(defaultElevation = 8.dp)
        ) {
            CardContent(
                title = "Renderer Status",
                icon = Icons.Default.DeveloperMode
            ) {
                RendererStatusCard(renderer)
            }
        }

        // Quick Stats Card
        Card(
            modifier = Modifier.fillMaxWidth(),
            elevation = CardDefaults.cardElevation(defaultElevation = 8.dp)
        ) {
            CardContent(
                title = "Live Statistics",
                icon = Icons.Default.Analytics
            ) {
                if (frameStats != null) {
                    QuickStatsCard(frameStats)
                } else {
                    PlaceholderCard("Waiting for frame data...")
                }
            }
        }

        // Feature Support Card
        Card(
            modifier = Modifier.fillMaxWidth(),
            elevation = CardDefaults.cardElevation(defaultElevation = 8.dp)
        ) {
            CardContent(
                title = "Feature Support",
                icon = Icons.Default.FeaturedPlayList
            ) {
                FeatureSupportCard(renderer)
            }
        }
    }
}

@Composable
fun GpuInfoCard(gpuInfo: GpuInfo) {
    Column(Modifier.padding(16.dp), horizontalAlignment = Alignment.CenterHorizontally) {
        // GPU Vendor Icon
        Box(
            modifier = Modifier
                .size(80.dp)
                .background(
                    ColorPalette.gpuVendorColor(gpuInfo.vendor),
                    CircleShape
                )
                .padding(16.dp)
        ) {
            Icon(
                imageVector = ColorPalette.gpuVendorIcon(gpuInfo.vendor),
                contentDescription = null,
                tint = Color.White
            )
        }

        Spacer(Modifier.height(12.dp))

        Text(
            gpuInfo.rendererString,
            style = MaterialTheme.typography.headlineSmall,
            textAlign = TextAlign.Center,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis
        )

        Spacer(Modifier.height(4.dp))

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.Center,
            horizontalGap = 16.dp
        ) {
            InfoChip(
                icon = Icons.Default.Badge,
                label = gpuInfo.vendor.name,
                color = ColorPalette.gpuVendorColor(gpuInfo.vendor)
            )
            InfoChip(
                icon = Icons.Default.Architecture,
                label = gpuInfo.architecture.name,
                color = MaterialTheme.colorScheme.secondary
            )
        }

        Spacer(Modifier.height(8.dp))

        Text(
            "Driver: ${gpuInfo.versionString}",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        if (gpuInfo.supportsVulkan) {
            Spacer(Modifier.height(8.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.Center
            ) {
                Icon(Icons.Default.Verified, tint = MaterialTheme.colorScheme.primary, contentDescription = null)
                Spacer(Modifier.width(4.dp))
                Text(
                    "Vulkan Supported",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.primary
                )
            }
        }
    }
}

@Composable
fun RendererStatusCard(renderer: CopperOxideRenderer?) {
    Column(Modifier.padding(16.dp)) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column {
                Text("Backend", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text(
                    renderer?.let { "Active" } ?: "Not Initialized",
                    style = MaterialTheme.typography.headlineMedium,
                    color = if (renderer != null) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error
                )
            }

            if (renderer != null) {
                RendererBackendIndicator(renderer)
            }
        }

        Spacer(Modifier.height(16.dp))

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            StatusIndicator(
                label = "VSync",
                value = "On",
                icon = Icons.Default.VideoStable,
                color = MaterialTheme.colorScheme.tertiary
            )
            StatusIndicator(
                label = "Low Latency",
                value = "Off",
                icon = Icons.Default.Speed,
                color = MaterialTheme.colorScheme.secondary
            )
            StatusIndicator(
                label = "Battery Saver",
                value = "Off",
                icon = Icons.Default.BatterySaver,
                color = MaterialTheme.colorScheme.tertiary
            )
        }
    }
}

@Composable
fun RendererBackendIndicator(renderer: CopperOxideRenderer) {
    // This would show the actual backend from the renderer
    // For now, placeholder
    Box(
        modifier = Modifier
            .padding(horizontal = 12.dp, vertical = 6.dp)
            .background(MaterialTheme.colorScheme.primaryContainer, CornerRadius(16.dp))
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 4.dp)
        ) {
            Icon(
                Icons.Default.Memory,
                tint = MaterialTheme.colorScheme.onPrimaryContainer,
                contentDescription = null
            )
            Spacer(Modifier.width(4.dp))
            Text(
                "Vulkan", // Would come from renderer
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onPrimaryContainer
            )
        }
    }
}

@Composable
fun StatusIndicator(
    label: String,
    value: String,
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    color: androidx.compose.ui.graphics.Color
) {
    Column(
        modifier = Modifier
            .weight(1f)
            .padding(12.dp)
            .background(MaterialTheme.colorScheme.surfaceContainerHighest, CornerRadius(12.dp)),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Icon(icon, tint = color, contentDescription = null)
        Spacer(Modifier.height(4.dp))
        Text(value, style = MaterialTheme.typography.titleMedium, color = color)
        Text(label, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
fun QuickStatsCard(frameStats: FrameStats) {
    Column(Modifier.padding(16.dp)) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            QuickStatItem(
                label = "FPS",
                value = "%.1f".format(frameStats.fps),
                icon = Icons.Default.Speed,
                color = if (frameStats.fps >= 55) ColorPalette.Success else if (frameStats.fps >= 30) ColorPalette.Warning else ColorPalette.Error,
                trend = Trend.Stable
            )
            QuickStatItem(
                label = "Frame Time",
                value = "%.1f ms".format(frameStats.frameTimeMs),
                icon = Icons.Default.Timer,
                color = MaterialTheme.colorScheme.primary,
                trend = Trend.Stable
            )
            QuickStatItem(
                label = "CPU",
                value = "%.1f ms".format(frameStats.cpuTimeMs),
                icon = Icons.Default.Memory,
                color = MaterialTheme.colorScheme.secondary,
                trend = Trend.Stable
            )
            QuickStatItem(
                label = "GPU",
                value = "%.1f ms".format(frameStats.gpuTimeMs),
                icon = Icons.Default.DeveloperBoard,
                color = MaterialTheme.colorScheme.tertiary,
                trend = Trend.Stable
            )
        }

        Spacer(Modifier.height(12.dp))

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            QuickStatItem(
                label = "Draw Calls",
                value = frameStats.drawCalls.toString(),
                icon = Icons.Default.CallSplit,
                color = MaterialTheme.colorScheme.primary,
                trend = Trend.Stable
            )
            QuickStatItem(
                label = "GPU Mem",
                value = formatBytes(frameStats.gpuMemoryUsed),
                icon = Icons.Default.Storage,
                color = MaterialTheme.colorScheme.secondary,
                trend = Trend.Stable
            )
            QuickStatItem(
                label = "CPU Mem",
                value = formatBytes(frameStats.cpuMemoryUsed),
                icon = Icons.Default.DataUsage,
                color = MaterialTheme.colorScheme.tertiary,
                trend = Trend.Stable
            )
        }
    }
}

@Composable
fun QuickStatItem(
    label: String,
    value: String,
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    color: androidx.compose.ui.graphics.Color,
    trend: Trend
) {
    Column(
        modifier = Modifier
            .weight(1f)
            .padding(12.dp)
            .background(MaterialTheme.colorScheme.surfaceContainerHighest, CornerRadius(12.dp)),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Row(horizontalArrangement = Arrangement.Center) {
            Icon(icon, tint = color, contentDescription = null)
            Spacer(Modifier.width(4.dp))
            Text(value, style = MaterialTheme.typography.titleMedium, color = color)
            when (trend) {
                Trend.Up -> Icon(Icons.Default.TrendingUp, tint = ColorPalette.Success, contentDescription = "Increasing")
                Trend.Down -> Icon(Icons.Default.TrendingDown, tint = ColorPalette.Error, contentDescription = "Decreasing")
                Trend.Stable -> Icon(Icons.Default.Remove, tint = MaterialTheme.colorScheme.onSurfaceVariant, contentDescription = "Stable")
            }
        }
        Spacer(Modifier.height(4.dp))
        Text(label, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
fun FeatureSupportCard(renderer: CopperOxideRenderer?) {
    Column(Modifier.padding(16.dp)) {
        val features = listOf(
            FeatureItem("Compute Shaders", RendererFeature.COMPUTE_SHADERS),
            FeatureItem("Indirect Draw", RendererFeature.INDIRECT_DRAW),
            FeatureItem("Bindless Textures", RendererFeature.BINDLESS_TEXTURES),
            FeatureItem("Descriptor Indexing", RendererFeature.DESCRIPTOR_INDEXING),
            FeatureItem("Timeline Semaphore", RendererFeature.TIMELINE_SEMAPHORE),
            FeatureItem("Dynamic Rendering", RendererFeature.DYNAMIC_RENDERING),
            FeatureItem("Synchronization 2", RendererFeature.SYNCHRONIZATION_2),
            FeatureItem("Mesh Shaders", RendererFeature.MESH_SHADERS),
        )

        LazyColumn(
            modifier = Modifier.fillMaxWidth(),
            contentPadding = PaddingValues(0.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp)
        ) {
            items(features) { feature ->
                FeatureRow(
                    feature = feature,
                    supported = renderer?.supportsFeature(feature.feature) ?: false
                )
            }
        }
    }
}

@Composable
fun FeatureRow(feature: FeatureItem, supported: Boolean) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 8.dp)
            .background(
                if (supported) ColorPalette.Success.copy(alpha = 0.1f) else ColorPalette.Error.copy(alpha = 0.1f),
                CornerRadius(8.dp)
            ),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            feature.name,
            style = MaterialTheme.typography.bodyMedium,
            color = if (supported) ColorPalette.Success else MaterialTheme.colorScheme.onSurfaceVariant
        )
        Row(horizontalArrangement = Arrangement.Center, horizontalGap = 8.dp) {
            Icon(
                if (supported) Icons.Default.CheckCircle else Icons.Default.Cancel,
                tint = if (supported) ColorPalette.Success else ColorPalette.Error,
                contentDescription = if (supported) "Supported" : "Not Supported"
            )
            Text(
                if (supported) "Supported" : "Unavailable",
                style = MaterialTheme.typography.labelMedium,
                color = if (supported) ColorPalette.Success else ColorPalette.Error
            )
        }
    }
}

@Composable
fun PerformanceTab(frameStats: FrameStats?) {
    Column(Modifier.fillMaxSize().padding(16.dp)) {
        Text("Performance Monitor", style = MaterialTheme.typography.headlineMedium)
        Spacer(Modifier.height(16.dp))
        // Would show detailed performance graphs, frame time history, etc.
        PlaceholderCard("Performance graphs coming soon...")
    }
}

@Composable
fun CompatibilityTab() {
    Column(Modifier.fillMaxSize().padding(16.dp)) {
        Text("Compatibility Matrix", style = MaterialTheme.typography.headlineMedium)
        Spacer(Modifier.height(16.dp))
        // Would show mod/shader compatibility
        PlaceholderCard("Compatibility matrix coming soon...")
    }
}

@Composable
fun SettingsTab() {
    Column(Modifier.fillMaxSize().padding(16.dp)) {
        Text("Settings", style = MaterialTheme.typography.headlineMedium)
        Spacer(Modifier.height(16.dp))
        PlaceholderCard("Settings coming soon...")
    }
}

@Composable
fun AboutTab() {
    Column(Modifier.fillMaxSize().padding(16.dp)) {
        Text("About Copper Oxide", style = MaterialTheme.typography.headlineMedium)
        Spacer(Modifier.height(16.dp))

        Card(
            modifier = Modifier.fillMaxWidth(),
            elevation = CardDefaults.cardElevation(defaultElevation = 4.dp)
        ) {
            Column(Modifier.padding(24.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                Icon(
                    Icons.Default.Diamond,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.size(64.dp)
                )
                Spacer(Modifier.height(16.dp))
                Text("Copper Oxide", style = MaterialTheme.typography.headlineLarge)
                Text("Version 1.0.0", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Spacer(Modifier.height(16.dp))
                Text(
                    "High-performance Minecraft Java Edition rendering translation layer for Android.\nSupports Vulkan and OpenGL ES backends with automatic GPU-specific optimizations.",
                    style = MaterialTheme.typography.bodyMedium,
                    textAlign = TextAlign.Center,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Spacer(Modifier.height(16.dp))
                Text("Author: oxide-mc", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

@Composable
fun PlaceholderCard(message: String) {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .padding(16.dp)
            .height(120.dp)
            .background(MaterialTheme.colorScheme.surfaceContainerHighest, CornerRadius(12.dp)),
        contentAlignment = Alignment.Center
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Icon(Icons.Default.HourglassEmpty, tint = MaterialTheme.colorScheme.onSurfaceVariant, contentDescription = null, modifier = Modifier.size(32.dp))
            Spacer(Modifier.height(8.dp))
            Text(message, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, textAlign = TextAlign.Center)
        }
    }
}

@Composable
fun CardContent(
    title: String,
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    content: @Composable () -> Unit
) {
    Column(Modifier.padding(16.dp)) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                Icon(icon, tint = MaterialTheme.colorScheme.primary, contentDescription = null)
                Text(title, style = MaterialTheme.typography.titleLarge)
            }
        }
        Spacer(Modifier.height(8.dp))
        content()
    }
}

@Composable
fun InfoChip(icon: androidx.compose.ui.graphics.vector.ImageVector, label: String, color: androidx.compose.ui.graphics.Color) {
    Row(
        modifier = Modifier
            .padding(horizontal = 12.dp, vertical = 6.dp)
            .background(color.copy(alpha = 0.15f), CornerRadius(16.dp)),
        horizontalArrangement = Arrangement.spacedBy(4.dp)
    ) {
        Icon(icon, tint = color, contentDescription = null)
        Text(label, style = MaterialTheme.typography.labelMedium, color = color)
    }
}

data class TabItem(
    val icon: androidx.compose.ui.graphics.vector.ImageVector,
    val label: String,
    val index: Int
)

data class FeatureItem(
    val name: String,
    val feature: RendererFeature
)

enum class Trend { Up, Down, Stable }

fun formatBytes(bytes: Long): String {
    return when {
        bytes >= 1_073_741_824 -> "%.2f GB".format(bytes / 1_073_741_824.0)
        bytes >= 1_048_576 -> "%.2f MB".format(bytes / 1_048_576.0)
        bytes >= 1024 -> "%.2f KB".format(bytes / 1024.0)
        else -> "$bytes B"
    }
}