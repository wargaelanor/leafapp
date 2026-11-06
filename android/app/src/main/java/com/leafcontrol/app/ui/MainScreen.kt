package com.leafcontrol.app.ui

import android.content.Intent
import android.content.Context
import android.net.Uri
import android.widget.Toast
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.LocalIndication
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.PagerDefaults
import androidx.compose.foundation.pager.PagerSnapDistance
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.AcUnit
import androidx.compose.material.icons.outlined.BatteryChargingFull
import androidx.compose.material.icons.outlined.Bolt
import androidx.compose.material.icons.outlined.ChevronRight
import androidx.compose.material.icons.outlined.Delete
import androidx.compose.material.icons.outlined.DirectionsCar
import androidx.compose.material.icons.outlined.Language
import androidx.compose.material.icons.outlined.FileDownload
import androidx.compose.material.icons.outlined.Home
import androidx.compose.material.icons.outlined.Info
import androidx.compose.material.icons.outlined.Lightbulb
import androidx.compose.material.icons.outlined.Lock
import androidx.compose.material.icons.outlined.LockOpen
import androidx.compose.material.icons.outlined.MoreHoriz
import androidx.compose.material.icons.outlined.PowerSettingsNew
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material.icons.outlined.Route
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material.icons.outlined.Speed
import androidx.compose.material.icons.outlined.SystemUpdate
import androidx.compose.material.icons.outlined.Thermostat
import androidx.compose.material.icons.outlined.VolumeUp
import androidx.compose.material.icons.outlined.Wifi
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import com.leafcontrol.app.BuildConfig
import com.leafcontrol.app.BoardSettingsActivity
import com.leafcontrol.app.BoardUpdateActivity
import com.leafcontrol.app.CsvWriter
import com.leafcontrol.app.R
import com.leafcontrol.app.SettingsActivity
import com.leafcontrol.app.data.Charge
import com.leafcontrol.app.data.LeafRepo
import com.leafcontrol.app.data.LeafState
import com.leafcontrol.app.data.Settings
import com.leafcontrol.app.data.Trip
import kotlinx.coroutines.launch
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlinx.coroutines.delay

private val dateFmt = SimpleDateFormat("dd MMM, HH:mm", Locale("ru"))
private val online = Color(0xFF34C759)

private data class TabItem(val label: String, val icon: ImageVector)

private val tabs = listOf(
    TabItem("Гараж", Icons.Outlined.Home),
    TabItem("Дистанция", Icons.Outlined.Bolt),
    TabItem("Состояние", Icons.Outlined.Speed),
    TabItem("Зарядки", Icons.Outlined.BatteryChargingFull),
    TabItem("Поездки", Icons.Outlined.Route),
    TabItem("Ещё", Icons.Outlined.MoreHoriz),
)

@Composable
fun MainScreen(vm: LeafViewModel = viewModel()) {
    val state by vm.state.collectAsStateWithLifecycle()
    val mqttConnected by LeafRepo.mqttConnected.collectAsStateWithLifecycle()
    val trips by vm.trips.collectAsStateWithLifecycle()
    val charges by vm.charges.collectAsStateWithLifecycle()
    val snack = remember { SnackbarHostState() }
    val pager = rememberPagerState(pageCount = { tabs.size })
    val scope = rememberCoroutineScope()

    LaunchedEffect(Unit) {
        vm.message.collect { msg ->
            if (msg != null) {
                snack.showSnackbar(msg)
                vm.message.value = null
            }
        }
    }

    Box(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background)) {
        Column(
            Modifier
                .fillMaxSize()
                .statusBarsPadding()
                .navigationBarsPadding()
        ) {
            LeafTopBar(state) { vm.refresh() }
            Box(Modifier.weight(1f).fillMaxWidth()) {
                HorizontalPager(
                    state = pager,
                    modifier = Modifier.fillMaxSize(),
                    beyondViewportPageCount = 1,
                    flingBehavior = PagerDefaults.flingBehavior(
                        state = pager,
                        pagerSnapDistance = PagerSnapDistance.atMost(1),
                        snapPositionalThreshold = 0.2f,
                    ),
                ) { page ->
                    when (page) {
                        0 -> GarageScreen(
                            state = state,
                            onCommand = vm::command,
                            onOpenTab = { scope.launch { pager.animateScrollToPage(it) } },
                            onSettings = { vm.openSettings() },
                        )
                        1 -> DistanceScreen(state, vm::command)
                        2 -> StateScreen(state)
                        3 -> HistoryScreen(
                            charges = charges,
                            isCharges = true,
                            onClear = { vm.clearHistory(true) },
                            onExport = { exportCharges(vm, charges) },
                        )
                        else -> if (page == 4) {
                            HistoryScreen(
                                trips = trips,
                                isCharges = false,
                                onClear = { vm.clearHistory(false) },
                                onExport = { exportTrips(vm, trips) },
                            )
                        } else {
                            MoreScreen(
                                state = state,
                                mqttConnected = mqttConnected,
                                onSettings = { vm.openSettings() },
                                onCheckUpdate = { vm.checkUpdate() },
                            )
                        }
                    }
                }
            }
            LeafBottomBar(pager.currentPage) { index ->
                scope.launch { pager.animateScrollToPage(index) }
            }
        }
        SnackbarHost(
            hostState = snack,
            modifier = Modifier.align(Alignment.BottomCenter).padding(bottom = 76.dp),
        )
    }
}

@Composable
private fun LeafTopBar(state: LeafState, onRefresh: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .height(56.dp)
            .background(MaterialTheme.colorScheme.surfaceVariant)
            .padding(horizontal = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        LeafLogo(Modifier.size(28.dp), MaterialTheme.colorScheme.onSurface)
        Spacer(Modifier.width(10.dp))
        Text(
            "Leaf Control",
            fontSize = 18.sp,
            letterSpacing = 0.6.sp,
            color = MaterialTheme.colorScheme.onSurface,
        )
        Spacer(Modifier.weight(1f))
        IconButton(onClick = onRefresh) {
            Icon(
                Icons.Outlined.Refresh,
                contentDescription = "Обновить",
                tint = MaterialTheme.colorScheme.onSurface,
            )
        }
        Box(
            Modifier
                .size(8.dp)
                .background(if (state.connected) online else MaterialTheme.colorScheme.error, CircleShape)
        )
        Spacer(Modifier.width(6.dp))
        Text(
            if (state.connected) "ONLINE" else "OFFLINE",
            fontSize = 9.sp,
            letterSpacing = 1.2.sp,
            color = if (state.connected) online else MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun LeafLogo(modifier: Modifier, color: Color) {
    Canvas(modifier) {
        val stroke = 1.8.dp.toPx()
        val r = size.minDimension * 0.30f
        val cy = size.height / 2f
        drawCircle(color, r, Offset(size.width / 2f, cy), style = androidx.compose.ui.graphics.drawscope.Stroke(stroke))
        drawLine(color, Offset(0f, cy), Offset(size.width / 2f - r, cy), stroke)
        drawLine(color, Offset(size.width / 2f + r, cy), Offset(size.width, cy), stroke)
    }
}

@Composable
private fun LeafBottomBar(selected: Int, onSelect: (Int) -> Unit) {
    val outline = MaterialTheme.colorScheme.outline
    Row(
        Modifier
            .fillMaxWidth()
            .height(56.dp)
            .background(MaterialTheme.colorScheme.surfaceVariant)
            .drawBehind {
                drawLine(
                    outline,
                    Offset(0f, 0f),
                    Offset(size.width, 0f),
                    1.dp.toPx(),
                )
            }
    ) {
        tabs.forEachIndexed { index, tab ->
            val active = selected == index
            val interaction = remember { MutableInteractionSource() }
            Column(
                Modifier
                    .weight(1f)
                    .fillMaxHeight()
                    .pressable(interaction) { onSelect(index) },
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center,
            ) {
                Box(
                    Modifier
                        .width(24.dp)
                        .height(2.dp)
                        .background(if (active) MaterialTheme.colorScheme.primary else Color.Transparent)
                )
                Spacer(Modifier.height(6.dp))
                Icon(
                    tab.icon,
                    contentDescription = tab.label,
                    modifier = Modifier.size(22.dp),
                    tint = if (active) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(3.dp))
                Text(
                    tab.label,
                    fontSize = 9.sp,
                    color = if (active) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun GarageScreen(
    state: LeafState,
    onCommand: (String, Double, Int) -> Unit,
    onOpenTab: (Int) -> Unit,
    onSettings: () -> Unit,
) {
    val list = rememberLazyListState()
    LazyColumn(
        state = list,
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        item {
            Surface(
                shape = RoundedCornerShape(14.dp),
                color = MaterialTheme.colorScheme.surface,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Column(Modifier.padding(12.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                    CarStage(state)
                    Text(
                        "NISSAN LEAF",
                        modifier = Modifier.padding(top = 8.dp),
                        fontSize = 15.sp,
                        letterSpacing = 1.sp,
                        color = MaterialTheme.colorScheme.onSurface,
                    )
                }
            }
        }
        item { SocCard(state, onCommand) }
        item { QuickActions(state, onCommand) }
        item { SectionTitle("ФУНКЦИИ") }
        item {
            Surface(
                shape = RoundedCornerShape(12.dp),
                color = MaterialTheme.colorScheme.surface,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Column {
                    FunctionRow(
                        Icons.Outlined.DirectionsCar,
                        "Состояние авто",
                        "Батарея ${pct(state.soc)} · ${if (state.hvac) "климат включён" else "климат выключен"}",
                    ) { onOpenTab(2) }
                    FunctionDivider()
                    FunctionRow(
                        Icons.Outlined.BatteryChargingFull,
                        "Зарядка",
                        "${chargerRu(state)} · ${fmt(state.power, 1)} кВт",
                    ) { onOpenTab(1) }
                    FunctionDivider()
                    FunctionRow(
                        Icons.Outlined.Route,
                        "Поездки",
                        "История поездок и зарядок",
                    ) { onOpenTab(4) }
                    FunctionDivider()
                    FunctionRow(
                        Icons.Outlined.Settings,
                        "Настройки",
                        "Уведомления, связь и обновления",
                        onSettings,
                    )
                }
            }
        }
        item { Spacer(Modifier.height(4.dp)) }
    }
}

@Composable
private fun CarStage(state: LeafState) {
    var wheelFrame by remember { mutableIntStateOf(0) }
    val moving = state.on == 1 && !state.charging
    LaunchedEffect(moving) {
        if (!moving) {
            wheelFrame = 0
            return@LaunchedEffect
        }
        while (true) {
            delay(140)
            wheelFrame = (wheelFrame + 1) % 4
        }
    }
    Box(Modifier.fillMaxWidth().height(180.dp)) {
        Image(
            painterResource(R.drawable.car_leaf),
            contentDescription = "Nissan Leaf",
            contentScale = ContentScale.Fit,
            modifier = Modifier.fillMaxSize(),
        )
        if (state.lights && state.lastUpdateMs > 0) {
            Image(
                painterResource(R.drawable.car_lights),
                contentDescription = null,
                contentScale = ContentScale.Fit,
                modifier = Modifier.fillMaxSize(),
            )
        }
        // The cable is drawn whenever the connector is plugged: an active
        // charge, a finished one (charger=done) or a scheduled wait (wait).
        // "charging" alone hid the plug when the session ended but the cable
        // stayed in the socket.
        val plugged = state.charging || state.charger == "done" || state.charger == "wait"
        if (plugged && state.lastUpdateMs > 0) {
            Image(
                painterResource(R.drawable.car_cable),
                contentDescription = null,
                contentScale = ContentScale.Fit,
                modifier = Modifier.fillMaxSize(),
            )
        }
        if (moving) {
            val wheel = when (wheelFrame) {
                0 -> R.drawable.car_wheel1
                1 -> R.drawable.car_wheel2
                2 -> R.drawable.car_wheel3
                else -> R.drawable.car_wheel4
            }
            Image(
                painterResource(wheel),
                contentDescription = null,
                contentScale = ContentScale.Fit,
                modifier = Modifier.fillMaxSize(),
            )
        }
    }
}

@Composable
private fun SocCard(state: LeafState, onCommand: (String, Double, Int) -> Unit) {
    Surface(
        shape = RoundedCornerShape(14.dp),
        color = MaterialTheme.colorScheme.surface,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(16.dp)) {
            Box(
                Modifier
                    .width(26.dp)
                    .height(1.dp)
                    .background(MaterialTheme.colorScheme.primary)
            )
            Spacer(Modifier.height(8.dp))
            Text("Уровень заряда", fontSize = 16.sp, color = MaterialTheme.colorScheme.onSurface)
            Spacer(Modifier.height(12.dp))
            Row(verticalAlignment = Alignment.Bottom) {
                if (state.displayRange != null) {
                    Text("≈", fontSize = 28.sp, color = MaterialTheme.colorScheme.onSurface)
                }
                Text(
                    state.displayRange?.toString() ?: "—",
                    fontSize = 54.sp,
                    fontWeight = FontWeight.Light,
                    letterSpacing = (-1).sp,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                Text(
                    "км",
                    fontSize = 14.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(start = 6.dp, bottom = 10.dp),
                )
            }
            Spacer(Modifier.height(10.dp))
            ChargeBar(state.soc)
            Spacer(Modifier.height(8.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text(
                    if (state.soc >= 0) "${state.soc.toInt()}%" else "—",
                    fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                Text(
                    chargerRu(state),
                    fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Spacer(Modifier.height(14.dp))
            HoldAction(
                onTrigger = {
                    onCommand(if (state.charging) "chargeoff" else "charge", 22.0, 15)
                },
                modifier = Modifier.fillMaxWidth().height(46.dp),
                ring = false,
            ) { progress, pressed ->
                Surface(
                    shape = RoundedCornerShape(8.dp),
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.fillMaxSize(),
                ) {
                    Box(contentAlignment = Alignment.Center) {
                        if (pressed && progress > 0f) {
                            HoldFill(progress, Color.White)
                        }
                        Text(
                            if (state.charging) "Остановить зарядку" else "Начать зарядку",
                            fontSize = 15.sp,
                            color = MaterialTheme.colorScheme.onPrimary,
                        )
                    }
                }
            }
            Text(
                "Удерживайте 1,5 с",
                fontSize = 10.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.align(Alignment.CenterHorizontally).padding(top = 6.dp),
            )
        }
    }
}

@Composable
private fun ChargeBar(soc: Double) {
    val fraction = (soc / 100.0).toFloat().coerceIn(0f, 1f)
    Box(
        Modifier
            .fillMaxWidth()
            .height(10.dp)
            .clip(RoundedCornerShape(5.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant)
    ) {
        Box(
            Modifier
                .fillMaxWidth(fraction)
                .fillMaxHeight()
                .clip(RoundedCornerShape(5.dp))
                .background(MaterialTheme.colorScheme.primary)
        )
    }
}

@Composable
private fun QuickActions(state: LeafState, onCommand: (String, Double, Int) -> Unit) {
    Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceEvenly,
        ) {
            QuickAction(
                Icons.Outlined.Lightbulb,
                "Вспышка фар",
                state.lights,
                Color(0xFFFFA726),
            ) { onCommand("lights", 22.0, 15) }
            QuickAction(
                if (state.locked == 1) Icons.Outlined.Lock else Icons.Outlined.LockOpen,
                "Двери",
                state.locked == 1,
                MaterialTheme.colorScheme.primary,
            ) { onCommand(if (state.locked == 1) "unlock" else "lock", 22.0, 15) }
            QuickAction(
                Icons.Outlined.AcUnit,
                "Климат",
                state.hvac,
                Color(0xFF34C759),
            ) { onCommand("hvac", 22.0, 15) }
        }
        Text(
            "Удерживайте 1,5 с",
            fontSize = 10.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(top = 6.dp),
        )
    }
}

@Composable
private fun QuickAction(
    icon: ImageVector,
    label: String,
    active: Boolean,
    activeColor: Color,
    onClick: () -> Unit,
) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        HoldAction(
            onTrigger = onClick,
            modifier = Modifier.size(64.dp),
        ) { _, _ ->
            Surface(
                shape = CircleShape,
                color = MaterialTheme.colorScheme.surfaceVariant,
                border = if (active) BorderStroke(1.5.dp, activeColor) else null,
                modifier = Modifier.fillMaxSize(),
            ) {
                Box(contentAlignment = Alignment.Center) {
                    Icon(
                        icon,
                        contentDescription = label,
                        modifier = Modifier.size(28.dp),
                        tint = if (active) activeColor else MaterialTheme.colorScheme.onSurface,
                    )
                }
            }
        }
        Spacer(Modifier.height(8.dp))
        Text(label, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurface)
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(
        text,
        fontSize = 11.sp,
        letterSpacing = 1.2.sp,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(start = 4.dp, top = 2.dp),
    )
}

@Composable
private fun FunctionRow(
    icon: ImageVector,
    title: String,
    subtitle: String,
    onClick: () -> Unit,
) {
    val interaction = remember { MutableInteractionSource() }
    Row(
        Modifier
            .fillMaxWidth()
            .pressable(interaction) { onClick() }
            .padding(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Surface(
            shape = RoundedCornerShape(10.dp),
            color = MaterialTheme.colorScheme.surfaceVariant,
            modifier = Modifier.size(38.dp),
        ) {
            Box(contentAlignment = Alignment.Center) {
                Icon(icon, contentDescription = null, modifier = Modifier.size(20.dp), tint = MaterialTheme.colorScheme.primary)
            }
        }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(title, fontSize = 14.sp, color = MaterialTheme.colorScheme.onSurface)
            Text(subtitle, fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Icon(
            Icons.Outlined.ChevronRight,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun FunctionDivider() {
    androidx.compose.material3.HorizontalDivider(
        color = MaterialTheme.colorScheme.outline,
        thickness = 1.dp,
        modifier = Modifier.padding(start = 62.dp),
    )
}

@Composable
private fun DistanceScreen(state: LeafState, onCommand: (String, Double, Int) -> Unit) {
    var setpoint by rememberSaveable {
        mutableFloatStateOf(if (state.setpoint in 16.0..30.0) state.setpoint.toFloat() else 22f)
    }
    var minutes by remember { mutableIntStateOf(Settings.climateMinutes) }
    var remaining by remember { mutableIntStateOf(0) }
    var total by remember { mutableIntStateOf(0) }
    var wasRunning by remember { mutableStateOf(false) }
    LaunchedEffect(state.hvac, state.hvacTimer) {
        if (!state.hvac) {
            remaining = 0
            total = 0
            wasRunning = false
            return@LaunchedEffect
        }
        if (!wasRunning) {
            total = maxOf(state.hvacTimer, 1)
            wasRunning = true
        }
        remaining = state.hvacTimer
        while (remaining > 0) {
            delay(60_000)
            remaining -= 1
        }
    }
    val climateRunning = state.fresh() && state.hvac && remaining > 0
    // The countdown lives on the climate button itself now: the ring depletes
    // counter-clockwise from the full duration, the label shows the minutes
    // left (or ∞ when the board runs climate without a timer).
    val climateInfinite = state.fresh() && state.hvac && state.hvacTimer == 0
    val ringFraction = when {
        climateRunning -> (remaining.toFloat() / maxOf(total, 1)).coerceIn(0f, 1f)
        climateInfinite -> 1f
        else -> 0f
    }
    val ringLabel = when {
        climateRunning -> "$remaining мин"
        climateInfinite -> "∞"
        else -> null
    }
    val ringWarn = climateRunning && remaining <= 5
    Column(
        Modifier.fillMaxSize().padding(horizontal = 16.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Surface(
                shape = RoundedCornerShape(14.dp),
                color = MaterialTheme.colorScheme.surface,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Column(Modifier.padding(16.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(Icons.Outlined.Thermostat, null, tint = MaterialTheme.colorScheme.primary)
                        Spacer(Modifier.width(8.dp))
                        Text(
                            when {
                                !state.fresh() -> "Климат: нет данных"
                                state.hvac -> "Климат включён"
                                else -> "Климат выключен"
                            },
                            fontSize = 16.sp,
                            color = MaterialTheme.colorScheme.onSurface,
                        )
                        Spacer(Modifier.weight(1f))
                        Text(
                            "${setpoint.toInt()} °C",
                            fontSize = 22.sp,
                            fontWeight = FontWeight.Light,
                            color = MaterialTheme.colorScheme.onSurface,
                        )
                    }
                    Slider(
                        value = setpoint,
                        onValueChange = { setpoint = it },
                        valueRange = 16f..30f,
                        steps = 13,
                    )
                    Text("Длительность", fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Row(
                        Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(6.dp),
                    ) {
                        listOf(5, 10, 15, 20, 30, 0).forEach { m ->
                            FilterChip(
                                selected = minutes == m,
                                onClick = {
                                    minutes = m
                                    Settings.climateMinutes = m
                                },
                                label = {
                                    Text(
                                        if (m == 0) "∞" else "$m",
                                        fontSize = 12.sp,
                                        maxLines = 1,
                                    )
                                },
                                modifier = Modifier.weight(1f),
                            )
                        }
                    }
                }
            }
        SectionTitle("КОМАНДЫ")
        CommandPad(state, setpoint, minutes, onCommand, ringFraction, ringLabel, ringWarn)
    }
}

@Composable
private fun CommandPad(
    state: LeafState,
    setpoint: Float,
    minutes: Int,
    onCommand: (String, Double, Int) -> Unit,
    climateRingFraction: Float = 0f,
    climateRingLabel: String? = null,
    climateRingWarn: Boolean = false,
) {
    val timerProgress = climateRingFraction
    Box(Modifier.fillMaxWidth().height(465.dp)) {
        Row(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = 20.dp)
                .align(Alignment.TopCenter),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            CircleCommand("Сигнал и фары", Icons.Outlined.VolumeUp) {
                onCommand("hornlights", 22.0, 15)
            }
            CircleCommand("Закрыть", Icons.Outlined.Lock) {
                onCommand("lock", 22.0, 15)
            }
        }
        HoldAction(
            onTrigger = { onCommand("hvac", setpoint.toDouble(), minutes) },
            modifier = Modifier
                .size(164.dp)
                .align(Alignment.TopCenter)
                .offset(y = 100.dp),
            idleProgress = timerProgress,
            idleRingColor = if (climateRingWarn) {
                MaterialTheme.colorScheme.error
            } else {
                MaterialTheme.colorScheme.primary
            },
        ) { _, _ ->
            Surface(
                shape = CircleShape,
                color = MaterialTheme.colorScheme.surface,
                border = BorderStroke(1.2.dp, commandBorderColor()),
                modifier = Modifier.fillMaxSize(),
            ) {
                Column(
                    Modifier.fillMaxSize(),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.Center,
                ) {
                    Icon(
                        Icons.Outlined.AcUnit,
                        contentDescription = null,
                        modifier = Modifier.size(44.dp),
                        tint = MaterialTheme.colorScheme.primary,
                    )
                    Spacer(Modifier.height(8.dp))
                    Text(
                        "КЛИМАТ",
                        fontSize = 11.sp,
                        letterSpacing = 1.sp,
                        fontWeight = FontWeight.SemiBold,
                        color = MaterialTheme.colorScheme.onSurface,
                    )
                    if (climateRingLabel != null) {
                        Text(
                            climateRingLabel,
                            fontSize = 12.sp,
                            fontWeight = FontWeight.Medium,
                            color = if (climateRingWarn) {
                                MaterialTheme.colorScheme.error
                            } else {
                                MaterialTheme.colorScheme.primary
                            },
                        )
                    }
                }
            }
        }
        Row(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = 20.dp)
                .align(Alignment.TopCenter)
                .offset(y = 245.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            CircleCommand("Вспышка фар", Icons.Outlined.Lightbulb) {
                onCommand("lights", 22.0, 15)
            }
            CircleCommand("Открыть", Icons.Outlined.LockOpen) {
                onCommand("unlock", 22.0, 15)
            }
        }
        Row(
            Modifier
                .fillMaxWidth()
                .align(Alignment.TopCenter)
                .offset(y = 385.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            PillCommand(
                if (state.charging) "Стоп зарядки" else "Зарядка",
                Icons.Outlined.BatteryChargingFull,
                Modifier.weight(1f),
            ) { onCommand(if (state.charging) "chargeoff" else "charge", 22.0, 15) }
            PillCommand(
                "Обновить",
                Icons.Outlined.Refresh,
                Modifier.weight(1f),
                hold = false,
            ) { onCommand("refresh", 22.0, 15) }
        }
        Text(
            "Удерживайте 1,5 с",
            fontSize = 10.sp,
            textAlign = TextAlign.Center,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier
                .align(Alignment.TopCenter)
                .offset(y = 445.dp),
        )
    }
}

@Composable
private fun commandBorderColor(): Color =
    if (isSystemInDarkTheme()) Color(0xFF3F4B59) else Color(0xFFC9D2DC)

@Composable
private fun CircleCommand(label: String, icon: ImageVector, onClick: () -> Unit) {
    HoldAction(
        onTrigger = onClick,
        modifier = Modifier.size(116.dp),
    ) { _, _ ->
        Surface(
            shape = CircleShape,
            color = MaterialTheme.colorScheme.surface,
            border = BorderStroke(1.2.dp, commandBorderColor()),
            modifier = Modifier.fillMaxSize(),
        ) {
            Column(
                Modifier.fillMaxSize().padding(10.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center,
            ) {
                Icon(
                    icon,
                    contentDescription = label,
                    modifier = Modifier.size(30.dp),
                    tint = MaterialTheme.colorScheme.onSurface,
                )
                Spacer(Modifier.height(5.dp))
                Text(
                    label.uppercase(),
                    fontSize = 9.sp,
                    lineHeight = 11.sp,
                    textAlign = TextAlign.Center,
                    maxLines = 2,
                    color = MaterialTheme.colorScheme.onSurface,
                )
            }
        }
    }
}

@Composable
private fun PillCommand(
    label: String,
    icon: ImageVector,
    modifier: Modifier = Modifier,
    hold: Boolean = true,
    onClick: () -> Unit,
) {
    if (!hold) {
        val interaction = remember { MutableInteractionSource() }
        Surface(
            shape = RoundedCornerShape(50),
            color = MaterialTheme.colorScheme.surface,
            border = BorderStroke(1.2.dp, commandBorderColor()),
            modifier = modifier.height(53.dp).pressable(interaction) { onClick() },
        ) {
            Row(
                Modifier.fillMaxSize(),
                horizontalArrangement = Arrangement.Center,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(icon, null, Modifier.size(20.dp), tint = MaterialTheme.colorScheme.primary)
                Spacer(Modifier.width(6.dp))
                Text(label, fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurface)
            }
        }
        return
    }
    HoldAction(
        onTrigger = onClick,
        modifier = modifier.height(53.dp),
        ring = false,
    ) { progress, pressed ->
        Surface(
            shape = RoundedCornerShape(50),
            color = MaterialTheme.colorScheme.surface,
            border = BorderStroke(1.2.dp, commandBorderColor()),
            modifier = Modifier.fillMaxSize(),
        ) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                if (pressed && progress > 0f) {
                    HoldFill(progress, MaterialTheme.colorScheme.primary)
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(icon, null, Modifier.size(20.dp), tint = MaterialTheme.colorScheme.primary)
                    Spacer(Modifier.width(6.dp))
                    Text(label, fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurface)
                }
            }
        }
    }
}

/** Solid fill with a bright leading edge for a hold button. */
@Composable
private fun BoxScope.HoldFill(progress: Float, color: Color) {
    Box(
        Modifier
            .fillMaxWidth(progress.coerceIn(0f, 1f))
            .fillMaxHeight()
            .background(color.copy(alpha = 0.28f))
    ) {
        Box(
            Modifier
                .align(Alignment.CenterEnd)
                .width(2.dp)
                .fillMaxHeight()
                .background(color)
        )
    }
}

@Composable
private fun CommandRow(title: String, action: String, icon: ImageVector, onClick: () -> Unit) {
    val interaction = remember { MutableInteractionSource() }
    Surface(
        shape = RoundedCornerShape(12.dp),
        color = MaterialTheme.colorScheme.surface,
        modifier = Modifier.fillMaxWidth().pressable(interaction) { onClick() },
    ) {
        Row(Modifier.padding(14.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(icon, null, tint = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.width(12.dp))
            Text(title, modifier = Modifier.weight(1f), fontSize = 14.sp, color = MaterialTheme.colorScheme.onSurface)
            Text(action, fontSize = 12.sp, color = MaterialTheme.colorScheme.primary)
        }
    }
}

@Composable
private fun StateScreen(state: LeafState) {
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        item {
            Surface(
                shape = RoundedCornerShape(12.dp),
                color = if (state.connected) MaterialTheme.colorScheme.surface else MaterialTheme.colorScheme.surfaceVariant,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Column(Modifier.padding(14.dp)) {
                    Text(
                        if (state.connected) "Обновлено ${ago(state.lastUpdateMs)} назад" else "Нет связи · последние сохранённые данные",
                        fontSize = 13.sp,
                        color = if (state.connected) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.error,
                    )
                    if (state.fw.isNotEmpty()) {
                        Text(state.fw, fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            }
        }
        val metrics = listOf(
            "Заряд" to pct(state.soc),
            "Запас" to (state.displayRange?.let { "$it км" } ?: "—"),
            "Мощность" to "${fmt(state.power, 1)} кВт",
            "Напряжение" to if (state.voltage > 0) "${state.voltage.toInt()} В" else "—",
            "Ток" to "${fmt(state.current, 1)} А",
            "Батарея" to temp(state.batteryTemp),
            "Салон" to temp(state.cabin),
            "Улица" to temp(state.ambient),
            "SOH" to if (state.soh > 0) "${state.soh}%" else "—",
            "Пробег" to if (state.odometer > 0) "${state.odometer} км" else "—",
            "Замки" to when (state.locked) { 0 -> "Открыто"; 1 -> "Закрыто"; else -> "—" },
            "Свет" to if (state.lights) "Включён" else "Выключен",
            "Климат" to if (state.hvac) "Включён" else "Выключен",
            "Зарядка" to chargerRu(state),
            "Wi-Fi" to if (state.wifiRssi != 0) "${state.wifiRssi} dBm" else "—",
            "Модем" to if (state.modem.isNotEmpty()) state.modem else "—",
        )
        metrics.chunked(2).forEach { pair ->
            item {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    pair.forEach { (label, value) ->
                        MetricCard(label, value, Modifier.weight(1f))
                    }
                    if (pair.size == 1) Spacer(Modifier.weight(1f))
                }
            }
        }
        if (state.vin.isNotEmpty()) {
            item { MetricCard("VIN", state.vin, Modifier.fillMaxWidth()) }
        }
        if (state.fwDesc.isNotEmpty()) {
            item { MetricCard("Прошивка", state.fwDesc, Modifier.fillMaxWidth()) }
        }
        item { Spacer(Modifier.height(4.dp)) }
    }
}

@Composable
private fun MetricCard(label: String, value: String, modifier: Modifier = Modifier) {
    Surface(
        shape = RoundedCornerShape(12.dp),
        color = MaterialTheme.colorScheme.surface,
        modifier = modifier,
    ) {
        Column(Modifier.padding(12.dp)) {
            Text(value, fontSize = 17.sp, fontWeight = FontWeight.SemiBold, color = MaterialTheme.colorScheme.onSurface)
            Text(label, fontSize = 10.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun HistoryScreen(
    trips: List<Trip> = emptyList(),
    charges: List<Charge> = emptyList(),
    isCharges: Boolean,
    onClear: () -> Unit,
    onExport: () -> Unit,
) {
    val list = if (isCharges) charges else trips
    val count = list.size
    val km = trips.sumOf { it.distKm }
    val kwh = if (isCharges) charges.sumOf { it.energyWh } / 1000.0 else trips.sumOf { it.energyWh } / 1000.0
    val hours = charges.sumOf { (it.endMs - it.startMs) / 3_600_000.0 }
    val rub = if (isCharges) charges.sumOf { LeafRepo.cost(it) } else trips.sumOf { LeafRepo.cost(it) }
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        item {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                SummaryCell(if (isCharges) "$count" else "$count", if (isCharges) "сеансов" else "поездок", Modifier.weight(1f))
                if (isCharges) {
                    SummaryCell(fmt(kwh, 2), "кВт·ч", Modifier.weight(1f))
                    SummaryCell(fmt(hours, 1), "часов", Modifier.weight(1f))
                } else {
                    SummaryCell(fmt(km, 1), "км", Modifier.weight(1f))
                    SummaryCell(fmt(kwh, 2), "кВт·ч", Modifier.weight(1f))
                }
                SummaryCell(if (rub > 0.5) "${rub.toInt()} ₽" else "—", "стоимость", Modifier.weight(1f))
            }
        }
        item {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = onExport, modifier = Modifier.weight(1f)) {
                    Icon(Icons.Outlined.FileDownload, null, Modifier.size(16.dp))
                    Spacer(Modifier.width(6.dp))
                    Text("CSV", fontSize = 12.sp)
                }
                OutlinedButton(onClick = onClear, modifier = Modifier.weight(1f)) {
                    Icon(Icons.Outlined.Delete, null, Modifier.size(16.dp))
                    Spacer(Modifier.width(6.dp))
                    Text("Очистить", fontSize = 12.sp)
                }
            }
        }
        if (list.isEmpty()) {
            item {
                Box(Modifier.fillMaxWidth().padding(40.dp), contentAlignment = Alignment.Center) {
                    Text(
                        if (isCharges) "Зарядок пока нет" else "Поездок пока нет",
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        } else {
            if (isCharges) {
                items(charges.sortedByDescending { it.startMs }) { c -> ChargeCard(c) }
            } else {
                items(trips.sortedByDescending { it.startMs }) { t -> TripCard(t) }
            }
        }
        item { Spacer(Modifier.height(4.dp)) }
    }
}

@Composable
private fun SummaryCell(value: String, label: String, modifier: Modifier = Modifier) {
    Surface(
        shape = RoundedCornerShape(12.dp),
        color = MaterialTheme.colorScheme.surface,
        modifier = modifier,
    ) {
        Column(
            Modifier.padding(vertical = 10.dp, horizontal = 6.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(value, fontSize = 16.sp, fontWeight = FontWeight.SemiBold, color = MaterialTheme.colorScheme.onSurface)
            Text(label, fontSize = 9.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun TripCard(t: Trip) {
    val per100 = LeafRepo.tripPer100(t)
    Surface(
        shape = RoundedCornerShape(12.dp),
        color = MaterialTheme.colorScheme.surface,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(Modifier.fillMaxWidth()) {
                Text(dateFmt.format(Date(t.startMs)), fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurface)
                Spacer(Modifier.weight(1f))
                Text(LeafRepo.fmtDuration(t.endMs - t.startMs), fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                HistoryValue("${fmt(t.distKm, 1)}", "км")
                HistoryValue("${fmt(t.energyWh / 1000.0, 2)}", "кВт·ч")
                if (per100 != null) HistoryValue(fmt(per100, 1), "кВт·ч/100")
                HistoryValue(if (LeafRepo.cost(t) > 0.5) "${LeafRepo.cost(t).toInt()} ₽" else "—", "стоимость")
            }
            if (t.startSoc != null && t.endSoc != null) {
                Text("Заряд ${t.startSoc}→${t.endSoc}%", fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

@Composable
private fun ChargeCard(c: Charge) {
    Surface(
        shape = RoundedCornerShape(12.dp),
        color = MaterialTheme.colorScheme.surface,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(Modifier.fillMaxWidth()) {
                Text(dateFmt.format(Date(c.startMs)), fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurface)
                Spacer(Modifier.weight(1f))
                Text(LeafRepo.fmtDuration(c.endMs - c.startMs), fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                HistoryValue(fmt(c.energyWh / 1000.0, 2), "кВт·ч")
                HistoryValue(if (LeafRepo.cost(c) > 0.5) "${LeafRepo.cost(c).toInt()} ₽" else "—", "стоимость")
            }
            if (c.startSoc != null && c.endSoc != null) {
                Text("Заряд ${c.startSoc}→${c.endSoc}%", fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

@Composable
private fun HistoryValue(value: String, label: String) {
    Row(verticalAlignment = Alignment.Bottom) {
        Text(value, fontSize = 13.sp, fontWeight = FontWeight.SemiBold, color = MaterialTheme.colorScheme.onSurface)
        Spacer(Modifier.width(3.dp))
        Text(label, fontSize = 9.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun MoreScreen(
    state: LeafState,
    mqttConnected: Boolean,
    onSettings: () -> Unit,
    onCheckUpdate: () -> Unit,
) {
    var showAbout by remember { mutableStateOf(false) }
    val ctx = LocalContext.current
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item { SectionTitle("ПРИЛОЖЕНИЕ") }
        item { MoreRow(Icons.Outlined.Settings, "Настройки", "Уведомления, связь, фоновый режим") { onSettings() } }
        item { MoreRow(Icons.Outlined.SystemUpdate, "Проверить обновление", "Версия ${BuildConfig.VERSION_NAME}") { onCheckUpdate() } }
        item { MoreRow(Icons.Outlined.Info, "О приложении", "Возможности и версия") { showAbout = true } }
        item { SectionTitle("ПЛАТА") }
        item {
            MoreRow(Icons.Outlined.Wifi, "Настройки платы", "Wi-Fi, MQTT, тариф — прямо в приложении") {
                ctx.startActivity(Intent(ctx, BoardSettingsActivity::class.java))
            }
        }
        item {
            MoreRow(Icons.Outlined.SystemUpdate, "Обновление прошивки платы", "Проверка и заливка без браузера") {
                ctx.startActivity(Intent(ctx, BoardUpdateActivity::class.java))
            }
        }
        item {
            MoreRow(Icons.Outlined.Language, "Web-интерфейс платы", "Полная страница платы в браузере") {
                openBoard(ctx, "/")
            }
        }
        item { SectionTitle("СВЯЗЬ") }
        item {
            Surface(
                shape = RoundedCornerShape(12.dp),
                color = MaterialTheme.colorScheme.surface,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    val modeLabel = when (Settings.mode) {
                        "local" -> "Локально"
                        "mqtt" -> "MQTT"
                        else -> "Авто"
                    }
                    val mqttConfigured = Settings.mqttUser.isNotEmpty()
                    val mqttUsed = Settings.mode == "mqtt" ||
                        (Settings.mode == "auto" &&
                            (state.transport == "mqtt" || (mqttConfigured && !state.connected)))
                    StatusRow(
                        "MQTT-сервер",
                        when {
                            !mqttUsed -> "не используется"
                            mqttConnected -> "подключён"
                            else -> "нет связи"
                        },
                        when {
                            !mqttUsed -> null
                            mqttConnected -> true
                            else -> false
                        },
                    )
                    StatusRow(
                        "Плата",
                        if (state.connected) "на связи" else "нет данных",
                        if (state.connected) true else false,
                    )
                    Text(
                        "Режим: $modeLabel",
                        fontSize = 11.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Text(
                        "Плата: ${Settings.localHost}",
                        fontSize = 11.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    if (state.fw.isNotEmpty()) {
                        Text(
                            "Прошивка: ${state.fw}",
                            fontSize = 11.sp,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    if (state.uptime > 0) {
                        Text(
                            "Работает: ${state.uptime / 60} мин",
                            fontSize = 11.sp,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
        }
        item { Spacer(Modifier.height(4.dp)) }
    }
    if (showAbout) {
        AboutDialog(state, onDismiss = { showAbout = false })
    }
}

@Composable
private fun StatusRow(label: String, value: String, ok: Boolean?) {
    val dot = when (ok) {
        true -> online
        false -> MaterialTheme.colorScheme.error
        null -> MaterialTheme.colorScheme.onSurfaceVariant
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(8.dp).background(dot, CircleShape))
        Spacer(Modifier.width(8.dp))
        Text(label, fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurface)
        Spacer(Modifier.weight(1f))
        Text(value, fontSize = 12.sp, color = dot)
    }
}

@Composable
private fun AboutDialog(state: LeafState, onDismiss: () -> Unit) {
    Dialog(onDismissRequest = onDismiss) {
        Surface(
            shape = RoundedCornerShape(18.dp),
            color = MaterialTheme.colorScheme.surface,
        ) {
            Column(
                Modifier
                    .heightIn(max = 620.dp)
                    .padding(20.dp)
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text("Leaf Control", style = MaterialTheme.typography.headlineSmall)
                Text(
                    "Версия ${BuildConfig.VERSION_NAME}",
                    fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(4.dp))
                Text("Возможности", style = MaterialTheme.typography.titleMedium)
                Text(
                    "• Гараж: заряд, запас хода, свет, двери, климат\n" +
                        "• Дистанция: климат с температурой и таймером, зарядка, замки, свет, сигнал\n" +
                        "• Состояние: заряд, мощность, напряжение, ток, температуры, SOH, пробег, VIN\n" +
                        "• История поездок и зарядок, стоимость по тарифу, экспорт CSV и очистка\n" +
                        "• Уведомления: зарядка, низкий заряд, офлайн, завершение поездки\n" +
                        "• Офлайн-режим: последние данные и история доступны без интернета\n" +
                        "• Локальная плата или MQTT, автоматический поиск платы в сети\n" +
                        "• Автообновление приложения из GitHub\n" +
                        "• Ярлыки: открыть/закрыть двери, климат вкл/выкл\n" +
                        "• Светлая и тёмная тема, как на телефоне\n" +
                        "• Настройки платы и OTA отдельными ссылками",
                    fontSize = 13.sp,
                    lineHeight = 20.sp,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    "Плата: ${state.board.ifEmpty { "—" }}\n" +
                        "Прошивка: ${state.fw.ifEmpty { "—" }}",
                    fontSize = 11.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                OutlinedButton(onClick = onDismiss, modifier = Modifier.fillMaxWidth()) {
                    Text("Закрыть")
                }
            }
        }
    }
}

@Composable
private fun MoreRow(icon: ImageVector, title: String, subtitle: String, onClick: () -> Unit) {
    val interaction = remember { MutableInteractionSource() }
    Surface(
        shape = RoundedCornerShape(12.dp),
        color = MaterialTheme.colorScheme.surface,
        modifier = Modifier.fillMaxWidth().pressable(interaction) { onClick() },
    ) {
        Row(Modifier.padding(14.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(icon, null, tint = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(title, fontSize = 14.sp, color = MaterialTheme.colorScheme.onSurface)
                Text(subtitle, fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Icon(Icons.Outlined.ChevronRight, null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

private fun pct(v: Double): String = if (v >= 0) "${v.toInt()}%" else "—"

private fun fmt(v: Double, digits: Int): String =
    if (v.isFinite()) String.format(Locale.US, "%.${digits}f", v) else "—"

private fun temp(v: Double): String = if (v > -50) "${fmt(v, 1)} °C" else "—"

private fun chargerRu(s: LeafState): String = when {
    s.charging -> "Заряжается"
    s.charger == "done" -> "Завершено"
    s.charger == "stopped" -> "Прервано"
    s.charger == "wait" -> "Ожидание"
    else -> "Ожидание"
}

private fun ago(ms: Long): String {
    if (ms <= 0) return "—"
    val sec = (System.currentTimeMillis() - ms) / 1000
    return when {
        sec < 60 -> "$sec с"
        sec < 3600 -> "${sec / 60} мин"
        else -> "${sec / 3600} ч"
    }
}

private fun exportTrips(vm: LeafViewModel, trips: List<Trip>) {
    val head = listOf("начало", "конец", "минут", "км", "кВт·ч", "кВт·ч/100км", "заряд до", "заряд после", "стоимость")
    val rows = trips.filter { it.endMs > 0 }.sortedBy { it.startMs }.map { t ->
        listOf(
            dateFmt.format(Date(t.startMs)),
            dateFmt.format(Date(t.endMs)),
            ((t.endMs - t.startMs) / 60000).toString(),
            fmt(t.distKm, 2),
            fmt(t.energyWh / 1000.0, 2),
            LeafRepo.tripPer100(t)?.let { fmt(it, 1) } ?: "",
            t.startSoc?.toString() ?: "",
            t.endSoc?.toString() ?: "",
            if (LeafRepo.cost(t) > 0.5) fmt(LeafRepo.cost(t), 0) else "",
        )
    }
    val text = "\ufeff" + (listOf(head) + rows).joinToString("\r\n") { it.joinToString(";") }
    val ok = CsvWriter.save(vm.getApplication(), "leaf-trips.csv", text)
    vm.message.value = if (ok) "CSV сохранён в Загрузки" else "Не удалось сохранить CSV"
}

private fun exportCharges(vm: LeafViewModel, charges: List<Charge>) {
    val head = listOf("начало", "конец", "минут", "кВт·ч", "заряд до", "заряд после", "стоимость")
    val rows = charges.filter { it.endMs > 0 }.sortedBy { it.startMs }.map { c ->
        listOf(
            dateFmt.format(Date(c.startMs)),
            dateFmt.format(Date(c.endMs)),
            ((c.endMs - c.startMs) / 60000).toString(),
            fmt(c.energyWh / 1000.0, 2),
            c.startSoc?.toString() ?: "",
            c.endSoc?.toString() ?: "",
            if (LeafRepo.cost(c) > 0.5) fmt(LeafRepo.cost(c), 0) else "",
        )
    }
    val text = "\ufeff" + (listOf(head) + rows).joinToString("\r\n") { it.joinToString(";") }
    val ok = CsvWriter.save(vm.getApplication(), "leaf-charges.csv", text)
    vm.message.value = if (ok) "CSV сохранён в Загрузки" else "Не удалось сохранить CSV"
}

private fun openBoard(ctx: Context, path: String) {
    com.leafcontrol.app.net.LocalApi.ensureLocalHost(ctx)
    val url = "http://${Settings.localHost}:${Settings.localPort}$path"
    try {
        ctx.startActivity(
            Intent(Intent.ACTION_VIEW, Uri.parse(url))
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        )
    } catch (_: Exception) {
        Toast.makeText(ctx, "Не удалось открыть страницу платы", Toast.LENGTH_SHORT).show()
    }
}

/** Press feedback: ripple + slight scale + a light haptic tick. */
@Composable
private fun Modifier.pressable(
    source: MutableInteractionSource,
    onClick: () -> Unit,
): Modifier {
    val pressed by source.collectIsPressedAsState()
    val scale by animateFloatAsState(
        targetValue = if (pressed) 1.03f else 1f,
        animationSpec = spring(stiffness = Spring.StiffnessHigh),
        label = "press",
    )
    val haptic = LocalHapticFeedback.current
    return this
        .graphicsLayer {
            scaleX = scale
            scaleY = scale
        }
        .clickable(
            interactionSource = source,
            indication = LocalIndication.current,
            onClick = {
                haptic.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                onClick()
            },
        )
}

/**
 * Command buttons require a 1.5 s hold. A short tap does nothing; the ring
 * fills while holding, so accidental touches cannot lock the car or start a
 * charge. The same ring can show the remaining climate timer when idle.
 */
@Composable
private fun HoldAction(
    onTrigger: () -> Unit,
    modifier: Modifier = Modifier,
    ring: Boolean = true,
    idleProgress: Float = 0f,
    idleRingColor: Color = Color.Unspecified,
    content: @Composable (Float, Boolean) -> Unit,
) {
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    var progress by remember { mutableFloatStateOf(0f) }
    var fired by remember { mutableStateOf(false) }
    val haptic = LocalHapticFeedback.current
    val ringColor = MaterialTheme.colorScheme.primary

    LaunchedEffect(pressed) {
        if (!pressed) {
            progress = 0f
            fired = false
            return@LaunchedEffect
        }
        haptic.performHapticFeedback(HapticFeedbackType.TextHandleMove)
        val started = System.currentTimeMillis()
        while (true) {
            val p = ((System.currentTimeMillis() - started) / 1500f).coerceIn(0f, 1f)
            progress = p
            if (p >= 1f) {
                if (!fired) {
                    fired = true
                    haptic.performHapticFeedback(HapticFeedbackType.LongPress)
                    onTrigger()
                }
                break
            }
            delay(16)
        }
    }

    val scale by animateFloatAsState(
        targetValue = if (pressed) 1.1f else 1f,
        animationSpec = spring(stiffness = Spring.StiffnessMediumLow),
        label = "holdScale",
    )
    val shownProgress = if (pressed) progress else idleProgress
    // While holding, the ring is the hold progress in the theme primary; when
    // idle it shows the climate timer, optionally in a warning color when the
    // run is almost over.
    val shownColor = if (pressed || idleRingColor == Color.Unspecified) {
        ringColor
    } else {
        idleRingColor
    }
    Box(
        modifier
            .graphicsLayer {
                scaleX = scale
                scaleY = scale
            }
            .drawWithContent {
                drawContent()
                if (ring && shownProgress > 0f) {
                    val stroke = 3.dp.toPx()
                    drawArc(
                        color = shownColor,
                        startAngle = -90f,
                        sweepAngle = 360f * shownProgress.coerceIn(0f, 1f),
                        useCenter = false,
                        style = Stroke(stroke),
                        topLeft = Offset(stroke / 2f, stroke / 2f),
                        size = Size(size.width - stroke, size.height - stroke),
                    )
                }
            }
            .clickable(
                interactionSource = interaction,
                indication = null,
                onClick = {},
            )
    ) {
        content(progress, pressed)
    }
}
