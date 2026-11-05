package com.leafcontrol.app

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings as AndroidSettings
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.FilterChip
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
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.leafcontrol.app.data.Settings
import com.leafcontrol.app.service.LeafMonitorService
import com.leafcontrol.app.ui.LeafTheme

class SettingsActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val night = (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
            Configuration.UI_MODE_NIGHT_YES
        WindowInsetsControllerCompat(window, window.decorView).isAppearanceLightStatusBars = !night
        window.statusBarColor = Color.TRANSPARENT
        window.navigationBarColor = Color.TRANSPARENT
        setContent {
            LeafTheme {
                Surface(
                    Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) { NativeSettings() }
            }
        }
    }

    @Composable
    private fun NativeSettings() {
        val ctx = LocalContext.current
        var monitor by remember { mutableStateOf(Settings.monitorEnabled) }
        var autoUpdate by remember { mutableStateOf(Settings.autoUpdate) }

        var mode by remember { mutableStateOf(Settings.mode) }
        var mqttHost by remember { mutableStateOf(Settings.mqttHost) }
        var mqttPort by remember { mutableStateOf(Settings.mqttPort.toString()) }
        var mqttUser by remember { mutableStateOf(Settings.mqttUser) }
        var mqttPass by remember { mutableStateOf(Settings.mqttPass) }
        var vehicleId by remember { mutableStateOf(Settings.vehicleId) }
        var tariffDay by remember { mutableStateOf(Settings.tariffDay.toString()) }
        var tariffNight by remember { mutableStateOf(Settings.tariffNight.toString()) }
        var nightFrom by remember { mutableStateOf(Settings.nightFrom.toString()) }
        var nightTo by remember { mutableStateOf(Settings.nightTo.toString()) }

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
                .safeDrawingPadding()
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            Text("Настройки", style = MaterialTheme.typography.headlineSmall)

            Group("Связь") {
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    listOf("auto" to "Авто", "local" to "Локально", "mqtt" to "MQTT").forEach { (value, label) ->
                        FilterChip(
                            selected = mode == value,
                            onClick = {
                                mode = value
                                Settings.mode = value
                                // Choosing a transport means the user wants data;
                                // start the monitor even if it was switched off.
                                Settings.monitorEnabled = true
                                monitor = true
                                com.leafcontrol.app.net.LocalApi.ensureLocalHost(ctx)
                                LeafMonitorService.reload(ctx)
                                Toast.makeText(ctx, "Режим: $label", Toast.LENGTH_SHORT).show()
                            },
                            label = { Text(label, fontSize = 12.sp) },
                        )
                    }
                }
                OutlinedTextField(
                    value = mqttHost,
                    onValueChange = { mqttHost = it },
                    label = { Text("MQTT-сервер") },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                )
                OutlinedTextField(
                    value = mqttPort,
                    onValueChange = { mqttPort = it.filter(Char::isDigit).take(5) },
                    label = { Text("Порт") },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
                OutlinedTextField(
                    value = mqttUser,
                    onValueChange = { mqttUser = it },
                    label = { Text("Пользователь") },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                )
                OutlinedTextField(
                    value = mqttPass,
                    onValueChange = { mqttPass = it },
                    label = { Text("Пароль") },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                    visualTransformation = PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                )
                OutlinedTextField(
                    value = vehicleId,
                    onValueChange = { vehicleId = it },
                    label = { Text("Vehicle ID") },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                )
                OutlinedButton(
                    onClick = {
                        Settings.mode = mode
                        Settings.mqttHost = mqttHost
                        mqttPort.toIntOrNull()?.let { if (it in 1..65535) Settings.mqttPort = it }
                        Settings.mqttUser = mqttUser
                        Settings.mqttPass = mqttPass
                        Settings.vehicleId = vehicleId
                        if (Settings.monitorEnabled) LeafMonitorService.reload(ctx)
                        Toast.makeText(ctx, "Связь сохранена", Toast.LENGTH_SHORT).show()
                    },
                    modifier = Modifier.fillMaxWidth()
                ) { Text("Сохранить связь") }
            }

            Group("Уведомления") {
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
                    onValueChange = {
                        lowThreshold = it.filter(Char::isDigit).take(2)
                        lowThreshold.toIntOrNull()?.let { v -> Settings.lowSocThreshold = v }
                    },
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
                            Toast.makeText(ctx, "Проверьте шторку", Toast.LENGTH_SHORT).show()
                        },
                        modifier = Modifier.weight(1f)
                    ) { Text("Проверить") }
                }
                ActionRow("Каналы уведомлений") {
                    try {
                        ctx.startActivity(
                            Intent(AndroidSettings.ACTION_APP_NOTIFICATION_SETTINGS)
                                .putExtra(AndroidSettings.EXTRA_APP_PACKAGE, ctx.packageName)
                        )
                    } catch (_: Exception) {
                        Toast.makeText(ctx, "Настройки каналов недоступны", Toast.LENGTH_SHORT).show()
                    }
                }
            }

            Group("Приложение") {
                SwitchRow("Фоновый мониторинг", monitor) {
                    monitor = it
                    Settings.monitorEnabled = it
                    if (it) LeafMonitorService.reload(ctx) else LeafMonitorService.stop(ctx)
                }
                ActionRow("Открыть плату") {
                    ctx.startActivity(
                        Intent(ctx, MainActivity::class.java)
                            .putExtra("open_board", true)
                            .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
                    )
                    finish()
                }
                ActionRow("Найти плату") {
                    Toast.makeText(ctx, "Ищем плату в этой сети…", Toast.LENGTH_SHORT).show()
                    Thread {
                        val board = BoardDiscovery.scanSubnet(ctx)
                        runOnUiThread {
                            if (isFinishing || isDestroyed) return@runOnUiThread
                            if (board != null) {
                                Settings.localHost = board.host
                                Settings.localPort = board.port
                                Toast.makeText(
                                    ctx,
                                    "Плата: ${board.host}",
                                    Toast.LENGTH_LONG
                                ).show()
                            } else {
                                Toast.makeText(
                                    ctx,
                                    "Плата не найдена. Подключитесь к её Wi-Fi или домашней сети.",
                                    Toast.LENGTH_LONG
                                ).show()
                            }
                        }
                    }.start()
                }
                ActionRow("Разрешить работу в фоне") {
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
                }
            }

            Group("Тариф") {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = tariffDay,
                        onValueChange = { tariffDay = it.filter { c -> c.isDigit() || c == '.' || c == ',' } },
                        label = { Text("День, ₽/кВт·ч") },
                        modifier = Modifier.weight(1f),
                        singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                    )
                    OutlinedTextField(
                        value = tariffNight,
                        onValueChange = { tariffNight = it.filter { c -> c.isDigit() || c == '.' || c == ',' } },
                        label = { Text("Ночь, ₽/кВт·ч") },
                        modifier = Modifier.weight(1f),
                        singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                    )
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = nightFrom,
                        onValueChange = { nightFrom = it.filter(Char::isDigit).take(2) },
                        label = { Text("Ночь с") },
                        modifier = Modifier.weight(1f),
                        singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    )
                    OutlinedTextField(
                        value = nightTo,
                        onValueChange = { nightTo = it.filter(Char::isDigit).take(2) },
                        label = { Text("Ночь до") },
                        modifier = Modifier.weight(1f),
                        singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    )
                }
                OutlinedButton(
                    onClick = {
                        val day = tariffDay.replace(',', '.').toDoubleOrNull() ?: Settings.tariffDay
                        val night = tariffNight.replace(',', '.').toDoubleOrNull() ?: Settings.tariffNight
                        val from = nightFrom.toIntOrNull() ?: Settings.nightFrom
                        val to = nightTo.toIntOrNull() ?: Settings.nightTo
                        Settings.tariffDay = day
                        Settings.tariffNight = night
                        Settings.nightFrom = from
                        Settings.nightTo = to
                        // The board's own tariff splits history energy day/night.
                        Thread {
                            kotlinx.coroutines.runBlocking {
                                com.leafcontrol.app.net.LocalApi.setTariff(ctx, day, night, from, to)
                            }
                        }.start()
                        Toast.makeText(ctx, "Тариф сохранён", Toast.LENGTH_SHORT).show()
                    },
                    modifier = Modifier.fillMaxWidth()
                ) { Text("Сохранить тариф") }
            }

            // Update stays outside the grouped cards, as requested.
            Surface(
                shape = RoundedCornerShape(14.dp),
                color = MaterialTheme.colorScheme.surface,
                tonalElevation = 1.dp,
                modifier = Modifier.fillMaxWidth()
            ) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    SwitchRow("Проверять обновления автоматически", autoUpdate) {
                        autoUpdate = it
                        Settings.autoUpdate = it
                    }
                    OutlinedButton(
                        onClick = {
                            ctx.startActivity(
                                Intent(ctx, MainActivity::class.java)
                                    .putExtra("check_update", true)
                                    .addFlags(
                                        Intent.FLAG_ACTIVITY_CLEAR_TOP or
                                            Intent.FLAG_ACTIVITY_SINGLE_TOP
                                    )
                            )
                            finish()
                        },
                        modifier = Modifier.fillMaxWidth()
                    ) { Text("Проверить обновление") }
                    Text(
                        "Версия ${BuildConfig.VERSION_NAME}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }

            Spacer(Modifier.height(8.dp))
        }
    }

    @Composable
    private fun Group(title: String, content: @Composable ColumnScope.() -> Unit) {
        Column(
            Modifier.fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            Text(
                title,
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 4.dp)
            )
            Surface(
                shape = RoundedCornerShape(14.dp),
                color = MaterialTheme.colorScheme.surface,
                tonalElevation = 1.dp,
                modifier = Modifier.fillMaxWidth()
            ) {
                Column(
                    Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
                    content = content
                )
            }
        }
    }

    @Composable
    private fun SwitchRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
        Row(
            Modifier
                .fillMaxWidth()
                .padding(vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(label, modifier = Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
            Switch(checked = checked, onCheckedChange = onChange)
        }
    }

    @Composable
    private fun ActionRow(label: String, onClick: () -> Unit) {
        Row(
            Modifier
                .fillMaxWidth()
                .height(48.dp)
                .clickable { onClick() },
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(label, modifier = Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
            Text(
                "›",
                style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}
