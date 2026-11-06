package com.leafcontrol.app

import android.content.Intent
import android.content.res.Configuration
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
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
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.leafcontrol.app.data.Settings
import com.leafcontrol.app.net.LocalApi
import com.leafcontrol.app.ui.LeafTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject

/**
 * Native board settings over GET/POST /api/config. Replaces the detour into
 * the board's web page for the everyday fields; OVMS/modem and other exotic
 * options stay on the web page, reachable from the row at the bottom.
 */
class BoardSettingsActivity : ComponentActivity() {
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
                ) { BoardSettingsScreen() }
            }
        }
    }
}

private class BoardConfigDraft(c: JSONObject) {
    var wifiSsid by mutableStateOf(c.optString("wifi_ssid"))
    var wifiPassword by mutableStateOf("")
    var wifiSsid2 by mutableStateOf(c.optString("wifi_ssid2"))
    var wifiPassword2 by mutableStateOf("")
    var wifiSsid3 by mutableStateOf(c.optString("wifi_ssid3"))
    var wifiPassword3 by mutableStateOf("")
    var hostName by mutableStateOf(c.optString("host_name", "leaf"))
    var apSsid by mutableStateOf(c.optString("ap_ssid"))
    var apPassword by mutableStateOf("")
    var mqttServer by mutableStateOf(c.optString("mqtt_server"))
    var mqttPort by mutableStateOf(c.optInt("mqtt_port", 8883).toString())
    var mqttUser by mutableStateOf(c.optString("mqtt_user"))
    var mqttPassword by mutableStateOf("")
    var mqttVid by mutableStateOf(c.optString("mqtt_vehicle_id"))
    var mqttTls by mutableStateOf(c.optBoolean("mqtt_tls"))
    var hass by mutableStateOf(c.optBoolean("hass_discovery"))
    var ovmsServer by mutableStateOf(c.optString("ovms_server"))
    var ovmsPort by mutableStateOf(c.optInt("ovms_port", 6870).toString())
    var ovmsTls by mutableStateOf(c.optBoolean("ovms_tls"))
    var ovmsPassword by mutableStateOf("")
    var ovmsModulePassword by mutableStateOf("")
    var modemEnabled by mutableStateOf(c.optBoolean("modem_enable"))
    var modemApn by mutableStateOf(c.optString("modem800_apn"))
    var modemUser by mutableStateOf(c.optString("modem800_user"))
    var modemPassword by mutableStateOf("")
    var remoteMode by mutableStateOf(c.optInt("remote_mode", 1))
    var locking by mutableStateOf(c.optBoolean("locking_enabled"))
    var sleepEnabled by mutableStateOf(c.optBoolean("sleep_enable"))
    var sleepAfterH by mutableStateOf(c.optInt("sleep_after_h", 24).toString())
    var wakeFast by mutableStateOf(c.optInt("wake_fast_min", 5).toString())
    var wakeSlow by mutableStateOf(c.optInt("wake_slow_min", 15).toString())
    var tariffDay by mutableStateOf(trimNum(c.optDouble("tariff_day", 0.0)))
    var tariffNight by mutableStateOf(trimNum(c.optDouble("tariff_night", 0.0)))
    var nightFrom by mutableStateOf(c.optInt("night_from", 0).toString())
    var nightTo by mutableStateOf(c.optInt("night_to", 7).toString())
    var chargeLimit by mutableStateOf(c.optInt("charge_limit_pct", 0))
    val wifiPassSet = c.optBoolean("wifi_password_set")
    val apPassSet = c.optBoolean("ap_password_set")
    val mqttPassSet = c.optBoolean("mqtt_password_set")
    val fw = c.optString("fw")
    val board = c.optString("board")

    fun toJson(): JSONObject {
        val o = JSONObject()
        o.put("wifi_ssid", wifiSsid.trim())
        if (wifiPassword.isNotEmpty()) o.put("wifi_password", wifiPassword)
        o.put("wifi_ssid2", wifiSsid2.trim())
        if (wifiPassword2.isNotEmpty()) o.put("wifi_password2", wifiPassword2)
        o.put("wifi_ssid3", wifiSsid3.trim())
        if (wifiPassword3.isNotEmpty()) o.put("wifi_password3", wifiPassword3)
        o.put("host_name", hostName.trim().ifEmpty { "leaf" })
        o.put("ap_ssid", apSsid.trim())
        if (apPassword.isNotEmpty()) o.put("ap_password", apPassword)
        o.put("mqtt_server", mqttServer.trim())
        o.put("mqtt_port", mqttPort.toIntOrNull() ?: 8883)
        o.put("mqtt_user", mqttUser.trim())
        if (mqttPassword.isNotEmpty()) o.put("mqtt_password", mqttPassword)
        o.put("mqtt_vehicle_id", mqttVid.trim())
        o.put("mqtt_tls", mqttTls)
        o.put("hass_discovery", hass)
        o.put("ovms_server", ovmsServer.trim())
        o.put("ovms_port", ovmsPort.toIntOrNull() ?: 6870)
        o.put("ovms_tls", ovmsTls)
        if (ovmsPassword.isNotEmpty()) o.put("ovms_password", ovmsPassword)
        if (ovmsModulePassword.isNotEmpty()) o.put("ovms_module_password", ovmsModulePassword)
        o.put("modem_enable", modemEnabled)
        o.put("modem800_apn", modemApn.trim())
        o.put("modem800_user", modemUser.trim())
        if (modemPassword.isNotEmpty()) o.put("modem800_pass", modemPassword)
        o.put("remote_mode", remoteMode)
        o.put("locking_enabled", locking)
        o.put("sleep_enable", sleepEnabled)
        o.put("sleep_after_h", sleepAfterH.toIntOrNull() ?: 24)
        o.put("wake_fast_min", wakeFast.toIntOrNull() ?: 5)
        o.put("wake_slow_min", wakeSlow.toIntOrNull() ?: 15)
        o.put("tariff_day", tariffDay.replace(',', '.').toDoubleOrNull() ?: 0.0)
        o.put("tariff_night", tariffNight.replace(',', '.').toDoubleOrNull() ?: 0.0)
        o.put("night_from", nightFrom.toIntOrNull() ?: 0)
        o.put("night_to", nightTo.toIntOrNull() ?: 7)
        o.put("charge_limit_pct", chargeLimit)
        return o
    }

    private companion object {
        fun trimNum(d: Double): String =
            if (d == d.toLong().toDouble()) d.toLong().toString() else d.toString()
    }
}

@Composable
private fun BoardSettingsScreen() {
    val ctx = LocalContext.current
    var config by remember { mutableStateOf<BoardConfigDraft?>(null) }
    var loadError by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf("") }
    var reload by remember { mutableIntStateOf(0) }
    val scope = rememberCoroutineScope()

    LaunchedEffect(reload) {
        busy = true
        loadError = null
        var r = LocalApi.getConfigResult(ctx)
        if (r.json == null && r.httpCode == null) {
            // Network error: the saved host may be stale (e.g. 192.168.4.1 from
            // the board's own AP, or an mDNS name this network can't resolve).
            // Find the board on this Wi-Fi and retry once — same self-healing
            // the command path already has.
            val board = withContext(Dispatchers.IO) {
                com.leafcontrol.app.BoardDiscovery.scanSubnet(ctx, 350)
            }
            if (board != null) {
                Settings.localHost = board.host
                Settings.localPort = board.port
                r = LocalApi.getConfigResult(ctx)
            }
        }
        busy = false
        when {
            r.json != null -> config = BoardConfigDraft(r.json)
            r.httpCode == 404 -> loadError =
                "Плата отвечает, но на ней нет API настроек: нужна прошивка " +
                    "FW-2026.11.08.122 или новее. Обновите её: Ещё → Плата → " +
                    "«Обновление прошивки платы»."
            r.httpCode != null -> loadError =
                "Плата ответила ошибкой HTTP ${r.httpCode}. Откройте web-интерфейс " +
                    "и проверьте страницу платы."
            else -> loadError =
                "Плата не отвечает по адресу ${Settings.localHost}:${Settings.localPort}" +
                    (r.errorKind?.let { " ($it)" } ?: "") +
                    ". Убедитесь, что телефон в одной сети с платой (или подключён к её " +
                    "точке LeafControl-Setup), и повторите."
        }
    }

    Column(
        Modifier
            .fillMaxSize()
            .safeDrawingPadding()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("Настройки платы", fontSize = 22.sp, color = MaterialTheme.colorScheme.onBackground)
        when {
            busy && config == null && loadError == null ->
                Text("Чтение конфигурации платы…", color = MaterialTheme.colorScheme.onSurfaceVariant)
            loadError != null && config == null -> {
                Text(
                    loadError ?: "",
                    color = MaterialTheme.colorScheme.error,
                )
                OutlinedButton(onClick = { reload++ }) { Text("Повторить") }
                OpenWebRow()
            }
            config != null -> {
                val c = config!!
                Text(
                    "${c.board} · ${c.fw}",
                    fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )

                SectionCard("Wi-Fi (основная сеть)") {
                    LabeledField("Сеть (SSID)", c.wifiSsid) { c.wifiSsid = it }
                    PasswordField(
                        "Пароль" + if (c.wifiPassSet) " (задан — пусто = не менять)" else "",
                        c.wifiPassword,
                    ) { c.wifiPassword = it }
                    LabeledField("Сеть 2 (пусто = не используется)", c.wifiSsid2) { c.wifiSsid2 = it }
                    PasswordField("Пароль сети 2 (пусто = не менять)", c.wifiPassword2) { c.wifiPassword2 = it }
                    LabeledField("Сеть 3 (пусто = не используется)", c.wifiSsid3) { c.wifiSsid3 = it }
                    PasswordField("Пароль сети 3 (пусто = не менять)", c.wifiPassword3) { c.wifiPassword3 = it }
                    LabeledField("Имя платы (mDNS, без .local)", c.hostName) { c.hostName = it }
                }

                SectionCard("Точка доступа платы") {
                    LabeledField("SSID (пусто = точка выключена)", c.apSsid) { c.apSsid = it }
                    PasswordField(
                        "Пароль (8+ символов" + (if (c.apPassSet) ", задан — пусто = не менять" else "") + ")",
                        c.apPassword,
                    ) { c.apPassword = it }
                }

                SectionCard("MQTT / удалённый доступ") {
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        listOf(0 to "Выключен", 1 to "MQTT", 2 to "OVMS V2").forEach { (id, label) ->
                            FilterChip(
                                selected = c.remoteMode == id,
                                onClick = { c.remoteMode = id },
                                label = { Text(label) },
                            )
                        }
                    }
                    LabeledField("Сервер", c.mqttServer) { c.mqttServer = it }
                    LabeledField("Порт", c.mqttPort, KeyboardType.Number) { c.mqttPort = it }
                    LabeledField("Пользователь", c.mqttUser) { c.mqttUser = it }
                    PasswordField(
                        "Пароль" + if (c.mqttPassSet) " (задан — пусто = не менять)" else "",
                        c.mqttPassword,
                    ) { c.mqttPassword = it }
                    LabeledField("Vehicle ID (топики)", c.mqttVid) { c.mqttVid = it }
                    SwitchRow("TLS (wss/8883)", c.mqttTls) { c.mqttTls = it }
                    SwitchRow("Home Assistant discovery", c.hass) { c.hass = it }
                }

                SectionCard("OVMS Server V2") {
                    LabeledField("Сервер", c.ovmsServer) { c.ovmsServer = it }
                    LabeledField("Порт", c.ovmsPort, KeyboardType.Number) { c.ovmsPort = it }
                    SwitchRow("TLS", c.ovmsTls) { c.ovmsTls = it }
                    PasswordField("Пароль (пусто = не менять)", c.ovmsPassword) { c.ovmsPassword = it }
                    PasswordField("Пароль модуля (пусто = не менять)", c.ovmsModulePassword) { c.ovmsModulePassword = it }
                }

                SectionCard("Модем (SIM800)") {
                    SwitchRow("Включить модем", c.modemEnabled) { c.modemEnabled = it }
                    LabeledField("APN", c.modemApn) { c.modemApn = it }
                    LabeledField("Пользователь APN", c.modemUser) { c.modemUser = it }
                    PasswordField("Пароль APN (пусто = не менять)", c.modemPassword) { c.modemPassword = it }
                }

                SectionCard("Зарядка") {
                    // Auto stop via the board's 0x1DB MITM: works even without
                    // the phone nearby. 0 = disabled.
                    Text("Остановить зарядку при заряде, %", fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        listOf(0, 70, 75, 80, 85, 90, 95).forEach { v ->
                            FilterChip(
                                selected = c.chargeLimit == v,
                                onClick = { c.chargeLimit = v },
                                label = { Text(if (v == 0) "Выкл" else "$v") },
                            )
                        }
                    }
                }

                SectionCard("Замки и сон") {
                    SwitchRow("Разрешить lock/unlock", c.locking) { c.locking = it }
                    SwitchRow("Deep sleep при простое", c.sleepEnabled) { c.sleepEnabled = it }
                    LabeledField("Спать через, часов", c.sleepAfterH, KeyboardType.Number) { c.sleepAfterH = it }
                    LabeledField("Пробуждение частое, мин", c.wakeFast, KeyboardType.Number) { c.wakeFast = it }
                    LabeledField("Пробуждение редкое, мин", c.wakeSlow, KeyboardType.Number) { c.wakeSlow = it }
                }

                SectionCard("Тариф") {
                    LabeledField("День, ₽/кВт·ч", c.tariffDay, KeyboardType.Decimal) { c.tariffDay = it }
                    LabeledField("Ночь, ₽/кВт·ч", c.tariffNight, KeyboardType.Decimal) { c.tariffNight = it }
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        LabeledField("Ночь с", c.nightFrom, KeyboardType.Number, Modifier.weight(1f)) { c.nightFrom = it }
                        LabeledField("до", c.nightTo, KeyboardType.Number, Modifier.weight(1f)) { c.nightTo = it }
                    }
                }

                if (status.isNotEmpty()) {
                    Text(status, fontSize = 13.sp, color = MaterialTheme.colorScheme.primary)
                }
                Button(
                    enabled = !busy,
                    onClick = {
                        busy = true
                        status = "Сохранение…"
                        val payload = c.toJson()
                        scope.launch {
                            val r = LocalApi.saveConfig(ctx, payload)
                            busy = false
                            status = when {
                                r == null -> "Плата не ответила — настройки не сохранены"
                                r.optBoolean("rebooting") -> "Сохранено. Плата перезагружается…"
                                else -> "Сохранено."
                            }
                            if (r != null && r.optBoolean("rebooting")) {
                                Toast.makeText(ctx, "Плата перезагружается", Toast.LENGTH_SHORT).show()
                            }
                        }
                    },
                    modifier = Modifier.fillMaxWidth(),
                ) { Text(if (busy) "Сохранение…" else "Сохранить и перезагрузить плату") }

                OpenWebRow()
            }
        }
        Spacer(Modifier.height(12.dp))
    }
}

@Composable
private fun OpenWebRow() {
    val ctx = LocalContext.current
    OutlinedButton(
        onClick = {
            LocalApi.ensureLocalHost(ctx)
            val url = "http://${Settings.localHost}:${Settings.localPort}/settings"
            try {
                ctx.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
            } catch (_: Exception) {
                Toast.makeText(ctx, "Не удалось открыть страницу платы", Toast.LENGTH_SHORT).show()
            }
        },
        modifier = Modifier.fillMaxWidth(),
    ) { Text("Открыть web-интерфейс платы") }
}

@Composable
private fun SectionCard(title: String, content: @Composable () -> Unit) {
    Surface(
        shape = RoundedCornerShape(12.dp),
        color = MaterialTheme.colorScheme.surface,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(title, fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            content()
        }
    }
}

@Composable
private fun LabeledField(
    label: String,
    value: String,
    keyboard: KeyboardType = KeyboardType.Text,
    modifier: Modifier = Modifier,
    onChange: (String) -> Unit,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onChange,
        label = { Text(label) },
        keyboardOptions = KeyboardOptions(keyboardType = keyboard),
        singleLine = true,
        modifier = modifier.fillMaxWidth(),
    )
}

@Composable
private fun PasswordField(label: String, value: String, onChange: (String) -> Unit) {
    OutlinedTextField(
        value = value,
        onValueChange = onChange,
        label = { Text(label) },
        visualTransformation = PasswordVisualTransformation(),
        singleLine = true,
        modifier = Modifier.fillMaxWidth(),
    )
}

@Composable
private fun SwitchRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, color = MaterialTheme.colorScheme.onSurface)
        Switch(checked = checked, onCheckedChange = onChange)
    }
}
