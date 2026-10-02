package com.oxide.mc.copperoxide.ui.theme

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp

private val DarkColorScheme = darkColorScheme(
    primary = ColorPalette.CopperLight,
    onPrimary = Color.Black,
    primaryContainer = ColorPalette.CopperDark,
    onPrimaryContainer = Color.White,
    secondary = ColorPalette.CopperOxideLight,
    onSecondary = Color.Black,
    secondaryContainer = ColorPalette.CopperOxideDark,
    onSecondaryContainer = Color.White,
    tertiary = ColorPalette.ElectricBlue,
    tertiaryContainer = ColorPalette.VoltagePurple,
    error = ColorPalette.Error,
    errorContainer = ColorPalette.Error.copy(alpha = 0.3f),
    onError = Color.White,
    background = Color(0xFF121212),
    onBackground = Color(0xFFE0E0E0),
    surface = Color(0xFF1E1E1E),
    onSurface = Color(0xFFE0E0E0),
    surfaceVariant = Color(0xFF2C2C2C),
    onSurfaceVariant = Color(0xFFBDBDBD),
    outline = Color(0xFF888888),
    scrim = Color.Black,
    inverseSurface = Color(0xFFE0E0E0),
    inverseOnSurface = Color(0xFF121212),
    inversePrimary = ColorPalette.CopperDark
)

private val LightColorScheme = lightColorScheme(
    primary = ColorPalette.Copper,
    onPrimary = Color.White,
    primaryContainer = ColorPalette.CopperDark,
    onPrimaryContainer = Color.White,
    secondary = ColorPalette.CopperOxide,
    onSecondary = Color.White,
    secondaryContainer = ColorPalette.CopperOxideLight,
    onSecondaryContainer = Color.Black,
    tertiary = ColorPalette.ElectricBlue,
    tertiaryContainer = ColorPalette.VoltagePurple,
    error = ColorPalette.Error,
    errorContainer = ColorPalette.Error.copy(alpha = 0.3f),
    onError = Color.White,
    background = Color.White,
    onBackground = Color(0xFF121212),
    surface = Color.White,
    onSurface = Color(0xFF121212),
    surfaceVariant = Color(0xFFE0E0E0),
    onSurfaceVariant = Color(0xFF444444),
    outline = Color(0xFF888888),
    scrim = Color.Black,
    inverseSurface = Color(0xFF121212),
    inverseOnSurface = Color.White,
    inversePrimary = ColorPalette.CopperLight
)

private val CopperOxideTypography = Typography()

private val CopperOxideShapes = Shapes(
    extraSmall = RoundedCornerShape(4.dp),
    small = RoundedCornerShape(8.dp),
    medium = RoundedCornerShape(12.dp),
    large = RoundedCornerShape(16.dp),
    extraLarge = RoundedCornerShape(24.dp)
)

@Composable
fun CopperOxideTheme(
    darkTheme: Boolean = true,
    content: @Composable () -> Unit
) {
    MaterialTheme(
        colorScheme = if (darkTheme) DarkColorScheme else LightColorScheme,
        typography = CopperOxideTypography,
        shapes = CopperOxideShapes,
        content = content
    )
}
