package com.leafcontrol.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.BatteryChargingFull
import androidx.compose.material.icons.filled.DirectionsCar
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Route
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import com.leafcontrol.app.data.Charge
import com.leafcontrol.app.data.LeafRepo
import com.leafcontrol.app.data.LeafState
import com.leafcontrol.app.data.Settings
import com.leafcontrol.app.data.Trip
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

private val dateFmt = SimpleDateFormat("dd MMM, HH:mm", Locale("ru"))

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen(vm: LeafViewModel = viewModel()) {
    val state by vm.state.collectAsStateWithLifecycle()
    val trips by vm.trips.collectAsStateWithLifecycle()
    val charges by vm.charges.collectAsStateWithLifecycle()
    val monitorText by vm.monitorText.collectAsStateWithLifecycle()
    val draft by vm.draft.collectAsStateWithLifecycle()
    val snack = remember { SnackbarHostState() }
    var tab by rememberSaveable { mutableIntStateOf(0) }

    LaunchedEffect(Unit) {
        vm.message.collect { msg ->
            if (msg != null) {
                snack.showSnackbar(msg)
                vm.message.value = null
            }
        }
    }

    val titles = listOf("Состояние", "Поездки", "Зарядки", "Ещё")
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(titles[tab]) },
                actions = {
                    IconButton(onClick = { vm.refresh() }) {
                        Icon(Icons.Default.Refresh, contentDescription = "Обновить")
                    }
                },
            )
        },
        bottomBar = {
            NavigationBar {
                NavigationBarItem(
                    selected = tab == 0, onClick = { tab = 0 },
                    icon = { Icon(Icons.Default.DirectionsCar, null) }, label = { Text("Авто") })
                NavigationBarItem(
                    selected = tab == 1, onClick = { tab = 1 },
                    icon = { Icon(Icons.Default.Route, null) }, label = { Text("Поездки") })
                NavigationBarItem(
                    selected = tab == 2, onClick = { tab = 2 },
                    icon = { Icon(Icons.Default.BatteryChargingFull, null) }, label = { Text("Зарядки") })
                NavigationBarItem(
                    selected = tab == 3, onClick = { tab = 3 },
                    icon = { Icon(Icons.Default.Settings, null) }, label = { Text("Ещё") })
            }
        },
        snackbarHost = { SnackbarHost(snack) },
    ) { pad ->
        Box(Modifier.fillMaxSize().padding(pad)) {
            when (tab) {
                0 -> StatusScreen(state, monitorText, vm::command)
                1 -> TripsScreen(trips, onClear = { vm.clearHistory(false) })
                2 -> ChargesScreen(charges, onClear = { vm.clearHistory(true) })
                else -> SettingsScreen(
                    draft = draft,
                    onChange = { vm.draft.value = it },
                    onSave = { vm.saveSettings() },
                    onStart = { vm.startMonitor() },
                    onStop = { vm.stopMonitor() },
                    onBattery = { vm.openBatterySettings() },
                )
            }
        }
    }
}

@Composable
private fun StatusScreen(state: LeafState, monitorText: String, onCommand: (String) -> Unit) {
    val fresh = state.fresh()
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Surface(
                color = when {
                    !state.connected -> MaterialTheme.colorScheme.errorContainer
                    state.transport == "mqtt" -> MaterialTheme.colorScheme.primaryContainer
                    else -> MaterialTheme.colorScheme.secondaryContainer
                },
                shape = RoundedCornerShape(20.dp)
            ) {
                Text(
                    when {
                        !state.connected -> "Нет связи"
                        state.transport == "mqtt" -> "MQTT"
                        else -> "Локально"
                    },
                    modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                    fontSize = 12.sp,
                )
            }
            Spacer(Modifier.width(10.dp))
            Text(
                if (state.lastUpdateMs > 0)
                    "обновлено ${LeafRepo.fmtDuration(System.currentTimeMillis() - state.lastUpdateMs)} назад"
                else monitorText,
                fontSize = 12.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.primaryContainer)) {
            Row(Modifier.fillMaxWidth().padding(20.dp), verticalAlignment = Alignment.Bottom) {
                Text(
                    if (state.soc >= 0) "${state.soc.toInt()}" else "—",
                    fontSize = 56.sp, fontWeight = FontWeight.Bold
                )
                Text("%", fontSize = 22.sp, modifier = Modifier.padding(bottom = 8.dp, start = 2.dp))
                Spacer(Modifier.weight(1f))
                Column(horizontalAlignment = Alignment.End) {
                    Text(
                        if (state.range > 0) "${state.range} км" else "—",
                        fontSize = 22.sp, fontWeight = FontWeight.SemiBold
                    )
                    Text("запас хода", fontSize = 12.sp)
                    Text(
                        when {
                            state.charging -> "заряжается"
                            state.hvac -> "климат"
                            state.on == 1 -> "включён"
                            else -> "покой"
                        },
                        fontSize = 13.sp,
                    )
                }
            }
            if (state.soc >= 0) {
                LinearProgressIndicator(
                    progress = { (state.soc / 100.0).toFloat().coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxWidth().height(6.dp),
                )
            }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Metric("Мощность", if (fresh) "${"%.1f".format(state.power)} кВт" else "—", Modifier.weight(1f))
            Metric("Напряжение", if (fresh && state.voltage > 0) "${state.voltage.toInt()} В" else "—", Modifier.weight(1f))
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Metric("Ток", if (fresh) "${"%.1f".format(state.current)} А" else "—", Modifier.weight(1f))
            Metric("Батарея", if (fresh && state.batteryTemp > -50) "${"%.1f".format(state.batteryTemp)} °C" else "—", Modifier.weight(1f))
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Metric("Салон", if (fresh && state.cabin > -50) "${"%.1f".format(state.cabin)} °C" else "—", Modifier.weight(1f))
            Metric("Пробег", if (state.odometer > 0) "${state.odometer} км" else "—", Modifier.weight(1f))
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Metric("SOH", if (state.soh > 0) "${state.soh}%" else "—", Modifier.weight(1f))
            Metric("Замки", when (state.locked) { 0 -> "открыто"; 1 -> "закрыто"; else -> "—" }, Modifier.weight(1f))
        }

        Text("Команды", fontWeight = FontWeight.SemiBold, modifier = Modifier.padding(top = 4.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = { onCommand("hvac") }, modifier = Modifier.weight(1f)) {
                Text(if (state.hvac) "Выкл климат" else "Климат")
            }
            OutlinedButton(onClick = { onCommand(if (state.charging) "chargeoff" else "charge") }, modifier = Modifier.weight(1f)) {
                Text(if (state.charging) "Стоп заряд" else "Зарядить")
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = { onCommand(if (state.locked == 0) "lock" else "unlock") }, modifier = Modifier.weight(1f)) {
                Text(if (state.locked == 0) "Закрыть" else "Открыть")
            }
            OutlinedButton(onClick = { onCommand("refresh") }, modifier = Modifier.weight(1f)) {
                Text("Обновить")
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = { onCommand("lights") }, modifier = Modifier.weight(1f)) { Text("Свет") }
            OutlinedButton(onClick = { onCommand("hornlights") }, modifier = Modifier.weight(1f)) { Text("Сигнал") }
        }
        Text(
            "Команды выполняются на плате. Если машина спит, ответ может занять 10–20 секунд.",
            fontSize = 11.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun Metric(label: String, value: String, modifier: Modifier = Modifier) {
    Card(modifier = modifier) {
        Column(Modifier.padding(12.dp)) {
            Text(value, fontSize = 18.sp, fontWeight = FontWeight.SemiBold)
            Text(label, fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun TripsScreen(trips: List<Trip>, onClear: () -> Unit) {
    val done = trips.sortedByDescending { it.startMs }
    Column(Modifier.fillMaxSize().padding(16.dp)) {
        SummaryRow(
            listOf(
                done.size.toString() to "поездок",
                "%.1f".format(done.sumOf { it.distKm }) to "км",
                "%.2f".format(done.sumOf { it.energyWh } / 1000.0) to "кВт·ч",
                money(done.sumOf { LeafRepo.cost(it) }) to "₽",
            )
        )
        Spacer(Modifier.height(10.dp))
        LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            items(done) { t -> TripCard(t) }
            if (done.isEmpty()) item { Empty("Поездок пока нет") }
        }
        if (done.isNotEmpty()) {
            TextButton(onClick = onClear, modifier = Modifier.align(Alignment.End)) { Text("Очистить историю") }
        }
    }
}

@Composable
private fun ChargesScreen(charges: List<Charge>, onClear: () -> Unit) {
    val done = charges.sortedByDescending { it.startMs }
    val hours = done.sumOf { (it.endMs - it.startMs) / 3_600_000.0 }
    Column(Modifier.fillMaxSize().padding(16.dp)) {
        SummaryRow(
            listOf(
                done.size.toString() to "сеансов",
                "%.2f".format(done.sumOf { it.energyWh } / 1000.0) to "кВт·ч",
                "%.1f".format(hours) to "часов",
                money(done.sumOf { LeafRepo.cost(it) }) to "₽",
            )
        )
        Spacer(Modifier.height(10.dp))
        LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            items(done) { c -> ChargeCard(c) }
            if (done.isEmpty()) item { Empty("Зарядок пока нет") }
        }
        if (done.isNotEmpty()) {
            TextButton(onClick = onClear, modifier = Modifier.align(Alignment.End)) { Text("Очистить историю") }
        }
    }
}

@Composable
private fun SummaryRow(cells: List<Pair<String, String>>) {
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        cells.forEach { (value, label) ->
            Card(modifier = Modifier.weight(1f)) {
                Column(Modifier.fillMaxWidth().padding(vertical = 10.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(value, fontSize = 17.sp, fontWeight = FontWeight.SemiBold)
                    Text(label, fontSize = 10.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
    }
}

@Composable
private fun TripCard(t: Trip) {
    val per100 = LeafRepo.tripPer100(t)
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(Modifier.fillMaxWidth()) {
                Text(dateFmt.format(Date(t.startMs)), fontWeight = FontWeight.SemiBold)
                Spacer(Modifier.weight(1f))
                Text(LeafRepo.fmtDuration(t.endMs - t.startMs), fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                Value("${"%.1f".format(t.distKm)} км")
                Value("${"%.2f".format(t.energyWh / 1000.0)} кВт·ч")
                if (per100 != null) Value("${"%.1f".format(per100)} кВт·ч/100")
                Value(money(LeafRepo.cost(t)))
            }
            if (t.startSoc != null && t.endSoc != null) {
                Text("Заряд ${t.startSoc}→${t.endSoc}%", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

@Composable
private fun ChargeCard(c: Charge) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(Modifier.fillMaxWidth()) {
                Text(dateFmt.format(Date(c.startMs)), fontWeight = FontWeight.SemiBold)
                Spacer(Modifier.weight(1f))
                Text(LeafRepo.fmtDuration(c.endMs - c.startMs), fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                Value("${"%.2f".format(c.energyWh / 1000.0)} кВт·ч")
                Value(money(LeafRepo.cost(c)))
            }
            if (c.startSoc != null && c.endSoc != null) {
                Text("Заряд ${c.startSoc}→${c.endSoc}%", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

@Composable
private fun Value(text: String) {
    Text(text, fontSize = 13.sp, fontWeight = FontWeight.Medium)
}

@Composable
private fun Empty(text: String) {
    Box(Modifier.fillMaxWidth().padding(40.dp), contentAlignment = Alignment.Center) {
        Text(text, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun SettingsScreen(
    draft: SettingsDraft,
    onChange: (SettingsDraft) -> Unit,
    onSave: () -> Unit,
    onStart: () -> Unit,
    onStop: () -> Unit,
    onBattery: () -> Unit,
) {
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        Text("Подключение", style = MaterialTheme.typography.titleMedium)
        ModeRow("Авто", "auto", draft, onChange)
        ModeRow("Только локально", "local", draft, onChange)
        ModeRow("MQTT (удалённо)", "mqtt", draft, onChange)
        OutlinedTextField(
            value = draft.localHost,
            onValueChange = { onChange(draft.copy(localHost = it)) },
            label = { Text("Плата: IP или leaf.local") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
        )
        OutlinedTextField(
            value = draft.localPort,
            onValueChange = { onChange(draft.copy(localPort = it)) },
            label = { Text("Порт платы") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
        )
        HorizontalDivider(Modifier.padding(vertical = 4.dp))
        OutlinedTextField(
            value = draft.mqttHost,
            onValueChange = { onChange(draft.copy(mqttHost = it)) },
            label = { Text("MQTT-сервер") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
        )
        OutlinedTextField(
            value = draft.mqttPort,
            onValueChange = { onChange(draft.copy(mqttPort = it)) },
            label = { Text("MQTT-порт") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
        )
        OutlinedTextField(
            value = draft.mqttUser,
            onValueChange = { onChange(draft.copy(mqttUser = it)) },
            label = { Text("Пользователь MQTT") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
        )
        OutlinedTextField(
            value = draft.mqttPass,
            onValueChange = { onChange(draft.copy(mqttPass = it)) },
            label = { Text("Пароль MQTT") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            visualTransformation = PasswordVisualTransformation(),
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
        )
        OutlinedTextField(
            value = draft.vehicleId,
            onValueChange = { onChange(draft.copy(vehicleId = it)) },
            label = { Text("Vehicle ID") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
        )

        HorizontalDivider(Modifier.padding(vertical = 4.dp))
        Text("Уведомления", style = MaterialTheme.typography.titleMedium)
        SwitchRow("Зарядка началась", draft.notifyChargeStart) { onChange(draft.copy(notifyChargeStart = it)) }
        SwitchRow("Зарядка завершена", draft.notifyChargeEnd) { onChange(draft.copy(notifyChargeEnd = it)) }
        SwitchRow("Низкий заряд", draft.notifyLowSoc) { onChange(draft.copy(notifyLowSoc = it)) }
        SwitchRow("Плата не в сети", draft.notifyOffline) { onChange(draft.copy(notifyOffline = it)) }
        SwitchRow("Поездка завершена", draft.notifyTripEnd) { onChange(draft.copy(notifyTripEnd = it)) }
        OutlinedTextField(
            value = draft.lowSoc,
            onValueChange = { onChange(draft.copy(lowSoc = it)) },
            label = { Text("Порог низкого заряда, %") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
        )

        HorizontalDivider(Modifier.padding(vertical = 4.dp))
        Text("Тариф", style = MaterialTheme.typography.titleMedium)
        OutlinedTextField(
            value = draft.tariffDay,
            onValueChange = { onChange(draft.copy(tariffDay = it)) },
            label = { Text("День, ₽/кВт·ч") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
        )
        OutlinedTextField(
            value = draft.tariffNight,
            onValueChange = { onChange(draft.copy(tariffNight = it)) },
            label = { Text("Ночь, ₽/кВт·ч") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedTextField(
                value = draft.nightFrom,
                onValueChange = { onChange(draft.copy(nightFrom = it)) },
                label = { Text("Ночь с") },
                modifier = Modifier.weight(1f),
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            )
            OutlinedTextField(
                value = draft.nightTo,
                onValueChange = { onChange(draft.copy(nightTo = it)) },
                label = { Text("Ночь до") },
                modifier = Modifier.weight(1f),
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            )
        }

        HorizontalDivider(Modifier.padding(vertical = 4.dp))
        Text("Фон", style = MaterialTheme.typography.titleMedium)
        SwitchRow("Фоновый мониторинг", draft.monitor) { onChange(draft.copy(monitor = it)) }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = onStart, modifier = Modifier.weight(1f)) { Text("Запустить") }
            OutlinedButton(onClick = onStop, modifier = Modifier.weight(1f)) { Text("Остановить") }
        }
        OutlinedButton(onClick = onBattery, modifier = Modifier.fillMaxWidth()) {
            Text("Разрешить работу в фоне")
        }

        Button(onClick = onSave, modifier = Modifier.fillMaxWidth().padding(top = 8.dp)) {
            Text("Сохранить настройки")
        }
        Text(
            "Для локального режима телефон должен быть в той же Wi-Fi, что и плата. Для MQTT нужен интернет.",
            fontSize = 11.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 20.dp),
        )
    }
}

@Composable
private fun ModeRow(label: String, value: String, draft: SettingsDraft, onChange: (SettingsDraft) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        RadioButton(selected = draft.mode == value, onClick = { onChange(draft.copy(mode = value)) })
        Text(label)
    }
}

@Composable
private fun SwitchRow(label: String, checked: Boolean, onCheckedChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(label, modifier = Modifier.weight(1f))
        Switch(checked = checked, onCheckedChange = onCheckedChange)
    }
}

private fun money(v: Double): String =
    if (v >= 0.5) "%.0f ₽".format(v) else "—"
