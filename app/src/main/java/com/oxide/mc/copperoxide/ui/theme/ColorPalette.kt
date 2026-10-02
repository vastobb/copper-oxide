package com.oxide.mc.copperoxide.ui.theme

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.material.icons.Default
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.DeveloperBoard
import androidx.compose.material.icons.filled.BatteryChargingFull
import androidx.compose.material.icons.filled.Speed
import androidx.compose.material.icons.filled.Architecture
import androidx.compose.material.icons.filled.Badge
import androidx.compose.material.icons.filled.Verified
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Cancel
import androidx.compose.material.icons.filled.TrendingUp
import androidx.compose.material.icons.filled.TrendingDown
import androidx.compose.material.icons.filled.Remove

object ColorPalette {
    // Copper-inspired color palette
    val Copper = Color(0xFFB87333)        // Main copper
    val CopperLight = Color(0xFFCD853F)   // Light copper
    val CopperDark = Color(0xFF9B5D2B)    // Dark copper
    val CopperOxide = Color(0xFF6B8E6B)   // Copper oxide green
    val CopperOxideLight = Color(0xFF8FAF8F)
    val CopperOxideDark = Color(0xFF4A6F4A)

    // Accent colors
    val ElectricBlue = Color(0xFF00BFFF)
    val NeonGreen = Color(0xFF39FF14)
    val HotPink = Color(0xFFFF1493)
    val VoltagePurple = Color(0xFF8A2BE2)
    val Gold = Color(0xFFFFD700)

    // Semantic colors
    val Success = Color(0xFF4CAF50)
    val Warning = Color(0xFFFF9800)
    val Error = Color(0xFFF44336)
    val Info = Color(0xFF2196F3)

    // Surface colors
    val SurfaceElevated = Color(0xFF1E1E1E)
    val SurfaceContainer = Color(0xFF2C2C2C)
    val SurfaceContainerHigh = Color(0xFF383838)
    val SurfaceContainerHighest = Color(0xFF444444)

    // GPU vendor colors
    fun gpuVendorColor(vendor: com.oxide.mc.copperoxide.renderer.GpuVendor): Color = when (vendor) {
        com.oxide.mc.copperoxide.renderer.GpuVendor.ADRENO -> Color(0xFFE91E63)      // Pink/Red
        com.oxide.mc.copperoxide.renderer.GpuVendor.MALI -> Color(0xFF00BCD4)         // Cyan
        com.oxide.mc.copperoxide.renderer.GpuVendor.POWERVR -> Color(0xFF9C27B0)      // Purple
        com.oxide.mc.copperoxide.renderer.GpuVendor.APPLE -> Color(0xFF607D8B)        // Blue Grey
        com.oxide.mc.copperoxide.renderer.GpuVendor.NVIDIA -> Color(0xFF76B900)       // NVIDIA Green
        com.oxide.mc.copperoxide.renderer.GpuVendor.AMD -> Color(0xFFED1C24)          // AMD Red
        com.oxide.mc.copperoxide.renderer.GpuVendor.INTEL -> Color(0xFF0071C5)        // Intel Blue
        com.oxide.mc.copperoxide.renderer.GpuVendor.BROADCOM -> Color(0xFFFF6F00)     // Orange
        com.oxide.mc.copperoxide.renderer.GpuVendor.VIVANTE -> Color(0xFF009688)      // Teal
        com.oxide.mc.copperoxide.renderer.GpuVendor.VERISILICON -> Color(0xFF673AB7)   // Deep Purple
        else -> Copper
    }

    fun gpuVendorIcon(vendor: com.oxide.mc.copperoxide.renderer.GpuVendor): ImageVector = when (vendor) {
        com.oxide.mc.copperoxide.renderer.GpuVendor.ADRENO -> Default.Memory
        com.oxide.mc.copperoxide.renderer.GpuVendor.MALI -> Default.DeveloperBoard
        com.oxide.mc.copperoxide.renderer.GpuVendor.POWERVR -> Default.BatteryChargingFull
        com.oxide.mc.copperoxide.renderer.GpuVendor.APPLE -> Default.Speed
        com.oxide.mc.copperoxide.renderer.GpuVendor.NVIDIA -> Default.Architecture
        com.oxide.mc.copperoxide.renderer.GpuVendor.AMD -> Default.Badge
        com.oxide.mc.copperoxide.renderer.GpuVendor.INTEL -> Default.Verified
        com.oxide.mc.copperoxide.renderer.GpuVendor.BROADCOM -> Default.CheckCircle
        com.oxide.mc.copperoxide.renderer.GpuVendor.VIVANTE -> Default.Cancel
        com.oxide.mc.copperoxide.renderer.GpuVendor.VERISILICON -> Default.TrendingUp
        else -> Default.Memory
    }

    // Trend colors
    val TrendUp = Success
    val TrendDown = Error
    val TrendStable = Color(0xFF9E9E9E)
}