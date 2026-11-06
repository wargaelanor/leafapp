package com.leafcontrol.app.service

import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationManagerCompat
import androidx.core.app.ServiceCompat
import com.leafcontrol.app.BoardDiscovery
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
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.abs

class LeafMonitorService : Service() {
    companion object {
        const val ACTION_START = "com.leafcontrol.app.START"
        const val ACTION_STOP = "com.leafcontrol.app.STOP"
        const val ACTION_RELOAD = "com.leafcontrol.app.RELOAD"
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

        fun reload(context: Context) {
            val i = Intent(context, LeafMonitorService::class.java).setAction(ACTION_RELOAD)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) context.startForegroundService(i)
            else context.startService(i)
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
    private var prevPlugged = false
    private var lowNotified = false
    private var offlineNotified = false
    private var startedForeground = false
    @Volatile private var monitoring = false

    // Commands issued while MQTT is not connected yet; flushed on connect.
    private val pendingCommands = ArrayList<String>()
    private val pendingLock = Any()

    // Invalidates callbacks from a client that has already been replaced.
    @Volatile private var mqttGeneration = 0

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
                // A shortcut can arrive while the service is not running. Start
                // the monitor so MQTT commands have a chance to connect too.
                if (!monitoring && Settings.monitorEnabled) startMonitor()
                val path = intent.getStringExtra(EXTRA_PATH) ?: ""
                val payload = intent.getStringExtra(EXTRA_PAYLOAD) ?: ""
                sendCommand(path, payload)
                return START_STICKY
            }
            ACTION_RELOAD -> {
                ensureForeground("Применение настроек…")
                stopMonitor()
                if (Settings.monitorEnabled) startMonitor()
                return START_STICKY
            }
            else -> {
                ensureForeground("Запуск…")
                if (Settings.monitorEnabled) {
                    if (!monitoring) startMonitor()
                } else {
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
                // specialUse is not subject to the Android 15 six-hour dataSync
                // limit, so continuous monitoring survives a longer drive day.
                val type = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
                } else {
                    0
                }
                ServiceCompat.startForeground(this, MONITOR_ID, Notifications.monitor(this, text), type)
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
        LeafRepo.setMqttConnected(false)
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

    private suspend fun runAuto() = coroutineScope {
        // Local first; if the board is not on this Wi-Fi, MQTT takes over.
        // The local network is still probed while MQTT carries telemetry, so the
        // app returns to the board's own Wi-Fi without a service restart.
        var syncedAt = 0L
        var lastScanMs = 0L
        var mqttJob: Job? = null

        fun stopMqtt() {
            mqttJob?.cancel()
            mqttJob = null
            mqttGeneration++
            val client = mqttClient
            mqttClient = null
            LeafRepo.mqtt = null
            if (client != null) {
                client.stop()
                LeafRepo.setMqttConnected(false)
            }
        }

        try {
            while (monitoring) {
                val s = LocalApi.status(this@LeafMonitorService)
                if (s != null) {
                    if (mqttJob != null) {
                        stopMqtt()
                        LeafRepo.monitorText.value = "Локально"
                    }
                    onState(s)
                    updateForeground("Локально · ${s.soc.takeIf { it >= 0 }?.toInt() ?: "—"}%")
                    if (System.currentTimeMillis() - syncedAt > 300_000L) {
                        syncedAt = System.currentTimeMillis()
                        syncHistory()
                    }
                    // Live while the car is active (ready / charging / climate
                    // running): 4 s keeps V/A/SOC moving in near real time.
                    // Parked and asleep: back off to the old 20 s cadence.
                    val pollMs = if (s.on == 1 || s.charging || s.hvac) 4_000L else 20_000L
                    delay(pollMs)
                    continue
                }

                val now = System.currentTimeMillis()
                if (now - lastScanMs > 120_000L) {
                    lastScanMs = now
                    val board = BoardDiscovery.scanSubnet(this@LeafMonitorService, 400)
                    if (board != null) {
                        Settings.localHost = board.host
                        Settings.localPort = board.port
                        updateForeground("Найдена плата: ${board.host}")
                        continue
                    }
                }
                if (mqttJob?.isActive != true) LeafRepo.setDisconnected("local")

                if (Settings.mqttUser.isNotEmpty()) {
                    if (mqttJob?.isActive != true) {
                        updateForeground("Локально нет · MQTT…")
                        mqttJob = launch { runMqtt() }
                    }
                    // Keep looking for the board's Wi-Fi while MQTT is in use.
                    delay(25_000)
                    continue
                }

                updateForeground("Нет связи с платой")
                delay(30_000)
            }
        } finally {
            stopMqtt()
        }
    }

    private suspend fun runLocal() {
        updateForeground("Локальный режим")
        var syncedAt = 0L
        var lastScanMs = 0L
        while (monitoring) {
            val s = LocalApi.status(this)
            if (s != null) {
                onState(s)
                updateForeground("Локально · ${s.soc.takeIf { it >= 0 }?.toInt() ?: "—"}% · ${s.voltage.toInt()} В")
                if (System.currentTimeMillis() - syncedAt > 300_000L) {
                    syncedAt = System.currentTimeMillis()
                    syncHistory()
                }
                // 4 s while the car is active, 20 s when parked (see runAuto)
                val pollMs = if (s.on == 1 || s.charging || s.hvac) 4_000L else 20_000L
                delay(pollMs)
                continue
            } else {
                val now = System.currentTimeMillis()
                if (now - lastScanMs > 120_000L) {
                    lastScanMs = now
                    val board = BoardDiscovery.scanSubnet(this, 400)
                    if (board != null) {
                        Settings.localHost = board.host
                        Settings.localPort = board.port
                        updateForeground("Найдена плата: ${board.host}")
                        continue
                    }
                }
                LeafRepo.setDisconnected("local")
                updateForeground("Нет связи с платой")
                handleState(LeafRepo.state.value)
            }
            delay(20_000)
        }
    }

    private suspend fun runMqtt() {
        if (Settings.mqttUser.isEmpty()) {
            LeafRepo.setMqttConnected(false)
            updateForeground("MQTT не настроен")
            return
        }
        updateForeground("MQTT · подключение…")
        mqttClient?.stop()
        val gen = ++mqttGeneration
        val client = MqttWssClient(
            host = Settings.mqttHost,
            port = Settings.mqttPort,
            path = Settings.mqttPath,
            user = Settings.mqttUser,
            pass = Settings.mqttPass,
            vehicleId = Settings.vehicleId,
            onConnected = { ok ->
                scope.launch {
                    if (gen != mqttGeneration) return@launch
                    LeafRepo.setMqttConnected(ok)
                    if (ok) {
                        updateForeground("MQTT · подключено")
                        LeafRepo.monitorText.value = "MQTT · подключено"
                        LeafRepo.onMqttConnected()
                        flushPendingCommands()
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
        try {
            client.start()
            while (monitoring) {
                delay(15_000)
                val s = LeafRepo.state.value
                recorder.sample(s, System.currentTimeMillis())
                handleState(s)
                if (s.connected) updateForeground("MQTT · ${s.soc.takeIf { it >= 0 }?.toInt() ?: "—"}% · ${s.power.toInt()} кВт")
            }
        } finally {
            client.stop()
            if (mqttClient === client) {
                mqttClient = null
                LeafRepo.mqtt = null
            }
            LeafRepo.setMqttConnected(false)
        }
    }

    private fun sendCommand(path: String, payload: String) {
        scope.launch {
            // Local first: the board answers immediately when the phone is on
            // the same Wi-Fi. If that fails, wait for MQTT (the service may
            // have just been started by a home-screen shortcut).
            var sent = false
            if (Settings.mode != "mqtt" && path.isNotEmpty()) {
                sent = LeafRepo.commandLocal(this@LeafMonitorService, path)
                if (!sent) {
                    // The saved host may be stale (e.g. 192.168.4.1 from the
                    // board's own AP). Find the board on this Wi-Fi right now
                    // and retry once instead of falling back to MQTT for
                    // another five minutes.
                    val board = withContext(Dispatchers.IO) {
                        BoardDiscovery.scanSubnet(this@LeafMonitorService, 350)
                    }
                    if (board != null) {
                        Settings.localHost = board.host
                        Settings.localPort = board.port
                        sent = LeafRepo.commandLocal(this@LeafMonitorService, path)
                    }
                }
            }
            if (sent) {
                reportCommand("Команда отправлена")
                return@launch
            }
            if (Settings.mode == "local") {
                reportCommand("Плата не ответила")
                return@launch
            }
            if (payload.isEmpty()) {
                reportCommand("Плата не ответила")
                return@launch
            }
            if (!Settings.monitorEnabled) {
                reportCommand("Мониторинг выключен")
                return@launch
            }
            if (Settings.mqttUser.isEmpty()) {
                reportCommand("MQTT не настроен")
                return@launch
            }
            var waited = 0
            while (!LeafRepo.mqttConnected.value && monitoring && waited < 12_000) {
                delay(500)
                waited += 500
            }
            if (LeafRepo.commandRemote(payload)) {
                reportCommand("Команда отправлена")
            } else {
                queueCommand(payload)
                reportCommand("Команда в очереди — уйдёт после подключения")
            }
        }
    }

    private fun reportCommand(text: String) {
        LeafRepo.commandResult.tryEmit(text)
    }

    /** Keep a command until the MQTT session is ready instead of dropping it. */
    private fun queueCommand(payload: String) {
        synchronized(pendingLock) {
            if (pendingCommands.contains(payload)) return
            if (pendingCommands.size >= 20) pendingCommands.removeAt(0)
            pendingCommands.add(payload)
        }
    }

    private fun flushPendingCommands() {
        val batch = synchronized(pendingLock) { pendingCommands.toList() }
        if (batch.isEmpty()) return
        var sentAny = false
        for (payload in batch) {
            if (!LeafRepo.commandRemote(payload)) break
            synchronized(pendingLock) { pendingCommands.remove(payload) }
            sentAny = true
        }
        if (sentAny) reportCommand("Команда отправлена")
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
            // Connector events: the plug can be in the socket while no charge
            // runs (timed wait, or the session already finished). "Зарядка
            // началась" only covers the active session, so a plug-in without
            // a session was silent while the car picture also stayed plain.
            val plugged = s.charging || s.charger == "done" || s.charger == "wait"
            if (plugged != prevPlugged) {
                if (plugged && !s.charging && Settings.notifyChargeStart) {
                    val why = if (s.charger == "done") {
                        "зарядка завершена, пистолет в гнезде"
                    } else {
                        "ожидание старта зарядки"
                    }
                    Notifications.event(this, "Пистолет подключён", why + socText(s))
                }
            }
            prevPlugged = plugged
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
        mqttGeneration++
        LeafRepo.setMqttConnected(false)
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
                if ((dur >= 60_000L || chargeWh >= 20.0) &&
                    !HistoryDb.hasBoardChargeNear(chargeStart, now)
                ) {
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
                    if ((tripDist >= 0.05 || tripWh >= 10.0) &&
                        !HistoryDb.hasBoardTripNear(tripStart, now)
                    ) {
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
