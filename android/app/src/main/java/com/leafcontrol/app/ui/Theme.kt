package com.leafcontrol.app.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val Dark = darkColorScheme(
    primary = Color(0xFF59B6DF), onPrimary = Color(0xFF08121A),
    primaryContainer = Color(0xFF17324A), onPrimaryContainer = Color(0xFFD6E8FF),
    secondary = Color(0xFF8FD3A8), background = Color(0xFF0E1116),
    onBackground = Color(0xFFE9EDF3), surface = Color(0xFF171C23),
    onSurface = Color(0xFFE9EDF3), surfaceVariant = Color(0xFF1D232C),
    onSurfaceVariant = Color(0xFFB9C2CF), outline = Color(0xFF2B333F),
    error = Color(0xFFFF6B6B),
)

private val Light = lightColorScheme(
    primary = Color(0xFF0077AC), onPrimary = Color.White,
    primaryContainer = Color(0xFFE3F2F9), onPrimaryContainer = Color(0xFF00324C),
    secondary = Color(0xFF1F9D55), background = Color(0xFFF2F4F7),
    onBackground = Color(0xFF151A24), surface = Color.White,
    onSurface = Color(0xFF151A24), surfaceVariant = Color(0xFFF7F8FA),
    onSurfaceVariant = Color(0xFF4B5563), outline = Color(0xFFE6EAF0),
)

@Composable
fun LeafTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = if (isSystemInDarkTheme()) Dark else Light,
        typography = Typography(),
        content = content,
    )
}
