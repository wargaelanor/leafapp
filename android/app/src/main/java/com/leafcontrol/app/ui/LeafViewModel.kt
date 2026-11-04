package com.leafcontrol.app.ui

import android.app.Application
import android.os.Build
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.leafcontrol.app.data.Charge
import com.leafcontrol.app.data.LeafRepo
import com.leafcontrol.app.data.Settings
import com.leafcontrol.app.data.Trip
import com.leafcontrol.app.service.LeafMonitorService
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

data class SettingsDraft(
    val mode: String = Settings.mode,
    val localHost: String = Settings.localHost,
    val localPort: String = Settings.localPort.toString(),
    val mqttHost: String = Settings.mqttHost,
    val mqttPort: String = Settings.mqttPort.toString(),
    val mqttUser: String = Settings.mqttUser,
    val mqttPass: String = Settings.mqttPass,
    val vehicleId: String = Settings.vehicleId,
    val monitor: Boolean = Settings.monitorEnabled,
    val notifyChargeStart: Boolean = Settings.notifyChargeStart,
    val notifyChargeEnd: Boolean = Settings.notifyChargeEnd,
    val notifyLowSoc: Boolean = Settings.notifyLowSoc,
    val notifyOffline: Boolean = Settings.notifyOffline,
    val notifyTripEnd: Boolean = Settings.notifyTripEnd,
    val lowSoc: String = Settings.lowSocThreshold.toString(),
    val tariffDay: String = Settings.tariffDay.toString(),
    val tariffNight: String = Settings.tariffNight.toString(),
    val nightFrom: String = Settings.nightFrom.toString(),
    val nightTo: String = Settings.nightTo.toString(),
) {
    fun toSettings() {
        Settings.mode = mode
        Settings.localHost = localHost
        localPort.toIntOrNull()?.let { Settings.localPort = it }
        Settings.mqttHost = mqttHost
        mqttPort.toIntOrNull()?.let { Settings.mqttPort = it }
        Settings.mqttUser = mqttUser
        Settings.mqttPass = mqttPass
        Settings.vehicleId = vehicleId
        Settings.monitorEnabled = monitor
        Settings.notifyChargeStart = notifyChargeStart
        Settings.notifyChargeEnd = notifyChargeEnd
        Settings.notifyLowSoc = notifyLowSoc
        Settings.notifyOffline = notifyOffline
        Settings.notifyTripEnd = notifyTripEnd
        lowSoc.toIntOrNull()?.let { Settings.lowSocThreshold = it }
        tariffDay.replace(',', '.').toDoubleOrNull()?.let { Settings.tariffDay = it }
        tariffNight.replace(',', '.').toDoubleOrNull()?.let { Settings.tariffNight = it }
        nightFrom.toIntOrNull()?.let { Settings.nightFrom = it }
        nightTo.toIntOrNull()?.let { Settings.nightTo = it }
    }
}

class LeafViewModel(app: Application) : AndroidViewModel(app) {
    val state: StateFlow<com.leafcontrol.app.data.LeafState> = LeafRepo.state
    val trips: StateFlow<List<Trip>> = LeafRepo.trips
    val charges: StateFlow<List<Charge>> = LeafRepo.charges
    val monitorText: StateFlow<String> = LeafRepo.monitorText
    val draft = MutableStateFlow(SettingsDraft())
    val message = MutableStateFlow<String?>(null)

    init {
        viewModelScope.launch {
            LeafRepo.loadHistory()
            LeafRepo.refreshHistory(getApplication())
        }
    }

    fun saveSettings() {
        draft.value.toSettings()
        message.value = "Настройки сохранены"
        val ctx = getApplication<Application>()
        if (Settings.monitorEnabled) LeafMonitorService.start(ctx) else LeafMonitorService.stop(ctx)
    }

    fun startMonitor() {
        Settings.monitorEnabled = true
        draft.value = draft.value.copy(monitor = true)
        LeafMonitorService.start(getApplication())
        message.value = "Мониторинг запущен"
    }

    fun stopMonitor() {
        Settings.monitorEnabled = false
        draft.value = draft.value.copy(monitor = false)
        LeafMonitorService.stop(getApplication())
        message.value = "Мониторинг остановлен"
    }

    fun refresh() {
        viewModelScope.launch {
            LeafRepo.refreshHistory(getApplication())
            message.value = "Обновлено"
        }
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
        val mode = Settings.mode
        val local = state.value.transport == "local"
        if ((mode == "local" || (mode == "auto" && local)) && state.value.connected) {
            viewModelScope.launch {
                val ok = LeafRepo.commandLocal(getApplication(), path)
                message.value = if (ok) "Команда отправлена" else "Плата не ответила"
                if (ok) {
                    LeafRepo.refreshHistory(getApplication())
                }
            }
        } else {
            LeafMonitorService.command(getApplication(), path, payload)
            message.value = "Команда отправлена"
        }
    }

    fun clearHistory(charges: Boolean) {
        viewModelScope.launch {
            if (state.value.transport == "local") {
                com.leafcontrol.app.net.LocalApi.clearHistory(getApplication(), charges)
            }
            if (charges) com.leafcontrol.app.data.HistoryDb.clearCharges()
            else com.leafcontrol.app.data.HistoryDb.clearTrips()
            LeafRepo.loadHistory()
            message.value = "История очищена"
        }
    }

    fun openBatterySettings() {
        val ctx = getApplication<Application>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            val i = android.content.Intent(android.provider.Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)
            i.data = android.net.Uri.parse("package:${ctx.packageName}")
            try { ctx.startActivity(i.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)) } catch (_: Exception) {
                ctx.startActivity(android.content.Intent(android.provider.Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
            }
        }
    }
}
