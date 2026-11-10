package com.leafcontrol.app.net

import android.content.Context
import com.leafcontrol.app.data.Charge
import com.leafcontrol.app.data.LeafState
import com.leafcontrol.app.data.Settings
import com.leafcontrol.app.data.Trip
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONArray
import org.json.JSONObject
import java.net.URLEncoder
import java.util.concurrent.TimeUnit

/**
 * LeafControl server (the same address the PWA uses). The phone talks to the
 * server, the server talks to the board, so the app works from any network.
 *
 * Auth is a normal account session: POST /api/auth/login returns the
 * leaf_session cookie, which we keep in SharedPreferences and send as a Cookie
 * header on every request.
 */
object ServerApi {
    private val client = OkHttpClient.Builder()
        .connectTimeout(8, TimeUnit.SECONDS)
        .readTimeout(15, TimeUnit.SECONDS)
        .callTimeout(20, TimeUnit.SECONDS)
        .followRedirects(true)
        .build()

    private val jsonType = "application/json".toMediaType()

    @Volatile
    private var loggingIn = false

    fun configured(): Boolean =
        Settings.serverUrl.isNotBlank() && Settings.serverUser.isNotBlank() && Settings.serverPass.isNotEmpty()

    private fun base(): String = Settings.serverUrl.trim().trimEnd('/')

    private fun request(path: String, method: String = "GET", body: String? = null): Request {
        val b = Request.Builder().url(base() + path)
        if (Settings.serverCookie.isNotEmpty()) b.header("Cookie", Settings.serverCookie)
        b.header("Accept", "application/json")
        if (body != null) b.method(method, body.toRequestBody(jsonType))
        else if (method == "GET") b.get()
        else b.method(method, "".toRequestBody(null))
        return b.build()
    }

    private fun saveCookie(context: Context, raw: String) {
        // "leaf_session=...; Path=/; HttpOnly; SameSite=Lax" -> "leaf_session=..."
        val first = raw.split(";").firstOrNull()?.trim().orEmpty()
        if (first.startsWith("leaf_session=")) Settings.serverCookie = first
    }

    /** Logs in and remembers the session cookie. */
    fun login(context: Context, force: Boolean = false): Boolean {
        if (!configured()) return false
        if (!force && Settings.serverCookie.isNotEmpty() && Settings.serverCookieTs > 0 &&
            System.currentTimeMillis() - Settings.serverCookieTs < 3L * 24 * 3600 * 1000
        ) {
            return true
        }
        if (loggingIn) return false
        loggingIn = true
        try {
            val payload = JSONObject()
                .put("username", Settings.serverUser.trim())
                .put("password", Settings.serverPass)
                .toString()
            val req = Request.Builder()
                .url(base() + "/api/auth/login")
                .post(payload.toRequestBody(jsonType))
                .header("Content-Type", "application/json")
                .build()
            client.newCall(req).execute().use { r ->
                val cookie = r.header("Set-Cookie")
                if (r.isSuccessful && cookie != null) {
                    saveCookie(context, cookie)
                    Settings.serverCookieTs = System.currentTimeMillis()
                    return true
                }
                return false
            }
        } catch (_: Exception) {
            return false
        } finally {
            loggingIn = false
        }
    }

    /** GET helper with one silent re-login on 401. */
    private fun get(context: Context, path: String): String? {
        try {
            client.newCall(request(path)).execute().use { r ->
                if (r.code == 401) {
                    Settings.serverCookie = ""
                    if (!login(context, force = true)) return null
                    client.newCall(request(path)).execute().use { r2 ->
                        return if (r2.isSuccessful) r2.body?.string() else null
                    }
                }
                return if (r.isSuccessful) r.body?.string() else null
            }
        } catch (_: Exception) {
            return null
        }
    }

    suspend fun status(context: Context): LeafState? = withContext(Dispatchers.IO) {
        if (!configured()) return@withContext null
        if (Settings.serverCookie.isEmpty() && !login(context)) return@withContext null
        val body = get(context, "/api/status") ?: return@withContext null
        try {
            val o = JSONObject(body)
            val online = o.optBoolean("online", true)
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
                board = o.optString("device_name", o.optString("board", "")),
                modem = o.optString("modem", ""),
                epoch = o.optLong("epoch", 0L),
                tariffDay = o.optDouble("tariff_day", 0.0),
                tariffNight = o.optDouble("tariff_night", 0.0),
                nightFrom = o.optInt("night_from", 0),
                nightTo = o.optInt("night_to", 7),
                lastUpdateMs = System.currentTimeMillis(),
                connected = online,
                transport = "server",
            )
        } catch (_: Exception) {
            null
        }
    }

    /** Sends a command through the server: /lock, /unlock, /hvac_on?... */
    suspend fun command(context: Context, path: String): Boolean = withContext(Dispatchers.IO) {
        if (!configured()) return@withContext false
        if (Settings.serverCookie.isEmpty() && !login(context)) return@withContext false
        try {
            client.newCall(request(path, method = "POST")).execute().use { r ->
                if (r.code == 401) {
                    Settings.serverCookie = ""
                    if (!login(context, force = true)) return@withContext false
                    client.newCall(request(path, method = "POST")).execute().use { r2 ->
                        return@withContext r2.isSuccessful
                    }
                }
                return@withContext r.isSuccessful
            }
        } catch (_: Exception) {
            false
        }
    }

    suspend fun trips(context: Context): List<Trip> = withContext(Dispatchers.IO) {
        val body = get(context, "/api/trips") ?: return@withContext emptyList()
        try {
            val arr = JSONArray(body)
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
                    source = "server",
                    reason = o.optString("r", ""),
                )
            }
            out
        } catch (_: Exception) {
            emptyList()
        }
    }

    suspend fun charges(context: Context): List<Charge> = withContext(Dispatchers.IO) {
        val body = get(context, "/api/charges") ?: return@withContext emptyList()
        try {
            val arr = JSONArray(body)
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
                    source = "server",
                    reason = o.optString("r", ""),
                )
            }
            out
        } catch (_: Exception) {
            emptyList()
        }
    }

    /** Login check for the settings screen: returns a short status text. */
    suspend fun probe(context: Context): String = withContext(Dispatchers.IO) {
        if (!configured()) return@withContext "не задан адрес или логин"
        Settings.serverCookie = ""
        if (!login(context, force = true)) return@withContext "сервер не принял логин или пароль"
        val body = get(context, "/api/devices")
        if (body == null) return@withContext "вошли, но сервер не отдал список плат"
        try {
            val arr = JSONObject(body).optJSONArray("devices") ?: JSONArray()
            if (arr.length() == 0) return@withContext "вход выполнен, плат нет"
            val d = arr.getJSONObject(0)
            val online = d.optBoolean("online", false)
            return@withContext "вход выполнен · " + (d.optString("name", d.optString("login", "плата"))) +
                (if (online) " на связи" else " не на связи")
        } catch (_: Exception) {
            "вход выполнен"
        }
    }

    fun encoded(value: String): String = URLEncoder.encode(value, "UTF-8")
}
