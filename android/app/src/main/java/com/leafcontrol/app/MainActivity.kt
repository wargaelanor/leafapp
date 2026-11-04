package com.leafcontrol.app

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import com.leafcontrol.app.data.Settings
import com.leafcontrol.app.service.LeafMonitorService
import com.leafcontrol.app.ui.LeafTheme
import com.leafcontrol.app.ui.MainScreen

class MainActivity : ComponentActivity() {
    private val notificationPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
        if (Settings.monitorEnabled) {
            try {
                LeafMonitorService.start(this)
            } catch (_: Exception) {
            }
        }
        setContent {
            LeafTheme {
                MainScreen()
            }
        }
    }
}
