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
import okhttp3.MultipartBody
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody
import okhttp3.RequestBody.Companion.toRequestBody
import okio.BufferedSink
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.net.Inet4Address
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

    @Volatile
    private var lastClockPushMs = 0L

    private fun client(context: Context, host: String): OkHttpClient {
        val cm = context.getSystemService(ConnectivityManager::class.java) ?: return base
        val wifi: Network? = cm.allNetworks.firstOrNull { n ->
            cm.getNetworkCapabilities(n)?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
        }
        if (wifi == null) return base
        // Bind to Wi-Fi unconditionally — for "leaf.local" too. Without the
        // binding Android sent hostname lookups and requests to the mobile
        // network, where the board's mDNS/DNS never answers, and "same
        // network" control looked dead even though the board page opened in
        // a browser by IP.
        return base.newBuilder()
            .socketFactory(wifi.socketFactory)
            .dns(object : Dns {
                override fun lookup(hostname: String) = wifi.getAllByName(hostname).toList()
            })
            .build()
    }

    private fun ipv4(value: String): ByteArray? {
        if (!Regex("""^\d{1,3}(\.\d{1,3}){3}$""").matches(value)) return null
        return try {
            (java.net.InetAddress.getByName(value) as? Inet4Address)?.address
        } catch (_: Exception) {
            null
        }
    }

    /**
     * A host saved from the board's own access point (192.168.4.1) is dead on
     * every other network, but it used to stick until a manual scan: with the
     * phone on the home Wi-Fi the app kept knocking on 192.168.4.1 and
     * reported "плата — нет данных" while the board sat on the same network.
     * Reset it to the mDNS name so the subnet scan can take over.
     */
    fun dropBoardApHostWhenOffAp(context: Context) {
        try {
            if (Settings.localHost != "192.168.4.1") return
            val cm = context.getSystemService(ConnectivityManager::class.java) ?: return
            val wifi = cm.allNetworks.firstOrNull { n ->
                cm.getNetworkCapabilities(n)?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
            } ?: return
            val ip = cm.getLinkProperties(wifi)?.linkAddresses
                ?.mapNotNull { it.address as? Inet4Address }
                ?.firstOrNull { !it.isLoopbackAddress }
                ?: return
            val b = ip.address
            val onBoardAp = (b[0].toInt() and 0xff) == 192 &&
                (b[1].toInt() and 0xff) == 168 &&
                (b[2].toInt() and 0xff) == 4
            if (!onBoardAp) Settings.localHost = "leaf.local"
        } catch (_: Exception) {
        }
    }

    private fun url(path: String): String =
        "http://${Settings.localHost}:${Settings.localPort}$path"

    /**
     * When the phone is on the board's own access point, the board is always
     * 192.168.4.1. A stale home/VPN address was the reason "Локально" looked
     * dead even though the board page opened in a browser.
     */
    fun ensureLocalHost(context: Context) {
        try {
            val cm = context.getSystemService(ConnectivityManager::class.java) ?: return
            val wifi = cm.allNetworks.firstOrNull { n ->
                cm.getNetworkCapabilities(n)?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
            } ?: return
            val ip = cm.getLinkProperties(wifi)?.linkAddresses
                ?.mapNotNull { it.address as? Inet4Address }
                ?.firstOrNull { !it.isLoopbackAddress }
                ?: return
            val b = ip.address
            val onBoardAp = (b[0].toInt() and 0xff) == 192 &&
                (b[1].toInt() and 0xff) == 168 &&
                (b[2].toInt() and 0xff) == 4
            if (onBoardAp && (Settings.localHost != "192.168.4.1" || Settings.localPort != 80)) {
                Settings.localHost = "192.168.4.1"
                Settings.localPort = 80
            }
        } catch (_: Exception) {
        }
    }

    suspend fun status(context: Context): LeafState? = withContext(Dispatchers.IO) {
        ensureLocalHost(context)
        dropBoardApHostWhenOffAp(context)
        try {
            val req = Request.Builder().url(url("/api/status")).get().build()
            client(context, Settings.localHost).newCall(req).execute().use { r ->
                if (!r.isSuccessful) return@withContext null
                val o = JSONObject(r.body?.string() ?: return@withContext null)
                val state = LeafState(
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
                // A board without NTP reports epoch 0 and cannot timestamp trips
                // or charges. Give it the phone's clock once over local HTTP.
                if (state.epoch <= 0L) pushClock(context)
                state
            }
        } catch (_: Exception) {
            null
        }
    }

    /**
     * Sends the phone's wall clock to /api/time. Throttled because a board with
     * older firmware has no such endpoint and would answer 404 on every poll.
     */
    private fun pushClock(context: Context) {
        val now = System.currentTimeMillis()
        if (now - lastClockPushMs < 60_000L) return
        lastClockPushMs = now
        try {
            val epoch = now / 1000L
            val req = Request.Builder()
                .url(url("/api/time?epoch=$epoch"))
                .post("".toRequestBody("text/plain".toMediaType()))
                .build()
            client(context, Settings.localHost).newCall(req).execute().close()
        } catch (_: Exception) {
        }
    }

    suspend fun trips(context: Context): List<Trip> = withContext(Dispatchers.IO) {
        ensureLocalHost(context)
        try {
            val req = Request.Builder().url(url("/api/trips")).get().build()
            client(context, Settings.localHost).newCall(req).execute().use { r ->
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
        ensureLocalHost(context)
        try {
            val req = Request.Builder().url(url("/api/charges")).get().build()
            client(context, Settings.localHost).newCall(req).execute().use { r ->
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
        ensureLocalHost(context)
        try {
            val req = Request.Builder()
                .url(url(path))
                .post("".toRequestBody("text/plain".toMediaType()))
                .build()
            client(context, Settings.localHost).newCall(req).execute().use { it.isSuccessful || it.code == 303 }
        } catch (_: Exception) {
            false
        }
    }

    /** Board settings for the native screen: GET /api/config. */
    suspend fun getConfig(context: Context): JSONObject? = withContext(Dispatchers.IO) {
        ensureLocalHost(context)
        dropBoardApHostWhenOffAp(context)
        try {
            val req = Request.Builder().url(url("/api/config")).get().build()
            client(context, Settings.localHost).newCall(req).execute().use { r ->
                if (!r.isSuccessful) return@withContext null
                JSONObject(r.body?.string() ?: return@withContext null)
            }
        } catch (_: Exception) {
            null
        }
    }

    /** Applies a partial settings JSON: POST /api/config. */
    suspend fun saveConfig(context: Context, json: JSONObject): JSONObject? =
        withContext(Dispatchers.IO) {
            ensureLocalHost(context)
            try {
                val req = Request.Builder()
                    .url(url("/api/config"))
                    .post(json.toString().toRequestBody("application/json".toMediaType()))
                    .build()
                client(context, Settings.localHost).newCall(req).execute().use { r ->
                    if (!r.isSuccessful) return@withContext null
                    JSONObject(r.body?.string() ?: return@withContext null)
                }
            } catch (_: Exception) {
                null
            }
        }

    /**
     * Streams a firmware image into the board's web-OTA endpoint
     * (POST /ota_update, multipart field "firmware"). The board reboots into
     * the new firmware on its own after a successful upload.
     */
    suspend fun otaUpdate(
        context: Context,
        file: File,
        onProgress: (Int) -> Unit,
    ): Boolean = withContext(Dispatchers.IO) {
        ensureLocalHost(context)
        try {
            val firmwarePart = object : RequestBody() {
                override fun contentType() = "application/octet-stream".toMediaType()
                override fun contentLength(): Long = file.length()
                override fun writeTo(sink: BufferedSink) {
                    file.inputStream().use { input ->
                        val total = file.length().coerceAtLeast(1L)
                        var sent = 0L
                        val buf = ByteArray(16 * 1024)
                        while (true) {
                            val n = input.read(buf)
                            if (n < 0) break
                            sink.write(buf, 0, n)
                            sent += n
                            onProgress((sent * 100 / total).toInt())
                        }
                        onProgress(100)
                    }
                }
            }
            val body = MultipartBody.Builder()
                .setType(MultipartBody.FORM)
                .addFormDataPart("firmware", file.name, firmwarePart)
                .build()
            val req = Request.Builder().url(url("/ota_update")).post(body).build()
            // An OTA push takes minutes on the ESP32 web server: drop the
            // global call timeout for this single call.
            val c = client(context, Settings.localHost).newBuilder()
                .callTimeout(0, TimeUnit.MILLISECONDS)
                .readTimeout(90, TimeUnit.SECONDS)
                .writeTimeout(90, TimeUnit.SECONDS)
                .build()
            c.newCall(req).execute().use { it.isSuccessful }
        } catch (_: Exception) {
            false
        }
    }

    suspend fun clearHistory(context: Context, charges: Boolean): Boolean = withContext(Dispatchers.IO) {
        ensureLocalHost(context)
        try {
            val path = if (charges) "/api/charges/clear" else "/api/trips/clear"
            val req = Request.Builder()
                .url(url(path))
                .header("X-Requested-With", "leaf")
                .post("".toRequestBody("text/plain".toMediaType()))
                .build()
            client(context, Settings.localHost).newCall(req).execute().use { it.isSuccessful }
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
        ensureLocalHost(context)
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
            client(context, Settings.localHost).newCall(req).execute().use {
                it.isSuccessful || it.code == 303
            }
        } catch (_: Exception) {
            false
        }
    }
}
