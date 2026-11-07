package com.leafcontrol.app.ui

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.leafcontrol.app.BoardDiscovery
import com.leafcontrol.app.data.Charge
import com.leafcontrol.app.data.LeafRepo
import com.leafcontrol.app.data.LeafState
import com.leafcontrol.app.data.Settings
import com.leafcontrol.app.data.Trip
import com.leafcontrol.app.service.LeafMonitorService
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

class LeafViewModel(app: Application) : AndroidViewModel(app) {
    val state: StateFlow<LeafState> = LeafRepo.state
    val trips: StateFlow<List<Trip>> = LeafRepo.trips
    val charges: StateFlow<List<Charge>> = LeafRepo.charges
    val monitorText: StateFlow<String> = LeafRepo.monitorText
    val message = MutableStateFlow<String?>(null)

    init {
        viewModelScope.launch {
            LeafRepo.loadHistory()
            LeafRepo.refreshHistory(getApplication())
            discoverBoard()
        }
        viewModelScope.launch {
            LeafRepo.commandResult.collect { message.value = it }
        }
    }

    fun refresh() {
        viewModelScope.launch {
            LeafRepo.refreshHistory(getApplication())
            message.value = if (state.value.connected) "Обновлено" else "Плата не отвечает"
        }
    }

    /** Find the board without an IP field; mDNS first, then the service polls. */
    fun discoverBoard() {
        val app = getApplication<Application>()
        BoardDiscovery.discoverNsd(app, 3000) { board ->
            if (board != null) {
                applyBoard(app, board.host, board.port)
            } else {
                // mDNS can be blocked on some routers; scan the current /24.
                viewModelScope.launch(Dispatchers.IO) {
                    val found = BoardDiscovery.scanSubnet(app)
                    if (found != null) applyBoard(app, found.host, found.port)
                }
            }
        }
    }

    private fun applyBoard(app: Application, host: String, port: Int) {
        if (host == Settings.localHost && port == Settings.localPort) return
        Settings.localHost = host
        Settings.localPort = port
        if (Settings.monitorEnabled) LeafMonitorService.reload(app)
        viewModelScope.launch { LeafRepo.refreshHistory(app) }
    }

    fun command(type: String, setpoint: Double = 22.0, minutes: Int = 15) {
        val path: String
        val payload: String
        when (type) {
            "charge" -> { path = "/charge_on"; payload = "charge start" }
            "chargeoff" -> { path = "/charge_off"; payload = "charge stop" }
            "lock" -> { path = "/lock"; payload = "lock" }
            "unlock" -> { path = "/unlock"; payload = "unlock" }
            "refresh" -> { path = "/refresh"; payload = "server v3 update modified" }
            "lights" -> { path = "/lights"; payload = "lights" }
            "hornlights" -> { path = "/hornlights"; payload = "hornlights" }
            "rhloff" -> { path = "/rhloff"; payload = "lights off" }
            "hvac" -> {
                val on = !state.value.hvac
                path = if (on) "/hvac_on?setpoint=${setpoint.toInt()}&minutes=$minutes" else "/hvac_off"
                payload = if (on) "climatecontrol on ${setpoint.toInt()} $minutes" else "climatecontrol off"
            }
            else -> return
        }
        val app = getApplication<Application>()
        val local = state.value.transport == "local" && state.value.connected
        if (Settings.mode != "mqtt" && local) {
            viewModelScope.launch {
                val code = com.leafcontrol.app.net.LocalApi.commandCode(app, path)
                val ok = code != null && (code in 200..299 || code == 303)
                message.value = when {
                    ok -> "Команда отправлена"
                    code == 503 -> "EV-CAN не готова — машина спит? Команда невозможна"
                    code == 400 -> "Команда отклонена платой (неверные параметры)"
                    code == null -> "Плата не ответила"
                    else -> "Плата ответила ошибкой HTTP $code"
                }
                if (ok) LeafRepo.refreshHistory(app)
            }
        } else {
            // The service reports the real outcome: sent locally, published to
            // MQTT, queued until the broker connects, or failed.
            LeafMonitorService.command(app, path, payload)
        }
    }

    fun clearHistory(charges: Boolean) {
        viewModelScope.launch {
            val app = getApplication<Application>()
            if (state.value.transport == "local" && state.value.connected) {
                com.leafcontrol.app.net.LocalApi.clearHistory(app, charges)
            }
            if (charges) com.leafcontrol.app.data.HistoryDb.clearCharges()
            else com.leafcontrol.app.data.HistoryDb.clearTrips()
            LeafRepo.loadHistory()
            message.value = "История очищена"
        }
    }

    fun openSettings() {
        val app = getApplication<Application>()
        try {
            app.startActivity(
                android.content.Intent(app, com.leafcontrol.app.SettingsActivity::class.java)
                    .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
            )
        } catch (_: Exception) {
        }
    }

    fun checkUpdate() {
        val app = getApplication<Application>()
        try {
            app.startActivity(
                android.content.Intent(app, com.leafcontrol.app.MainActivity::class.java)
                    .putExtra("check_update", true)
                    .addFlags(
                        android.content.Intent.FLAG_ACTIVITY_NEW_TASK or
                            android.content.Intent.FLAG_ACTIVITY_CLEAR_TOP or
                            android.content.Intent.FLAG_ACTIVITY_SINGLE_TOP
                    )
            )
        } catch (_: Exception) {
        }
    }
}
