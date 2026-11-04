package com.leafcontrol.app

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings as AndroidSettings
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.leafcontrol.app.data.Settings
import com.leafcontrol.app.service.LeafMonitorService
import com.leafcontrol.app.ui.LeafTheme

class SettingsActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            LeafTheme {
                Surface(Modifier.fillMaxSize()) { NativeSettings() }
            }
        }
    }

    @Composable
    private fun NativeSettings() {
        val ctx = LocalContext.current
        var host by remember { mutableStateOf(Settings.localHost) }
        var port by remember { mutableStateOf(Settings.localPort.toString()) }
        var monitor by remember { mutableStateOf(Settings.monitorEnabled) }

        Column(
            Modifier.fillMaxSize().padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Text("Настройки Android", style = MaterialTheme.typography.headlineSmall)
            Text(
                "Внешний вид и все функции — в самой PWA. Здесь только адрес платы и фоновая служба.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            OutlinedTextField(
                value = host,
                onValueChange = { host = it },
                label = { Text("Плата: IP или leaf.local") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true
            )
            OutlinedTextField(
                value = port,
                onValueChange = { port = it },
                label = { Text("Порт") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
            )
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Фоновый мониторинг", modifier = Modifier.weight(1f))
                Switch(checked = monitor, onCheckedChange = { monitor = it })
            }
            Button(
                onClick = {
                    Settings.localHost = host
                    port.toIntOrNull()?.let { if (it in 1..65535) Settings.localPort = it }
                    Settings.monitorEnabled = monitor
                    if (monitor) LeafMonitorService.reload(ctx) else LeafMonitorService.stop(ctx)
                    finish()
                },
                modifier = Modifier.fillMaxWidth()
            ) { Text("Сохранить") }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(
                    onClick = {
                        Settings.localHost = host
                        port.toIntOrNull()?.let { if (it in 1..65535) Settings.localPort = it }
                        startActivity(
                            Intent(ctx, MainActivity::class.java)
                                .putExtra("open_board", true)
                                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
                        )
                        finish()
                    },
                    modifier = Modifier.weight(1f)
                ) { Text("Открыть плату") }
                OutlinedButton(
                    onClick = {
                        startActivity(
                            Intent(ctx, MainActivity::class.java)
                                .putExtra("open_pwa", true)
                                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
                        )
                        finish()
                    },
                    modifier = Modifier.weight(1f)
                ) { Text("PWA") }
            }

            OutlinedButton(
                onClick = {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                        val i = Intent(AndroidSettings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)
                            .setData(Uri.parse("package:${ctx.packageName}"))
                        try {
                            ctx.startActivity(i)
                        } catch (_: Exception) {
                            ctx.startActivity(
                                Intent(AndroidSettings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)
                            )
                        }
                    }
                },
                modifier = Modifier.fillMaxWidth()
            ) { Text("Разрешить работу в фоне") }

            Spacer(Modifier.height(8.dp))
            Text(
                "MQTT настраивается в самой PWA: Ещё → Настройки. Приложение автоматически передаёт эти данные фоновой службе.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}
