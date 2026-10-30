package com.core.desktop.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

private val DarkColorScheme = darkColorScheme(
    primary = DesktopColors.Accent,
    background = DesktopColors.BackgroundBase,
    surface = DesktopColors.SurfaceCard,
    onPrimary = DesktopColors.TextPrimary,
    onBackground = DesktopColors.TextPrimary,
    onSurface = DesktopColors.TextPrimary
)

@Composable
fun NativeDesktopTheme(
    content: @Composable () -> Unit
) {
    MaterialTheme(
        colorScheme = DarkColorScheme,
        typography = DesktopTypography,
        shapes = DesktopShapes,
        content = content
    )
}

