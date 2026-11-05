package com.leafcontrol.app.data

import kotlin.math.roundToInt

/** One snapshot of the car, from local HTTP or MQTT, plus a small cache. */
data class LeafState(
    val soc: Double = -1.0,
    val range: Int = -1,
    val charging: Boolean = false,
    val charger: String = "",
    val hvac: Boolean = false,
    val power: Double = 0.0,
    val voltage: Double = -1.0,
    val current: Double = 0.0,
    val batteryTemp: Double = -99.0,
    val cabin: Double = -99.0,
    val ambient: Double = -99.0,
    val odometer: Int = 0,
    val soh: Int = -1,
    val gids: Int = -1,
    val cellDelta: Int = -1,
    val locked: Int = -1,
    val lights: Boolean = false,
    val on: Int = -1,
    val setpoint: Double = -1.0,
    val fan: Int = -1,
    val heat: Boolean = false,
    val cool: Boolean = false,
    val vent: Int = -1,
    val hvacTimer: Int = 0,
    val seq: Boolean = false,
    val locking: Boolean = false,
    val packValid: Boolean = false,
    val vin: String = "",
    val fw: String = "",
    val fwDesc: String = "",
    val board: String = "",
    val wifiRssi: Int = 0,
    val uptime: Int = 0,
    val modem: String = "off",
    val epoch: Long = 0L,
    val tariffDay: Double = 0.0,
    val tariffNight: Double = 0.0,
    val nightFrom: Int = 0,
    val nightTo: Int = 7,
    val lastUpdateMs: Long = 0L,
    val connected: Boolean = false,
    val transport: String = "offline",
) {
    fun fresh(now: Long = System.currentTimeMillis(), maxAgeMs: Long = 90_000L): Boolean =
        lastUpdateMs > 0 && now - lastUpdateMs < maxAgeMs

    /** Instrument range first, then a sane GIDS estimate; 0x3FF is invalid. */
    val displayRange: Int?
        get() = when {
            range > 0 -> range
            gids in 1..500 -> (gids * 0.6).roundToInt()
            else -> null
        }

    val isChargingKnown: Boolean get() = charging
}
