package com.leafcontrol.app.ui

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.AcUnit
import androidx.compose.material.icons.outlined.BatteryChargingFull
import androidx.compose.material.icons.outlined.Bolt
import androidx.compose.material.icons.outlined.Lightbulb
import androidx.compose.material.icons.outlined.Lock
import androidx.compose.material.icons.outlined.LockOpen
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material.icons.outlined.WifiOff
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.leafcontrol.app.R
import com.leafcontrol.app.data.LeafState
import com.leafcontrol.app.data.Settings
import kotlinx.coroutines.delay
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * «Новый» дизайн: главная-дашборд по макету редизайна.
 *
 * Всё, что здесь нарисовано, читает тот же [LeafState], что и классический
 * интерфейс, и отправляет команды через тот же `LeafViewModel.command`, так что
 * поведение приложения не меняется — меняется только оформление.
 * Элемент выбирается в Настройках («Дизайн интерфейса»).
 */

private val ndGreen = Color(0xFF34C759)
private val ndBlue = Color(0xFF2F7CF6)
private val ndOrange = Color(0xFFFF9A3D)
private val ndGrey = Color(0xFF9AA4B2)

private val ndTime = SimpleDateFormat("HH:mm", Locale("ru"))

@Composable
fun NewDashboard(
    state: LeafState,
    onCommand: (String, Double, Int) -> Unit,
    onRefresh: () -> Unit,
) {
    val stale = !state.connected || !state.fresh()
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 6.dp, bottom = 18.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (!state.connected) {
            item { OfflineBanner() }
        }
        item { HeroCard(state, onRefresh = onRefresh, stale = stale) }
        item { SocCardNew(state, stale = stale) }
        item { QuickActionGrid(state, onCommand) }
        item { MetricsGrid(state, stale = stale) }
        item { Spacer(Modifier.height(2.dp)) }
    }
}

// ---------------------------------------------------------------- hero card

private enum class HeroMode(val label: String, val color: Color) {
    Idle("Покой", ndGrey),
    Charging("Зарядка", ndGreen),
    Moving("Движение", ndBlue),
    Climate("Климат", ndOrange),
}

private fun heroMode(state: LeafState, fresh: Boolean): HeroMode = when {
    !state.connected && state.lastUpdateMs == 0L -> HeroMode.Idle
    state.charging -> HeroMode.Charging
    state.on == 1 && fresh -> HeroMode.Moving
    state.hvac && fresh -> HeroMode.Climate
    else -> HeroMode.Idle
}

@Composable
private fun HeroCard(state: LeafState, onRefresh: () -> Unit, stale: Boolean) {
    val mode = heroMode(state, !stale)
    Surface(
        shape = RoundedCornerShape(20.dp),
        color = MaterialTheme.colorScheme.surface,
        tonalElevation = 1.dp,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(horizontal = 14.dp, vertical = 12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                StatusBadge(mode)
                Spacer(Modifier.weight(1f))
                Text(
                    "NISSAN LEAF",
                    fontSize = 11.sp,
                    letterSpacing = 1.4.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                IconButton(onClick = onRefresh, modifier = Modifier.size(36.dp)) {
                    Icon(
                        Icons.Outlined.Refresh,
                        contentDescription = "Обновить",
                        tint = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.size(20.dp),
                    )
                }
            }
            Spacer(Modifier.height(4.dp))
            CarHero(state, mode)
        }
    }
}

@Composable
private fun StatusBadge(mode: HeroMode) {
    val pulse = if (mode == HeroMode.Charging) {
        val t = rememberInfiniteTransition(label = "badgePulse")
        t.animateFloat(
            initialValue = 0.35f,
            targetValue = 1f,
            animationSpec = infiniteRepeatable(tween(1100, easing = LinearEasing), RepeatMode.Reverse),
            label = "badgeAlpha",
        ).value
    } else {
        1f
    }
    Surface(
        shape = RoundedCornerShape(50),
        color = mode.color.copy(alpha = 0.14f),
        border = BorderStroke(1.dp, mode.color.copy(alpha = 0.35f)),
    ) {
        Row(
            Modifier.padding(horizontal = 10.dp, vertical = 5.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                Modifier
                    .size(8.dp)
                    .graphicsLayer { alpha = pulse }
                    .background(mode.color, CircleShape)
            )
            Spacer(Modifier.width(7.dp))
            Text(
                mode.label,
                fontSize = 12.sp,
                fontWeight = FontWeight.Medium,
                color = if (mode == HeroMode.Idle) MaterialTheme.colorScheme.onSurfaceVariant else mode.color,
            )
        }
    }
}

/**
 * Машина из присланных слоёв: base задаёт размер, остальные картинки лежат на
 * той же канве и растянуты на весь размер — так они совпадают пиксель в пиксель.
 */
@Composable
private fun CarHero(state: LeafState, mode: HeroMode) {
    var wheelFrame by remember { mutableIntStateOf(0) }
    val moving = mode == HeroMode.Moving
    LaunchedEffect(moving) {
        if (!moving) {
            wheelFrame = 0
            return@LaunchedEffect
        }
        // Кадры 4 → 3 → 2 → 1 каждые ~90 мс: колесо «крутится».
        while (true) {
            delay(90)
            wheelFrame = (wheelFrame + 1) % 4
        }
    }
    // Тёплое свечение под машиной, когда работает климат.
    val glow = if (state.hvac) {
        val t = rememberInfiniteTransition(label = "hvacGlow")
        t.animateFloat(
            initialValue = 0.22f,
            targetValue = 0.55f,
            animationSpec = infiniteRepeatable(tween(1600, easing = LinearEasing), RepeatMode.Reverse),
            label = "glowAlpha",
        ).value
    } else {
        0f
    }
    Box(
        Modifier
            .fillMaxWidth()
            .aspectRatio(660f / 367f),
    ) {
        if (glow > 0f) {
            Box(
                Modifier
                    .align(Alignment.BottomCenter)
                    .fillMaxWidth(0.86f)
                    .height(72.dp)
                    .padding(bottom = 4.dp)
                    .graphicsLayer { alpha = glow }
                    .background(
                        Brush.radialGradient(
                            colors = listOf(ndOrange.copy(alpha = 0.85f), Color.Transparent),
                        ),
                        shape = CircleShape,
                    )
            )
        }
        Image(
            painterResource(R.drawable.car_leaf),
            contentDescription = "Nissan Leaf",
            contentScale = ContentScale.Fit,
            modifier = Modifier.fillMaxSize(),
        )
        // Фары: в LeafState есть только булев lights (его включает вспышка).
        if (state.lights && state.lastUpdateMs > 0) {
            Image(
                painterResource(R.drawable.car_lights),
                contentDescription = null,
                contentScale = ContentScale.Fit,
                modifier = Modifier.fillMaxSize(),
            )
        }
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
            val frames = intArrayOf(
                R.drawable.car_wheel4,
                R.drawable.car_wheel3,
                R.drawable.car_wheel2,
                R.drawable.car_wheel1,
            )
            Image(
                painterResource(frames[wheelFrame.coerceIn(0, 3)]),
                contentDescription = null,
                contentScale = ContentScale.Fit,
                modifier = Modifier.fillMaxSize(),
            )
        }
    }
}

// ------------------------------------------------------------------ SOC card

@Composable
private fun SocCardNew(state: LeafState, stale: Boolean) {
    Surface(
        shape = RoundedCornerShape(20.dp),
        color = MaterialTheme.colorScheme.surface,
        tonalElevation = 1.dp,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(14.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    Modifier
                        .width(22.dp)
                        .height(2.dp)
                        .background(MaterialTheme.colorScheme.primary, RoundedCornerShape(1.dp))
                )
                Spacer(Modifier.width(8.dp))
                Text("Уровень заряда", fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Spacer(Modifier.weight(1f))
                if (state.charger == "done") Tag("Зарядка завершена", ndGreen)
                if (state.charger == "wait") Tag("Ожидание старта", ndGrey)
            }
            Spacer(Modifier.height(10.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                SocRing(soc = state.soc, dim = stale)
                Spacer(Modifier.width(16.dp))
                Column(Modifier.weight(1f)) {
                    val range = state.displayRange
                    Text(
                        if (range != null) "≈ $range км" else "— км",
                        fontSize = 26.sp,
                        fontWeight = FontWeight.Light,
                        color = MaterialTheme.colorScheme.onSurface,
                        modifier = Modifier.alpha(if (stale) 0.5f else 1f),
                    )
                    Text(
                        "запас хода",
                        fontSize = 11.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.height(10.dp))
                    LegendRow("Зарядка", chargerRuNew(state))
                    LegendRow("Мощность", if (state.connected) "${ndFmt(state.power, 1)} кВт" else "—")
                }
            }
            Spacer(Modifier.height(12.dp))
            SocBar(state.soc)
            Spacer(Modifier.height(8.dp))
            Text(
                buildString {
                    if (state.lastUpdateMs > 0) {
                        append("Данные от ")
                        append(ndTime.format(Date(state.lastUpdateMs)))
                        append(" · обновлено ")
                        append(ndAgo(state.lastUpdateMs))
                        append(" назад")
                    } else {
                        append("Данных ещё нет")
                    }
                },
                fontSize = 11.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.alpha(if (stale) 0.6f else 1f),
            )
        }
    }
}

@Composable
private fun SocRing(soc: Double, dim: Boolean) {
    val hasSoc = soc >= 0
    val frac = if (hasSoc) (soc / 100.0).toFloat().coerceIn(0f, 1f) else 0f
    val track = MaterialTheme.colorScheme.surfaceVariant
    val from = if (soc >= 0 && soc <= 20) Color(0xFFFF6B6B) else Color(0xFF2F7CF6)
    val to = if (soc >= 0 && soc <= 20) Color(0xFFFFA26B) else Color(0xFF28D3C8)
    Box(
        Modifier.size(116.dp).alpha(if (dim) 0.55f else 1f),
        contentAlignment = Alignment.Center,
    ) {
        Canvas(Modifier.fillMaxSize()) {
            val stroke = 11.dp.toPx()
            val inset = stroke / 2f
            val arcSize = Size(size.width - stroke, size.height - stroke)
            val topLeft = Offset(inset, inset)
            drawArc(
                color = track,
                startAngle = 0f,
                sweepAngle = 360f,
                useCenter = false,
                topLeft = topLeft,
                size = arcSize,
                style = Stroke(width = stroke, cap = StrokeCap.Round),
            )
            if (frac > 0f) {
                val brush = Brush.sweepGradient(listOf(from, to, to))
                rotate(degrees = -90f) {
                    drawArc(
                        brush = brush,
                        startAngle = 0f,
                        sweepAngle = 360f * frac,
                        useCenter = false,
                        topLeft = topLeft,
                        size = arcSize,
                        style = Stroke(width = stroke, cap = StrokeCap.Round),
                    )
                }
            }
        }
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(
                if (hasSoc) "${soc.toInt()}%" else "—",
                fontSize = 27.sp,
                fontWeight = FontWeight.Medium,
                color = MaterialTheme.colorScheme.onSurface,
            )
            Text("SOC", fontSize = 9.sp, letterSpacing = 1.2.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun SocBar(soc: Double) {
    val fraction = (soc / 100.0).toFloat().coerceIn(0f, 1f)
    Box(
        Modifier
            .fillMaxWidth()
            .height(8.dp)
            .clip(RoundedCornerShape(4.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant)
    ) {
        Box(
            Modifier
                .fillMaxWidth(fraction)
                .fillMaxHeight()
                .clip(RoundedCornerShape(4.dp))
                .background(
                    Brush.horizontalGradient(
                        listOf(Color(0xFF2F7CF6), Color(0xFF28D3C8))
                    )
                )
        )
    }
}

@Composable
private fun LegendRow(label: String, value: String) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(label, fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.weight(1f))
        Text(value, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurface)
    }
}

@Composable
private fun Tag(text: String, color: Color) {
    Surface(
        shape = RoundedCornerShape(50),
        color = color.copy(alpha = 0.14f),
    ) {
        Text(
            text,
            fontSize = 10.sp,
            color = color,
            modifier = Modifier.padding(horizontal = 8.dp, vertical = 3.dp),
        )
    }
}

@Composable
private fun OfflineBanner() {
    Surface(
        shape = RoundedCornerShape(14.dp),
        color = MaterialTheme.colorScheme.error.copy(alpha = 0.12f),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Row(Modifier.padding(horizontal = 12.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(
                Icons.Outlined.WifiOff,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.error,
                modifier = Modifier.size(18.dp),
            )
            Spacer(Modifier.width(10.dp))
            Column {
                Text("Нет связи с машиной", fontSize = 13.sp, color = MaterialTheme.colorScheme.error)
                Text(
                    "Показаны последние известные данные",
                    fontSize = 11.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

// ------------------------------------------------------------ quick actions

@Composable
private fun QuickActionGrid(state: LeafState, onCommand: (String, Double, Int) -> Unit) {
    // Климат берёт длительность из настроек — как на экране «Дистанция».
    val minutes = Settings.climateMinutes
    val setpoint = if (state.setpoint in 16.0..30.0) state.setpoint else 22.0
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            ActionTile(
                icon = Icons.Outlined.AcUnit,
                title = "Климат",
                subtitle = if (state.hvac) "Вкл · $minutes мин" else "Выкл",
                active = state.hvac,
                accent = ndOrange,
                modifier = Modifier.weight(1f),
            ) { onCommand("hvac", setpoint, minutes) }
            ActionTile(
                icon = Icons.Outlined.BatteryChargingFull,
                title = "Зарядка",
                subtitle = if (state.charging) "Вкл" else "Выкл",
                active = state.charging,
                accent = ndGreen,
                modifier = Modifier.weight(1f),
            ) { onCommand(if (state.charging) "chargeoff" else "charge", 22.0, 15) }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            ActionTile(
                icon = if (state.locked == 1) Icons.Outlined.Lock else Icons.Outlined.LockOpen,
                title = "Замок",
                subtitle = when (state.locked) {
                    1 -> "Заперто"
                    0 -> "Открыто"
                    else -> "—"
                },
                active = state.locked == 1,
                accent = MaterialTheme.colorScheme.primary,
                modifier = Modifier.weight(1f),
            ) { onCommand(if (state.locked == 1) "unlock" else "lock", 22.0, 15) }
            ActionTile(
                icon = Icons.Outlined.Lightbulb,
                title = if (state.lights) "Свет погасить" else "Вспышка фар",
                subtitle = "Сигнал",
                active = state.lights,
                accent = Color(0xFFFFA726),
                modifier = Modifier.weight(1f),
            ) { onCommand("hornlights", 22.0, 15) }
        }
        Text(
            "Удерживайте 1,5 с — чтобы не нажать случайно",
            fontSize = 10.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
            modifier = Modifier.fillMaxWidth().padding(top = 2.dp),
        )
    }
}

@Composable
private fun ActionTile(
    icon: ImageVector,
    title: String,
    subtitle: String,
    active: Boolean,
    accent: Color,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    NdHoldAction(onTrigger = onClick, modifier = modifier.height(78.dp), accent = accent) { progress, pressed ->
        Surface(
            shape = RoundedCornerShape(16.dp),
            color = if (active) accent.copy(alpha = 0.10f) else MaterialTheme.colorScheme.surfaceVariant,
            border = if (active) BorderStroke(1.dp, accent.copy(alpha = 0.55f)) else null,
            modifier = Modifier.fillMaxSize(),
        ) {
            Box(Modifier.fillMaxSize()) {
                Row(
                    Modifier.fillMaxSize().padding(horizontal = 12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Surface(
                        shape = CircleShape,
                        color = if (active) accent.copy(alpha = 0.18f) else MaterialTheme.colorScheme.surface,
                        modifier = Modifier.size(38.dp),
                    ) {
                        Box(contentAlignment = Alignment.Center) {
                            Icon(
                                icon,
                                contentDescription = title,
                                tint = if (active) accent else MaterialTheme.colorScheme.onSurface,
                                modifier = Modifier.size(20.dp),
                            )
                        }
                    }
                    Spacer(Modifier.width(10.dp))
                    Column(Modifier.weight(1f)) {
                        Text(
                            title,
                            fontSize = 13.sp,
                            fontWeight = FontWeight.Medium,
                            color = MaterialTheme.colorScheme.onSurface,
                        )
                        Text(subtitle, fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
                if (pressed && progress > 0f) {
                    Box(
                        Modifier
                            .align(Alignment.BottomStart)
                            .fillMaxWidth(progress)
                            .height(3.dp)
                            .background(accent.copy(alpha = 0.65f))
                    )
                }
            }
        }
    }
}

// ---------------------------------------------------------------- metrics

@Composable
private fun MetricsGrid(state: LeafState, stale: Boolean) {
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            MetricTile("Салон", ndTemp(state.cabin), Modifier.weight(1f), stale)
            MetricTile("Улица", ndTemp(state.ambient), Modifier.weight(1f), stale)
        }
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            MetricTile("Батарея", ndTemp(state.batteryTemp), Modifier.weight(1f), stale)
            MetricTile(
                "Мощность",
                if (state.connected) "${ndFmt(state.power, 1)} кВт" else "—",
                Modifier.weight(1f),
                stale,
            )
        }
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            MetricTile(
                "Пробег",
                if (state.odometer > 0) "${state.odometer} км" else "—",
                Modifier.weight(1f),
                stale,
            )
            MetricTile(
                "Связь",
                when {
                    !state.connected -> "нет"
                    state.transport == "local" -> "плата"
                    state.transport == "server" -> "сервер"
                    else -> state.transport
                },
                Modifier.weight(1f),
                stale,
            )
        }
    }
}

@Composable
private fun MetricTile(label: String, value: String, modifier: Modifier = Modifier, stale: Boolean) {
    Surface(
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surface,
        tonalElevation = 1.dp,
        modifier = modifier,
    ) {
        Column(Modifier.padding(horizontal = 12.dp, vertical = 10.dp)) {
            Text(label, fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(3.dp))
            Text(
                value,
                fontSize = 17.sp,
                fontWeight = FontWeight.Medium,
                color = MaterialTheme.colorScheme.onSurface,
                modifier = Modifier.alpha(if (stale) 0.5f else 1f),
            )
        }
    }
}

// ------------------------------------------------------------------ helpers

private fun ndFmt(v: Double, digits: Int): String =
    if (v.isNaN() || v.isInfinite()) "—" else String.format(Locale("ru"), "%.${digits}f", v)

/** Датчики приходят как -99, когда плата их не знает: это не ноль. */
private fun ndTemp(v: Double): String = if (v > -50) "${ndFmt(v, 1)} °C" else "—"

private fun ndAgo(ms: Long, now: Long = System.currentTimeMillis()): String {
    if (ms <= 0) return "—"
    val s = ((now - ms) / 1000).coerceAtLeast(0)
    return when {
        s < 5 -> "только что"
        s < 60 -> "$s с"
        s < 3600 -> "${s / 60} мин"
        s < 86_400 -> "${s / 3600} ч"
        else -> "${s / 86_400} дн"
    }
}

private fun chargerRuNew(state: LeafState): String = when {
    !state.connected -> "нет связи"
    state.charging -> "идёт"
    state.charger == "done" -> "завершена"
    state.charger == "wait" -> "ожидание"
    state.charger == "off" || state.charger.isEmpty() -> "нет"
    else -> state.charger
}

/**
 * Копия механики HoldAction из MainScreen: команда срабатывает только после
 * удержания 1,5 с, а полоска показывает прогресс. Отдельная копия — потому что
 * оригинал приватный внутри MainScreen.kt.
 */
@Composable
private fun NdHoldAction(
    onTrigger: () -> Unit,
    modifier: Modifier = Modifier,
    accent: Color = Color.Unspecified,
    content: @Composable (Float, Boolean) -> Unit,
) {
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    var progress by remember { mutableFloatStateOf(0f) }
    var fired by remember { mutableStateOf(false) }
    val haptic = LocalHapticFeedback.current

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

    Box(
        modifier
            .drawWithContent {
                drawContent()
                // Тонкий контур-«заливка» вокруг плитки во время удержания.
                if (pressed && progress > 0f) {
                    val stroke = 2.dp.toPx()
                    val color = if (accent == Color.Unspecified) Color.White else accent
                    drawRoundRect(
                        color = color.copy(alpha = 0.75f),
                        topLeft = Offset(stroke / 2f, stroke / 2f),
                        size = Size(size.width - stroke, size.height - stroke),
                        cornerRadius = CornerRadius(16.dp.toPx()),
                        style = Stroke(width = stroke),
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
