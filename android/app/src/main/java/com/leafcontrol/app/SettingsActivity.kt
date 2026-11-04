package com.leafcontrol.app

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings as AndroidSettings
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
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
import androidx.core.content.ContextCompat
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

        var chargeStart by remember { mutableStateOf(Settings.notifyChargeStart) }
        var chargeEnd by remember { mutableStateOf(Settings.notifyChargeEnd) }
        var lowSoc by remember { mutableStateOf(Settings.notifyLowSoc) }
        var offline by remember { mutableStateOf(Settings.notifyOffline) }
        var tripEnd by remember { mutableStateOf(Settings.notifyTripEnd) }
        var lowThreshold by remember { mutableStateOf(Settings.lowSocThreshold.toString()) }

        val notificationPermission =
            rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { ok ->
                Toast.makeText(
                    ctx,
                    if (ok) "Уведомления разрешены" else "Уведомления запрещены",
                    Toast.LENGTH_SHORT
                ).show()
            }

        Column(
            Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Text("Настройки Android", style = MaterialTheme.typography.headlineSmall)
            Text(
                "Внешний вид и все функции — в самой PWA. Здесь адрес платы, фоновая служба и уведомления.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            Text("Плата", style = MaterialTheme.typography.titleMedium)
            OutlinedTextField(
                value = host,
                onValueChange = { host = it },
                label = { Text("IP или leaf.local") },
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
            SwitchRow("Фоновый мониторинг", monitor) {
                monitor = it
                Settings.monitorEnabled = it
                if (it) LeafMonitorService.reload(ctx) else LeafMonitorService.stop(ctx)
            }

            Text("Уведомления", style = MaterialTheme.typography.titleMedium)
            SwitchRow("Зарядка началась", chargeStart) {
                chargeStart = it
                Settings.notifyChargeStart = it
            }
            SwitchRow("Зарядка завершена", chargeEnd) {
                chargeEnd = it
                Settings.notifyChargeEnd = it
            }
            SwitchRow("Низкий заряд", lowSoc) {
                lowSoc = it
                Settings.notifyLowSoc = it
            }
            OutlinedTextField(
                value = lowThreshold,
                onValueChange = { lowThreshold = it.filter(Char::isDigit).take(2) },
                label = { Text("Порог низкого заряда, %") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
            )
            SwitchRow("Плата не в сети", offline) {
                offline = it
                Settings.notifyOffline = it
            }
            SwitchRow("Поездка завершена", tripEnd) {
                tripEnd = it
                Settings.notifyTripEnd = it
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(
                    onClick = {
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
                            ContextCompat.checkSelfPermission(
                                ctx, Manifest.permission.POST_NOTIFICATIONS
                            ) != PackageManager.PERMISSION_GRANTED
                        ) {
                            notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
                        } else {
                            Toast.makeText(ctx, "Уведомления уже разрешены", Toast.LENGTH_SHORT).show()
                        }
                    },
                    modifier = Modifier.weight(1f)
                ) { Text("Разрешить") }

                OutlinedButton(
                    onClick = {
                        Notifications.event(ctx, "Leaf Control", "Тестовое уведомление")
                    },
                    modifier = Modifier.weight(1f)
                ) { Text("Проверить") }
            }
            OutlinedButton(
                onClick = {
                    try {
                        ctx.startActivity(
                            Intent(AndroidSettings.ACTION_APP_NOTIFICATION_SETTINGS)
                                .putExtra(AndroidSettings.EXTRA_APP_PACKAGE, ctx.packageName)
                        )
                    } catch (_: Exception) {
                        Toast.makeText(ctx, "Настройки каналов недоступны", Toast.LENGTH_SHORT).show()
                    }
                },
                modifier = Modifier.fillMaxWidth()
            ) { Text("Каналы уведомлений") }

            Text("Обновление", style = MaterialTheme.typography.titleMedium)
            OutlinedButton(
                onClick = {
                    lowThreshold.toIntOrNull()?.let { Settings.lowSocThreshold = it }
                    ctx.startActivity(
                        Intent(ctx, MainActivity::class.java)
                            .putExtra("check_update", true)
                            .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
                    )
                    finish()
                },
                modifier = Modifier.fillMaxWidth()
            ) { Text("Проверить обновление") }
            Text(
                "Версия ${BuildConfig.VERSION_NAME}. Приложение проверяет последний APK в релизах GitHub.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            Spacer(Modifier.height(4.dp))
            Button(
                onClick = {
                    Settings.localHost = host
                    port.toIntOrNull()?.let { if (it in 1..65535) Settings.localPort = it }
                    Settings.monitorEnabled = monitor
                    lowThreshold.toIntOrNull()?.let { Settings.lowSocThreshold = it }
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

            Text(
                "MQTT настраивается в самой PWA: Ещё → Настройки. Приложение автоматически передаёт эти данные фоновой службе.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }

    @Composable
    private fun SwitchRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(label, modifier = Modifier.weight(1f))
            Switch(checked = checked, onCheckedChange = onChange)
        }
    }
}
