package com.leafcontrol.app.data

import android.content.Context
import android.content.SharedPreferences

/**
 * Everything the app needs to talk to the board. Stored in plain
 * SharedPreferences because the values are small and the app has no account.
 */
object Settings {
    private lateinit var p: SharedPreferences

    fun init(context: Context) {
        p = context.applicationContext.getSharedPreferences("leaf_settings", Context.MODE_PRIVATE)
    }

    var mode: String
        get() = p.getString("mode", "auto") ?: "auto"
        set(v) = p.edit().putString("mode", v).apply()

    var localHost: String
        get() = p.getString("local_host", "192.168.4.1") ?: "192.168.4.1"
        set(v) = p.edit().putString("local_host", v.trim()).apply()

    var localPort: Int
        get() = p.getInt("local_port", 80)
        set(v) = p.edit().putInt("local_port", v).apply()

    var mqttHost: String
        get() = p.getString("mqtt_host", "m5.wqtt.ru") ?: "m5.wqtt.ru"
        set(v) = p.edit().putString("mqtt_host", v.trim()).apply()

    var mqttPort: Int
        get() = p.getInt("mqtt_port", 15871)
        set(v) = p.edit().putInt("mqtt_port", v).apply()

    var mqttUser: String
        get() = p.getString("mqtt_user", "") ?: ""
        set(v) = p.edit().putString("mqtt_user", v.trim()).apply()

    var mqttPass: String
        get() = p.getString("mqtt_pass", "") ?: ""
        set(v) = p.edit().putString("mqtt_pass", v).apply()

    var vehicleId: String
        get() = p.getString("vehicle_id", "leaf") ?: "leaf"
        set(v) = p.edit().putString("vehicle_id", v.trim().ifEmpty { "leaf" }).apply()

    var mqttPath: String
        get() = p.getString("mqtt_path", "/") ?: "/"
        set(v) = p.edit().putString("mqtt_path", v).apply()

    var monitorEnabled: Boolean
        get() = p.getBoolean("monitor", true)
        set(v) = p.edit().putBoolean("monitor", v).apply()

    var notifyChargeStart: Boolean
        get() = p.getBoolean("n_charge_start", true)
        set(v) = p.edit().putBoolean("n_charge_start", v).apply()

    var notifyChargeEnd: Boolean
        get() = p.getBoolean("n_charge_end", true)
        set(v) = p.edit().putBoolean("n_charge_end", v).apply()

    var notifyLowSoc: Boolean
        get() = p.getBoolean("n_low_soc", true)
        set(v) = p.edit().putBoolean("n_low_soc", v).apply()

    var notifyOffline: Boolean
        get() = p.getBoolean("n_offline", true)
        set(v) = p.edit().putBoolean("n_offline", v).apply()

    var notifyTripEnd: Boolean
        get() = p.getBoolean("n_trip_end", true)
        set(v) = p.edit().putBoolean("n_trip_end", v).apply()

    var lowSocThreshold: Int
        get() = p.getInt("low_soc", 20)
        set(v) = p.edit().putInt("low_soc", v.coerceIn(5, 50)).apply()

    var tariffDay: Double
        get() = p.getFloat("tariff_day", 0f).toDouble()
        set(v) = p.edit().putFloat("tariff_day", v.toFloat()).apply()

    var tariffNight: Double
        get() = p.getFloat("tariff_night", 0f).toDouble()
        set(v) = p.edit().putFloat("tariff_night", v.toFloat()).apply()

    var nightFrom: Int
        get() = p.getInt("night_from", 0)
        set(v) = p.edit().putInt("night_from", v.coerceIn(0, 23)).apply()

    var nightTo: Int
        get() = p.getInt("night_to", 7)
        set(v) = p.edit().putInt("night_to", v.coerceIn(0, 23)).apply()
}
