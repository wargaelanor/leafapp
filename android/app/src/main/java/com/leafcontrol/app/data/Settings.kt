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

    /** The PWA to show in the WebView. */
    var appUrl: String
        get() = p.getString("app_url", "https://wargaelanor.github.io/leafapp/")
            ?: "https://wargaelanor.github.io/leafapp/"
        set(v) = p.edit().putString("app_url", v.trim()).apply()

    /** LeafControl server (the same address the PWA uses). */
    var serverUrl: String
        get() = p.getString("server_url", "https://leaf.wargaelanor.keenetic.link")
            ?: "https://leaf.wargaelanor.keenetic.link"
        set(v) = p.edit().putString("server_url", v.trim().trimEnd('/')).apply()

    var serverUser: String
        get() = p.getString("server_user", "") ?: ""
        set(v) = p.edit().putString("server_user", v.trim()).apply()

    var serverPass: String
        get() = p.getString("server_pass", "") ?: ""
        set(v) = p.edit().putString("server_pass", v).apply()

    /** Session cookie of the server login (leaf_session=...). */
    var serverCookie: String
        get() = p.getString("server_cookie", "") ?: ""
        set(v) = p.edit().putString("server_cookie", v).apply()

    var serverCookieTs: Long
        get() = p.getLong("server_cookie_ts", 0L)
        set(v) = p.edit().putLong("server_cookie_ts", v).apply()

    var monitorEnabled: Boolean
        get() = p.getBoolean("monitor", true)
        set(v) = p.edit().putBoolean("monitor", v).apply()

    /** Climate duration selected on the Distance screen; 0 means no timer. */
    var climateMinutes: Int
        get() = p.getInt("climate_minutes", 15)
        set(v) = p.edit().putInt("climate_minutes", v).apply()

    /** Board profile used by the firmware downloader: "CMD" or "TCALL". */
    var firmwareBoard: String
        get() = p.getString("firmware_board", "") ?: ""
        set(v) = p.edit().putString("firmware_board", v).apply()

    /** Last time the app asked GitHub for a newer APK. */
    var lastUpdateCheck: Long
        get() = p.getLong("last_update_check", 0L)
        set(v) = p.edit().putLong("last_update_check", v).apply()

    /** Check GitHub automatically once a day; manual checks still work. */
    var autoUpdate: Boolean
        get() = p.getBoolean("auto_update", true)
        set(v) = p.edit().putBoolean("auto_update", v).apply()

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
