package com.leafcontrol.app.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val Dark = darkColorScheme(
    primary = Color(0xFF4AA8D8),
    onPrimary = Color(0xFF08121A),
    primaryContainer = Color(0xFF17324A),
    onPrimaryContainer = Color(0xFFD6E8FF),
    secondary = Color(0xFF8FD3A8),
    background = Color(0xFF0F1216),
    onBackground = Color(0xFFE6EDF3),
    surface = Color(0xFF181C21),
    onSurface = Color(0xFFE6EDF3),
    surfaceVariant = Color(0xFF232931),
    onSurfaceVariant = Color(0xFF96A0AA),
    outline = Color(0xFF2F3640),
    error = Color(0xFFFF6B6B),
)

private val Light = lightColorScheme(
    primary = Color(0xFF0077AC),
    onPrimary = Color.White,
    primaryContainer = Color(0xFFD3E8FF),
    onPrimaryContainer = Color(0xFF001D33),
    secondary = Color(0xFF2E7D4F),
    background = Color(0xFFF4F5F7),
    onBackground = Color(0xFF333333),
    surface = Color.White,
    onSurface = Color(0xFF333333),
    surfaceVariant = Color(0xFFECEEF1),
    onSurfaceVariant = Color(0xFF707070),
    outline = Color(0xFFF1F1F1),
)

@Composable
fun LeafTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = if (isSystemInDarkTheme()) Dark else Light,
        typography = Typography(),
        content = content,
    )
}
