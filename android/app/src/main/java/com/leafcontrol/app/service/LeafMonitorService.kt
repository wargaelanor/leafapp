package com.leafcontrol.app.service

import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationManagerCompat
import com.leafcontrol.app.Notifications
import com.leafcontrol.app.data.Charge
import com.leafcontrol.app.data.HistoryDb
import com.leafcontrol.app.data.LeafRepo
import com.leafcontrol.app.data.LeafState
import com.leafcontrol.app.data.Settings
import com.leafcontrol.app.data.Trip
import com.leafcontrol.app.net.LocalApi
import com.leafcontrol.app.net.MqttWssClient
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlin.math.abs

class LeafMonitorService : Service() {
    companion object {
        const val ACTION_START = "com.leafcontrol.app.START"
        const val ACTION_STOP = "com.leafcontrol.app.STOP"
        const val ACTION_COMMAND = "com.leafcontrol.app.COMMAND"
        const val EXTRA_PAYLOAD = "payload"
        const val EXTRA_PATH = "path"
        private const val MONITOR_ID = 1

        fun start(context: Context) {
            val i = Intent(context, LeafMonitorService::class.java).setAction(ACTION_START)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) context.startForegroundService(i)
            else context.startService(i)
        }

        fun stop(context: Context) {
            context.startService(Intent(context, LeafMonitorService::class.java).setAction(ACTION_STOP))
        }

        fun command(context: Context, path: String, payload: String) {
            val i = Intent(context, LeafMonitorService::class.java)
                .setAction(ACTION_COMMAND)
                .putExtra(EXTRA_PATH, path)
                .putExtra(EXTRA_PAYLOAD, payload)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) context.startForegroundService(i)
            else context.startService(i)
        }
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var monitorJob: Job? = null
    private var mqttClient: MqttWssClient? = null
    private var recorder = Recorder()
    private var prevCharging = false
    private var lowNotified = false
    private var offlineNotified = false
    private var startedForeground = false
    @Volatile private var monitoring = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        Notifications.createChannels(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopMonitor()
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
                return START_NOT_STICKY
            }
            ACTION_COMMAND -> {
                ensureForeground("Команда…")
                val path = intent.getStringExtra(EXTRA_PATH) ?: ""
                val payload = intent.getStringExtra(EXTRA_PAYLOAD) ?: ""
                sendCommand(path, payload)
                return START_STICKY
            }
            else -> {
                ensureForeground("Запуск…")
                if (Settings.monitorEnabled) startMonitor()
                else {
                    LeafRepo.monitorText.value = "Мониторинг выключен"
                    updateForeground("Мониторинг выключен")
                }
                return START_STICKY
            }
        }
    }

    private fun ensureForeground(text: String) {
        if (!startedForeground) {
            try {
                startForeground(MONITOR_ID, Notifications.monitor(this, text))
                startedForeground = true
            } catch (_: Exception) {
                stopSelf()
                return
            }
        } else {
            updateForeground(text)
        }
    }

    private fun updateForeground(text: String) {
        LeafRepo.monitorText.value = text
        try {
            NotificationManagerCompat.from(this).notify(MONITOR_ID, Notifications.monitor(this, text))
        } catch (_: SecurityException) {
        }
    }

    private fun startMonitor() {
        monitoring = true
        monitorJob?.cancel()
        monitorJob = scope.launch {
            val mode = Settings.mode
            when (mode) {
                "local" -> runLocal()
                "mqtt" -> runMqtt()
                else -> runAuto()
            }
        }
    }

    private suspend fun runAuto() {
        // Try local first; if the board is not on this Wi-Fi, fall back to MQTT.
        var failures = 0
        var syncedAt = 0L
        while (monitoring) {
            val s = LocalApi.status(this)
            if (s != null) {
                failures = 0
                onState(s)
                updateForeground("Локально · ${s.soc.takeIf { it >= 0 }?.toInt() ?: "—"}%")
                if (System.currentTimeMillis() - syncedAt > 300_000L) {
                    syncedAt = System.currentTimeMillis()
                    syncHistory()
                }
                delay(20_000)
            } else {
                failures++
                if (failures >= 3 && Settings.mqttUser.isNotEmpty()) {
                    runMqtt()
                    return
                }
                updateForeground("Нет связи с платой")
                LeafRepo.setDisconnected("local")
                delay(30_000)
            }
        }
    }

    private suspend fun runLocal() {
        updateForeground("Локальный режим")
        var syncedAt = 0L
        while (monitoring) {
            val s = LocalApi.status(this)
            if (s != null) {
                onState(s)
                updateForeground("Локально · ${s.soc.takeIf { it >= 0 }?.toInt() ?: "—"}% · ${s.voltage.toInt()} В")
                if (System.currentTimeMillis() - syncedAt > 300_000L) {
                    syncedAt = System.currentTimeMillis()
                    syncHistory()
                }
            } else {
                LeafRepo.setDisconnected("local")
                updateForeground("Нет связи с платой")
                handleState(LeafRepo.state.value)
            }
            delay(20_000)
        }
    }

    private suspend fun runMqtt() {
        if (Settings.mqttUser.isEmpty()) {
            updateForeground("MQTT не настроен")
            return
        }
        updateForeground("MQTT · подключение…")
        mqttClient?.stop()
        val client = MqttWssClient(
            host = Settings.mqttHost,
            port = Settings.mqttPort,
            path = Settings.mqttPath,
            user = Settings.mqttUser,
            pass = Settings.mqttPass,
            vehicleId = Settings.vehicleId,
            onConnected = { ok ->
                scope.launch {
                    if (ok) {
                        updateForeground("MQTT · подключено")
                        LeafRepo.monitorText.value = "MQTT · подключено"
                        LeafRepo.onMqttConnected()
                    } else {
                        updateForeground("MQTT · нет связи")
                        LeafRepo.setDisconnected("mqtt")
                    }
                }
            },
            onMetric = { topic, payload ->
                LeafRepo.onMqttMetric(topic, payload)
                val s = LeafRepo.state.value
                onState(s)
                recorder.sample(s, System.currentTimeMillis())
            },
        )
        mqttClient = client
        LeafRepo.mqtt = client
        client.start()
        while (monitoring) {
            delay(15_000)
            val s = LeafRepo.state.value
            recorder.sample(s, System.currentTimeMillis())
            handleState(s)
            if (s.connected) updateForeground("MQTT · ${s.soc.takeIf { it >= 0 }?.toInt() ?: "—"}% · ${s.power.toInt()} кВт")
        }
        client.stop()
        LeafRepo.mqtt = null
    }

    private fun sendCommand(path: String, payload: String) {
        scope.launch {
            if (Settings.mode == "mqtt" || (Settings.mode == "auto" && LeafRepo.mqtt != null)) {
                LeafRepo.commandRemote(payload)
            } else if (path.isNotEmpty()) {
                LeafRepo.commandLocal(this@LeafMonitorService, path)
            } else if (LeafRepo.mqtt != null) {
                LeafRepo.commandRemote(payload)
            }
        }
    }

    private suspend fun syncHistory() {
        val t = LocalApi.trips(this)
        val c = LocalApi.charges(this)
        if (t.isNotEmpty() || c.isNotEmpty()) {
            t.forEach { HistoryDb.upsertTrip(it) }
            c.forEach { HistoryDb.upsertCharge(it) }
        }
        LeafRepo.loadHistory()
    }

    private fun onState(s: LeafState) {
        handleState(s)
        recorder.sample(s, System.currentTimeMillis())
    }

    private fun handleState(s: LeafState) {
        val now = System.currentTimeMillis()
        if (s.connected) {
            offlineNotified = false
            if (prevCharging != s.charging) {
                if (s.charging && Settings.notifyChargeStart) {
                    Notifications.event(this, "Зарядка началась", "Leaf подключён к зарядке${socText(s)}")
                } else if (!s.charging && prevCharging && Settings.notifyChargeEnd) {
                    Notifications.event(this, "Зарядка завершена", "Батарея${socText(s)}")
                }
            }
            prevCharging = s.charging
            if (s.soc in 0.0..Settings.lowSocThreshold.toDouble() && !s.charging) {
                if (!lowNotified && Settings.notifyLowSoc) {
                    lowNotified = true
                    Notifications.event(this, "Низкий заряд", "Осталось ${s.soc.toInt()}%${rangeText(s)}")
                }
            } else if (s.charging || s.soc > Settings.lowSocThreshold + 5) {
                lowNotified = false
            }
        } else {
            if (!offlineNotified && Settings.notifyOffline && s.lastUpdateMs > 0 &&
                now - s.lastUpdateMs > 10 * 60_000L
            ) {
                offlineNotified = true
                Notifications.event(this, "Плата не в сети", "Нет данных с автомобиля больше 10 минут")
            }
        }
    }

    private fun socText(s: LeafState): String =
        if (s.soc >= 0) " (${s.soc.toInt()}%)" else ""

    private fun rangeText(s: LeafState): String =
        if (s.range > 0) ", запас ${s.range} км" else ""

    private fun stopMonitor() {
        monitoring = false
        monitorJob?.cancel()
        mqttClient?.stop()
        LeafRepo.mqtt = null
        mqttClient = null
        LeafRepo.setDisconnected("offline")
        LeafRepo.monitorText.value = "Мониторинг остановлен"
    }

    override fun onDestroy() {
        stopMonitor()
        scope.cancel()
        super.onDestroy()
    }

    /** Records trips/charges from MQTT telemetry when the board is remote. */
    private inner class Recorder {
        private var chargeStart = 0L
        private var chargeSocStart: Int? = null
        private var chargeWh = 0.0
        private var chargeWhDay = 0.0
        private var chargeWhNight = 0.0

        private var tripStart = 0L
        private var tripSocStart: Int? = null
        private var tripOdoStart = 0
        private var tripDist = 0.0
        private var tripWh = 0.0
        private var tripWhDay = 0.0
        private var tripWhNight = 0.0
        private var lastMoveMs = 0L
        private var lastOdo = 0
        private var lastSampleMs = 0L

        fun sample(s: LeafState, now: Long) {
            if (!s.connected) return
            // The board writes its own history in local mode; only record from
            // MQTT telemetry, otherwise the list would contain duplicates.
            if (s.transport != "mqtt") {
                lastSampleMs = now
                if (s.odometer > 0) lastOdo = s.odometer
                return
            }
            val dt = if (lastSampleMs > 0) (now - lastSampleMs) / 3_600_000.0 else 0.0
            lastSampleMs = now
            val wh = if (dt in 0.0001..0.02) abs(s.power) * dt else 0.0

            // ---- charge ----
            if (s.charging && chargeStart == 0L) {
                chargeStart = now
                chargeSocStart = s.soc.takeIf { it >= 0 }?.toInt()
                chargeWh = 0.0
                chargeWhDay = 0.0
                chargeWhNight = 0.0
            } else if (s.charging && chargeStart > 0) {
                chargeWh += wh
                if (LeafRepo.isNight(now)) chargeWhNight += wh else chargeWhDay += wh
            } else if (!s.charging && chargeStart > 0) {
                val dur = now - chargeStart
                if (dur >= 60_000L || chargeWh >= 20.0) {
                    HistoryDb.upsertCharge(
                        Charge(
                            startMs = chargeStart,
                            endMs = now,
                            energyWh = chargeWh,
                            whDay = chargeWhDay,
                            whNight = chargeWhNight,
                            startSoc = chargeSocStart,
                            endSoc = s.soc.takeIf { it >= 0 }?.toInt(),
                            source = "phone",
                            reason = "stopped",
                        )
                    )
                    scope.launch { LeafRepo.loadHistory() }
                }
                chargeStart = 0L
            }

            // ---- trip ----
            val moved = s.odometer > 0 && lastOdo > 0 && s.odometer > lastOdo
            if (moved) lastMoveMs = now
            if (tripStart == 0L && moved) {
                tripStart = now
                tripSocStart = s.soc.takeIf { it >= 0 }?.toInt()
                tripOdoStart = lastOdo
                tripDist = 0.0
                tripWh = 0.0
                tripWhDay = 0.0
                tripWhNight = 0.0
                lastMoveMs = now
            } else if (tripStart > 0) {
                if (moved) {
                    val delta = s.odometer - lastOdo
                    if (delta > 0) tripDist += delta.toDouble()
                }
                tripWh += wh
                if (LeafRepo.isNight(now)) tripWhNight += wh else tripWhDay += wh
                val end = (s.on == 0 && now - lastMoveMs > 180_000L) || now - lastMoveMs > 900_000L
                if (end) {
                    if (tripDist >= 0.05 || tripWh >= 10.0) {
                        HistoryDb.upsertTrip(
                            Trip(
                                startMs = tripStart,
                                endMs = now,
                                distKm = tripDist,
                                energyWh = tripWh,
                                whDay = tripWhDay,
                                whNight = tripWhNight,
                                startSoc = tripSocStart,
                                endSoc = s.soc.takeIf { it >= 0 }?.toInt(),
                                source = "phone",
                                reason = if (s.on == 0) "off" else "idle",
                            )
                        )
                        scope.launch { LeafRepo.loadHistory() }
                        if (Settings.notifyTripEnd) {
                            Notifications.event(
                                this@LeafMonitorService,
                                "Поездка завершена",
                                "${"%.1f".format(tripDist)} км · ${"%.2f".format(tripWh / 1000)} кВт·ч"
                            )
                        }
                    }
                    tripStart = 0L
                }
            }
            if (s.odometer > 0) lastOdo = s.odometer
        }
    }
}
