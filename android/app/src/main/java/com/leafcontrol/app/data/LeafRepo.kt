package com.leafcontrol.app.data

import android.content.Context
import com.leafcontrol.app.net.LocalApi
import com.leafcontrol.app.net.MqttWssClient
import com.leafcontrol.app.net.ServerApi
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.withContext
import java.time.Instant
import java.time.ZoneId
import kotlin.math.abs

object LeafRepo {
    val state = MutableStateFlow(LeafState())
    val trips = MutableStateFlow<List<Trip>>(emptyList())
    val charges = MutableStateFlow<List<Charge>>(emptyList())
    val monitorText = MutableStateFlow("Мониторинг выключен")
    val mqttConnected = MutableStateFlow(false)

    /** Result of the last command attempt; consumed once by the UI. */
    val commandResult = MutableSharedFlow<String>(extraBufferCapacity = 4)

    /**
     * Duration the climate was last armed with, minutes. Lives outside the
     * composition so the countdown ring keeps its scale across screen
     * navigation and activity recreation (a composition-local total was
     * re-derived from the CURRENT remaining and the ring jumped back to
     * full whenever the user left the screen).
     */
    @Volatile
    var climateTotalMin: Int = 0

    @Volatile
    var mqtt: MqttWssClient? = null

    private var appContext: Context? = null
    private var lastCacheMs = 0L

    // Retained topic metric/s/v3/connected is the board's will. Use it so
    // retained metrics are not presented as live after the board went offline.
    @Volatile
    private var boardOnline = true

    fun init(context: Context) {
        appContext = context.applicationContext
        StateCache.load(context)?.let { cached ->
            if (state.value.lastUpdateMs == 0L) state.value = cached
        }
    }

    private fun cache(s: LeafState) {
        val ctx = appContext ?: return
        val now = System.currentTimeMillis()
        if (now - lastCacheMs < 4000L) return
        lastCacheMs = now
        StateCache.save(ctx, s)
    }

    fun setLocal(s: LeafState) {
        boardOnline = true
        state.value = s
        cache(s)
    }

    fun onMqttConnected() {
        boardOnline = true
        mqttConnected.value = true
        state.update { it.copy(connected = true, transport = "mqtt") }
    }

    fun setMqttConnected(ok: Boolean) {
        mqttConnected.value = ok
    }

    fun setDisconnected(transport: String) {
        if (transport == "mqtt") mqttConnected.value = false
        state.update { it.copy(connected = false, transport = transport) }
        cache(state.value)
    }

    /** Parse one ovms/<user>/<vehicle>/metric/... message. */
    fun onMqttMetric(topic: String, payload: String) {
        val base = "ovms/${Settings.mqttUser}/${Settings.vehicleId.ifEmpty { "leaf" }}/metric/"
        if (!topic.startsWith(base)) return
        val m = topic.removePrefix(base)
        val num = payload.toDoubleOrNull()
        val now = System.currentTimeMillis()
        if (m == "s/v3/connected") {
            boardOnline = payload == "yes"
            state.update { it.copy(connected = boardOnline, transport = "mqtt") }
            return
        }
        state.update { s ->
            val n = when (m) {
                "v/b/soc" -> s.copy(soc = num ?: s.soc)
                "v/b/range/instrument" -> s.copy(range = num?.toInt() ?: s.range)
                "v/b/temp" -> s.copy(batteryTemp = num ?: s.batteryTemp)
                "v/b/soh" -> s.copy(soh = num?.toInt() ?: s.soh)
                "v/b/gids" -> s.copy(gids = num?.toInt() ?: s.gids)
                "v/b/voltage" -> s.copy(voltage = num ?: s.voltage)
                "v/b/current" -> s.copy(current = num ?: s.current)
                "v/b/power" -> s.copy(power = num ?: s.power)
                "v/b/celldelta" -> s.copy(cellDelta = num?.toInt() ?: s.cellDelta)
                "v/e/cabintemp" -> s.copy(cabin = num ?: s.cabin)
                "v/e/ambient" -> s.copy(ambient = num ?: s.ambient)
                "v/e/setpoint" -> s.copy(setpoint = num ?: s.setpoint)
                "v/e/fan" -> s.copy(fan = num?.toInt() ?: s.fan)
                "v/e/heat" -> s.copy(heat = payload == "yes")
                "v/e/cool" -> s.copy(cool = payload == "yes")
                "v/e/vent" -> s.copy(vent = num?.toInt() ?: s.vent)
                "v/e/hvac/timer" -> s.copy(hvacTimer = num?.toInt() ?: s.hvacTimer)
                "v/c/charging" -> s.copy(charging = payload == "yes")
                "v/c/state" -> s.copy(charger = payload)
                "v/e/hvac" -> s.copy(hvac = payload == "yes")
                "v/e/locked" -> s.copy(locked = if (payload == "yes") 1 else 0)
                "v/e/on" -> s.copy(on = if (payload == "yes") 1 else 0)
                "v/e/headlights" -> s.copy(lights = payload == "yes")
                "v/p/odometer" -> s.copy(odometer = num?.toInt() ?: s.odometer)
                "v/vin" -> s.copy(vin = payload)
                else -> s
            }
            n.copy(lastUpdateMs = now, connected = boardOnline, transport = "mqtt")
        }
        cache(state.value)
    }

    suspend fun refreshHistory(context: Context) {
        val server = Settings.mode == "server"
        val local = if (server) ServerApi.status(context) else LocalApi.status(context)
        if (local != null) {
            setLocal(local)
            val t = if (server) ServerApi.trips(context) else LocalApi.trips(context)
            val c = if (server) ServerApi.charges(context) else LocalApi.charges(context)
            if (t.isNotEmpty() || c.isNotEmpty()) {
                withContext(Dispatchers.IO) {
                    t.forEach { HistoryDb.upsertTrip(it) }
                    c.forEach { HistoryDb.upsertCharge(it) }
                }
            }
        }
        loadHistory()
    }

    suspend fun loadHistory() {
        val t = withContext(Dispatchers.IO) { HistoryDb.trips() }
        val c = withContext(Dispatchers.IO) { HistoryDb.charges() }
        trips.value = t
        charges.value = c
    }

    suspend fun commandLocal(context: Context, path: String): Boolean =
        LocalApi.command(context, path)

    fun commandRemote(payload: String): Boolean {
        val c = mqtt ?: return false
        return c.publishCommand(payload)
    }

    fun recordCost(whDay: Double, whNight: Double, energyWh: Double): Double {
        val day = state.value.tariffDay.takeIf { it > 0.0 } ?: Settings.tariffDay
        val night = state.value.tariffNight.takeIf { it > 0.0 } ?: Settings.tariffNight
        if (day <= 0.0 && night <= 0.0) return 0.0
        val d = if (whDay > 0.0 || whNight > 0.0) whDay else energyWh
        val n = if (whDay > 0.0 || whNight > 0.0) whNight else 0.0
        return d / 1000.0 * day + n / 1000.0 * night
    }

    fun cost(t: Trip): Double = recordCost(t.whDay, t.whNight, t.energyWh)
    fun cost(c: Charge): Double = recordCost(c.whDay, c.whNight, c.energyWh)

    fun isNight(epochMs: Long): Boolean {
        val h = Instant.ofEpochMilli(epochMs).atZone(ZoneId.systemDefault()).hour
        val from = state.value.nightFrom.takeIf { it in 0..23 } ?: Settings.nightFrom
        val to = state.value.nightTo.takeIf { it in 0..23 } ?: Settings.nightTo
        if (from == to) return false
        return if (from < to) h >= from && h < to else h >= from || h < to
    }

    fun tripPer100(t: Trip): Double? =
        if (t.distKm > 0.5) t.energyWh / 1000.0 / t.distKm * 100.0 else null

    fun fmtDuration(ms: Long): String {
        val sec = abs(ms) / 1000
        val h = sec / 3600
        val m = (sec % 3600) / 60
        return when {
            h > 0 -> "$h ч $m мин"
            m > 0 -> "$m мин"
            else -> "$sec с"
        }
    }
}
