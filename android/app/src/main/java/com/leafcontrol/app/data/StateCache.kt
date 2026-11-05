package com.leafcontrol.app.data

import android.content.Context
import org.json.JSONObject

/** Last snapshot on disk, so the app opens instantly with data offline. */
object StateCache {
    private const val PREFS = "leaf_state_cache"
    private const val KEY = "snapshot"

    fun save(context: Context, s: LeafState) {
        try {
            val o = JSONObject()
            o.put("soc", s.soc)
            o.put("range", s.range)
            o.put("charging", s.charging)
            o.put("charger", s.charger)
            o.put("hvac", s.hvac)
            o.put("power", s.power)
            o.put("voltage", s.voltage)
            o.put("current", s.current)
            o.put("batteryTemp", s.batteryTemp)
            o.put("cabin", s.cabin)
            o.put("ambient", s.ambient)
            o.put("odometer", s.odometer)
            o.put("soh", s.soh)
            o.put("gids", s.gids)
            o.put("cellDelta", s.cellDelta)
            o.put("locked", s.locked)
            o.put("lights", s.lights)
            o.put("on", s.on)
            o.put("setpoint", s.setpoint)
            o.put("fan", s.fan)
            o.put("heat", s.heat)
            o.put("cool", s.cool)
            o.put("vent", s.vent)
            o.put("hvacTimer", s.hvacTimer)
            o.put("seq", s.seq)
            o.put("locking", s.locking)
            o.put("packValid", s.packValid)
            o.put("vin", s.vin)
            o.put("fw", s.fw)
            o.put("fwDesc", s.fwDesc)
            o.put("board", s.board)
            o.put("wifiRssi", s.wifiRssi)
            o.put("uptime", s.uptime)
            o.put("modem", s.modem)
            o.put("epoch", s.epoch)
            o.put("tariffDay", s.tariffDay)
            o.put("tariffNight", s.tariffNight)
            o.put("nightFrom", s.nightFrom)
            o.put("nightTo", s.nightTo)
            o.put("lastUpdateMs", s.lastUpdateMs)
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit().putString(KEY, o.toString()).apply()
        } catch (_: Exception) {
        }
    }

    fun load(context: Context): LeafState? {
        return try {
            val text = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getString(KEY, null) ?: return null
            val o = JSONObject(text)
            LeafState(
                soc = o.optDouble("soc", -1.0),
                range = o.optInt("range", -1),
                charging = o.optBoolean("charging", false),
                charger = o.optString("charger", ""),
                hvac = o.optBoolean("hvac", false),
                power = o.optDouble("power", 0.0),
                voltage = o.optDouble("voltage", -1.0),
                current = o.optDouble("current", 0.0),
                batteryTemp = o.optDouble("batteryTemp", -99.0),
                cabin = o.optDouble("cabin", -99.0),
                ambient = o.optDouble("ambient", -99.0),
                odometer = o.optInt("odometer", 0),
                soh = o.optInt("soh", -1),
                gids = o.optInt("gids", -1),
                cellDelta = o.optInt("cellDelta", -1),
                locked = o.optInt("locked", -1),
                lights = o.optBoolean("lights", false),
                on = o.optInt("on", -1),
                setpoint = o.optDouble("setpoint", -1.0),
                fan = o.optInt("fan", -1),
                heat = o.optBoolean("heat", false),
                cool = o.optBoolean("cool", false),
                vent = o.optInt("vent", -1),
                hvacTimer = o.optInt("hvacTimer", 0),
                seq = o.optBoolean("seq", false),
                locking = o.optBoolean("locking", false),
                packValid = o.optBoolean("packValid", false),
                vin = o.optString("vin", ""),
                fw = o.optString("fw", ""),
                fwDesc = o.optString("fwDesc", ""),
                board = o.optString("board", ""),
                wifiRssi = o.optInt("wifiRssi", 0),
                uptime = o.optInt("uptime", 0),
                modem = o.optString("modem", "off"),
                epoch = o.optLong("epoch", 0L),
                tariffDay = o.optDouble("tariffDay", 0.0),
                tariffNight = o.optDouble("tariffNight", 0.0),
                nightFrom = o.optInt("nightFrom", 0),
                nightTo = o.optInt("nightTo", 7),
                lastUpdateMs = o.optLong("lastUpdateMs", 0L),
                connected = false,
                transport = "offline",
            )
        } catch (_: Exception) {
            null
        }
    }
}
