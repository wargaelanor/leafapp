package com.leafcontrol.app.data

import android.content.Context
import com.leafcontrol.app.net.LocalApi
import com.leafcontrol.app.net.MqttWssClient
import kotlinx.coroutines.Dispatchers
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

    @Volatile
    var mqtt: MqttWssClient? = null

    // Retained topic metric/s/v3/connected is the board's will. Use it so
    // retained metrics are not presented as live after the board went offline.
    @Volatile
    private var boardOnline = true

    fun setLocal(s: LeafState) {
        boardOnline = true
        state.value = s
    }

    fun onMqttConnected() {
        boardOnline = true
        state.update { it.copy(connected = true, transport = "mqtt") }
    }

    fun setDisconnected(transport: String) {
        state.update { it.copy(connected = false, transport = transport) }
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
                "v/b/soc/instrument" -> s.copy(soc = num ?: s.soc)
                "v/b/range/instrument" -> s.copy(range = num?.toInt() ?: s.range)
                "v/b/temp" -> s.copy(batteryTemp = num ?: s.batteryTemp)
                "v/b/soh" -> s.copy(soh = num?.toInt() ?: s.soh)
                "v/b/voltage" -> s.copy(voltage = num ?: s.voltage)
                "v/b/current" -> s.copy(current = num ?: s.current)
                "v/b/power" -> s.copy(power = num ?: s.power)
                "v/e/cabintemp" -> s.copy(cabin = num ?: s.cabin)
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
    }

    suspend fun refreshHistory(context: Context) {
        val local = LocalApi.status(context)
        if (local != null) {
            setLocal(local)
            val t = LocalApi.trips(context)
            val c = LocalApi.charges(context)
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
        c.publishCommand(payload)
        return true
    }

    fun recordCost(whDay: Double, whNight: Double, energyWh: Double): Double {
        val day = Settings.tariffDay
        val night = Settings.tariffNight
        if (day <= 0.0 && night <= 0.0) return 0.0
        val d = if (whDay > 0.0 || whNight > 0.0) whDay else energyWh
        val n = if (whDay > 0.0 || whNight > 0.0) whNight else 0.0
        return d / 1000.0 * day + n / 1000.0 * night
    }

    fun cost(t: Trip): Double = recordCost(t.whDay, t.whNight, t.energyWh)
    fun cost(c: Charge): Double = recordCost(c.whDay, c.whNight, c.energyWh)

    fun isNight(epochMs: Long): Boolean {
        val h = Instant.ofEpochMilli(epochMs).atZone(ZoneId.systemDefault()).hour
        val from = Settings.nightFrom
        val to = Settings.nightTo
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
