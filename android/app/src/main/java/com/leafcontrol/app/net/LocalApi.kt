package com.leafcontrol.app.net

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import com.leafcontrol.app.data.Charge
import com.leafcontrol.app.data.LeafState
import com.leafcontrol.app.data.Settings
import com.leafcontrol.app.data.Trip
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.Dns
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.TimeUnit

/**
 * Local HTTP API of the board. Requests are bound to the Wi-Fi network, so a
 * phone that also has mobile data still reaches 192.168.4.1 / leaf.local.
 */
object LocalApi {
    private val base = OkHttpClient.Builder()
        .connectTimeout(4, TimeUnit.SECONDS)
        .readTimeout(8, TimeUnit.SECONDS)
        .callTimeout(10, TimeUnit.SECONDS)
        .build()

    private fun client(context: Context): OkHttpClient {
        val cm = context.getSystemService(ConnectivityManager::class.java) ?: return base
        val wifi: Network? = cm.allNetworks.firstOrNull { n ->
            cm.getNetworkCapabilities(n)?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
        }
        if (wifi == null) return base
        return base.newBuilder()
            .socketFactory(wifi.socketFactory)
            .dns(object : Dns {
                override fun lookup(hostname: String) = wifi.getAllByName(hostname).toList()
            })
            .build()
    }

    private fun url(path: String): String =
        "http://${Settings.localHost}:${Settings.localPort}$path"

    suspend fun status(context: Context): LeafState? = withContext(Dispatchers.IO) {
        try {
            val req = Request.Builder().url(url("/api/status")).get().build()
            client(context).newCall(req).execute().use { r ->
                if (!r.isSuccessful) return@withContext null
                val o = JSONObject(r.body?.string() ?: return@withContext null)
                LeafState(
                    soc = o.optDouble("soc", -1.0),
                    range = o.optInt("irange", -1),
                    charging = o.optInt("charging", 0) == 1,
                    charger = o.optString("charger", ""),
                    hvac = o.optInt("hvac", 0) == 1,
                    power = o.optDouble("power", 0.0),
                    voltage = o.optDouble("v", -1.0),
                    current = o.optDouble("i", 0.0),
                    batteryTemp = o.optDouble("btemp", -99.0),
                    cabin = o.optDouble("cabin", -99.0),
                    ambient = o.optDouble("amb", -99.0),
                    odometer = o.optInt("odo", 0),
                    soh = o.optInt("soh", -1),
                    gids = o.optInt("gids", -1),
                    cellDelta = o.optInt("cdelta", -1),
                    locked = o.optInt("lock", -1),
                    lights = o.optInt("lights", 0) == 1,
                    on = o.optInt("on", -1),
                    setpoint = o.optDouble("setpoint", -1.0),
                    fan = o.optInt("fan", -1),
                    heat = o.optInt("heat", 0) == 1,
                    cool = o.optInt("cool", 0) == 1,
                    vent = o.optInt("vent", -1),
                    hvacTimer = o.optInt("hvac_timer", 0),
                    seq = o.optInt("seq", 0) == 1,
                    locking = o.optInt("locking", 0) == 1,
                    packValid = o.optInt("pack", 0) == 1,
                    vin = o.optString("vin", ""),
                    fw = o.optString("fw", ""),
                    fwDesc = o.optString("fw_desc", ""),
                    board = o.optString("board", ""),
                    wifiRssi = o.optInt("wifi_rssi", 0),
                    uptime = o.optInt("uptime", 0),
                    modem = o.optString("modem", "off"),
                    epoch = o.optLong("epoch", 0L),
                    tariffDay = o.optDouble("tariff_day", 0.0),
                    tariffNight = o.optDouble("tariff_night", 0.0),
                    nightFrom = o.optInt("night_from", 0),
                    nightTo = o.optInt("night_to", 7),
                    lastUpdateMs = System.currentTimeMillis(),
                    connected = true,
                    transport = "local",
                )
            }
        } catch (_: Exception) {
            null
        }
    }

    suspend fun trips(context: Context): List<Trip> = withContext(Dispatchers.IO) {
        try {
            val req = Request.Builder().url(url("/api/trips")).get().build()
            client(context).newCall(req).execute().use { r ->
                if (!r.isSuccessful) return@withContext emptyList()
                val arr = JSONArray(r.body?.string() ?: "[]")
                val out = ArrayList<Trip>(arr.length())
                for (i in 0 until arr.length()) {
                    val o = arr.getJSONObject(i)
                    out += Trip(
                        startMs = o.optLong("s", 0) * 1000L,
                        endMs = o.optLong("e", 0) * 1000L,
                        distKm = o.optDouble("k", 0.0),
                        energyWh = o.optDouble("w", 0.0),
                        whDay = o.optDouble("wd", 0.0),
                        whNight = o.optDouble("wn", 0.0),
                        startSoc = o.optInt("s0", -1).takeIf { it >= 0 },
                        endSoc = o.optInt("s1", -1).takeIf { it >= 0 },
                        source = "board",
                        reason = o.optString("r", ""),
                    )
                }
                out
            }
        } catch (_: Exception) {
            emptyList()
        }
    }

    suspend fun charges(context: Context): List<Charge> = withContext(Dispatchers.IO) {
        try {
            val req = Request.Builder().url(url("/api/charges")).get().build()
            client(context).newCall(req).execute().use { r ->
                if (!r.isSuccessful) return@withContext emptyList()
                val arr = JSONArray(r.body?.string() ?: "[]")
                val out = ArrayList<Charge>(arr.length())
                for (i in 0 until arr.length()) {
                    val o = arr.getJSONObject(i)
                    out += Charge(
                        startMs = o.optLong("s", 0) * 1000L,
                        endMs = o.optLong("e", 0) * 1000L,
                        energyWh = o.optDouble("w", 0.0),
                        whDay = o.optDouble("wd", 0.0),
                        whNight = o.optDouble("wn", 0.0),
                        startSoc = o.optInt("s0", -1).takeIf { it >= 0 },
                        endSoc = o.optInt("s1", -1).takeIf { it >= 0 },
                        source = "board",
                        reason = o.optString("r", ""),
                    )
                }
                out
            }
        } catch (_: Exception) {
            emptyList()
        }
    }

    suspend fun command(context: Context, path: String): Boolean = withContext(Dispatchers.IO) {
        try {
            val req = Request.Builder()
                .url(url(path))
                .post("".toRequestBody("text/plain".toMediaType()))
                .build()
            client(context).newCall(req).execute().use { it.isSuccessful || it.code == 303 }
        } catch (_: Exception) {
            false
        }
    }

    suspend fun clearHistory(context: Context, charges: Boolean): Boolean = withContext(Dispatchers.IO) {
        try {
            val path = if (charges) "/api/charges/clear" else "/api/trips/clear"
            val req = Request.Builder()
                .url(url(path))
                .header("X-Requested-With", "leaf")
                .post("".toRequestBody("text/plain".toMediaType()))
                .build()
            client(context).newCall(req).execute().use { it.isSuccessful }
        } catch (_: Exception) {
            false
        }
    }

    suspend fun setTariff(
        context: Context,
        day: Double,
        night: Double,
        from: Int,
        to: Int,
    ): Boolean = withContext(Dispatchers.IO) {
        try {
            val body = okhttp3.FormBody.Builder()
                .add("day", day.toString())
                .add("night", night.toString())
                .add("from", from.toString())
                .add("to", to.toString())
                .build()
            val req = Request.Builder()
                .url(url("/api/tariff"))
                .header("X-Requested-With", "leaf")
                .post(body)
                .build()
            client(context).newCall(req).execute().use {
                it.isSuccessful || it.code == 303
            }
        } catch (_: Exception) {
            false
        }
    }
}
