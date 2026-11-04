package com.leafcontrol.app.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val Dark = darkColorScheme(
    primary = Color(0xFF4DA3FF),
    onPrimary = Color(0xFF06121E),
    primaryContainer = Color(0xFF17324A),
    onPrimaryContainer = Color(0xFFD6E8FF),
    secondary = Color(0xFF8FD3A8),
    background = Color(0xFF0F141A),
    onBackground = Color(0xFFE6EDF3),
    surface = Color(0xFF181E26),
    onSurface = Color(0xFFE6EDF3),
    surfaceVariant = Color(0xFF232B35),
    onSurfaceVariant = Color(0xFFAAB4C0),
    outline = Color(0xFF3A4553),
    error = Color(0xFFFF6B6B),
)

private val Light = lightColorScheme(
    primary = Color(0xFF0069B4),
    onPrimary = Color.White,
    primaryContainer = Color(0xFFD3E8FF),
    onPrimaryContainer = Color(0xFF001D33),
    secondary = Color(0xFF2E7D4F),
    background = Color(0xFFF4F6F9),
    onBackground = Color(0xFF1A1D22),
    surface = Color.White,
    onSurface = Color(0xFF1A1D22),
    surfaceVariant = Color(0xFFE8EDF3),
    onSurfaceVariant = Color(0xFF4A5563),
    outline = Color(0xFFBFC8D3),
)

@Composable
fun LeafTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = if (isSystemInDarkTheme()) Dark else Light,
        typography = Typography(),
        content = content,
    )
}
