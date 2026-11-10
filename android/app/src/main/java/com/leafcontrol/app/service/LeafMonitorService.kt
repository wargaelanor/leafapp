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
import com.leafcontrol.app.net.ServerApi
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
    private var recorder = Recorder()
    private var prevCharging = false
    private var prevPlugged = false
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
                // A shortcut can arrive while the service is not running:
                // start the monitor so the command has a transport.
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
        monitorJob?.cancel()
        monitorJob = scope.launch {
            val mode = Settings.mode
            when (mode) {
                "local" -> runLocal()
                "server" -> runServer()
                else -> runAuto()
            }
        }
    }

    private suspend fun runAuto() = coroutineScope {
        // Локально, если плата в этой же сети; иначе — через сервер.
        var syncedAt = 0L
        var lastScanMs = 0L
        while (monitoring) {
            val s = LocalApi.status(this@LeafMonitorService)
            if (s != null) {
                onState(s)
                updateForeground("Локально · ${s.soc.takeIf { it >= 0 }?.toInt() ?: "—"}%")
                if (System.currentTimeMillis() - syncedAt > 300_000L) {
                    syncedAt = System.currentTimeMillis()
                    syncHistory()
                }
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
            LeafRepo.setDisconnected("local")

            if (ServerApi.configured()) {
                updateForeground("Локально нет · сервер…")
                runServer()
                continue
            }

            updateForeground("Нет связи с платой")
            delay(30_000)
        }
    }

    /**
     * Server mode: the app talks to the LeafControl server, the server talks to
     * the board. Works from any network, including mobile data.
     */
    private suspend fun runServer() {
        updateForeground("Сервер")
        var syncedAt = 0L
        var failures = 0
        while (monitoring) {
            if (!ServerApi.configured()) {
                LeafRepo.setDisconnected("server")
                updateForeground("Сервер не настроен")
                delay(30_000)
                continue
            }
            val s = ServerApi.status(this)
            if (s != null) {
                failures = 0
                onState(s)
                LeafRepo.monitorText.value = if (s.connected) "Сервер" else "Сервер · плата не на связи"
                val soc = s.soc.takeIf { it >= 0 }?.toInt()?.toString() ?: "—"
                updateForeground("Сервер · $soc%")
                if (System.currentTimeMillis() - syncedAt > 300_000L) {
                    syncedAt = System.currentTimeMillis()
                    syncHistory()
                }
                // 8 s while the car is active, 20 s when it is parked.
                delay(if (s.on == 1 || s.charging || s.hvac) 8_000L else 20_000L)
            } else {
                failures++
                LeafRepo.setDisconnected("server")
                updateForeground(if (failures > 2) "Сервер недоступен" else "Сервер…")
                delay(if (failures > 2) 30_000L else 8_000L)
            }
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

    private fun reportCommand(text: String) {
        LeafRepo.commandResult.tryEmit(text)
    }

    private fun sendCommand(path: String, payload: String) {
        scope.launch {
            // Server mode: the server queues the command for the board, so it
            // works from any network and needs no direct access to the car.
            if (Settings.mode == "server" && path.isNotEmpty()) {
                val ok = ServerApi.command(this@LeafMonitorService, path)
                reportCommand(if (ok) "Команда отправлена на сервер" else "Сервер не принял команду")
                return@launch
            }
            var sent = false
            if (path.isNotEmpty()) {
                sent = LeafRepo.commandLocal(this@LeafMonitorService, path)
                if (!sent) {
                    // The saved host may be stale (e.g. 192.168.4.1 from the
                    // board's own AP). Find the board on this Wi-Fi and retry.
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
            } else {
                reportCommand("Плата не отвечает: подключитесь к её Wi-Fi или включите режим «Сервер»")
            }
        }
    }

    private suspend fun syncHistory() {
        val t = if (Settings.mode == "server") ServerApi.trips(this) else LocalApi.trips(this)
        val c = if (Settings.mode == "server") ServerApi.charges(this) else LocalApi.charges(this)
        if (t.isNotEmpty() || c.isNotEmpty()) {
            t.forEach { HistoryDb.upsertTrip(it) }
            c.forEach { HistoryDb.upsertCharge(it) }
        }
        LeafRepo.loadHistory()
    }

    private fun onState(s: LeafState) {
        // Publish every poll into the UI state. Without this the screens
        // only ever saw data after a manual "Обновить" (setLocal was called
        // solely from refreshHistory): the garage climate button ignored the
        // board, the countdown reset on navigation and V/A looked frozen.
        LeafRepo.setLocal(s)
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
        monitorJob?.cancel()
        LeafRepo.setDisconnected("offline")
        LeafRepo.monitorText.value = "Мониторинг остановлен"
    }

    override fun onDestroy() {
        stopMonitor()
        scope.cancel()
        super.onDestroy()
    }

    /** Records trips/charges from the telemetry stream. */
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
            // The board writes its own history in local mode, so the app does
            // not duplicate it; server mode is recorded locally.
            if (s.transport == "local") {
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
