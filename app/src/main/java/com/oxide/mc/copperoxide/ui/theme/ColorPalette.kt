package com.oxide.mc.copperoxide.ui.theme

import androidx.compose.ui.graphics.Color
import com.oxide.mc.copperoxide.renderer.GpuVendor

/** Copper-inspired palette used by the diagnostics UI. */
object ColorPalette {
    val Copper = Color(0xFFB87333)
    val CopperLight = Color(0xFFCD853F)
    val CopperDark = Color(0xFF9B5D2B)
    val CopperOxide = Color(0xFF6B8E6B)
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

    /** Per-vendor accent color used in diagnostics output. */
    fun gpuVendorColor(vendor: GpuVendor): Color = when (vendor) {
        GpuVendor.ADRENO -> Color(0xFFE91E63)
        GpuVendor.MALI -> Color(0xFF00BCD4)
        GpuVendor.POWERVR -> Color(0xFF9C27B0)
        GpuVendor.APPLE -> Color(0xFF607D8B)
        GpuVendor.NVIDIA -> Color(0xFF76B900)
        GpuVendor.AMD -> Color(0xFFED1C24)
        GpuVendor.INTEL -> Color(0xFF0071C5)
        GpuVendor.BROADCOM -> Color(0xFFFF6F00)
        GpuVendor.VIVANTE -> Color(0xFF009688)
        GpuVendor.VERISILICON -> Color(0xFF673AB7)
        GpuVendor.UNKNOWN -> Copper
    }
}