package com.leafcontrol.app.data

/** One snapshot of the car, from local HTTP or MQTT. */
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
    val odometer: Int = 0,
    val soh: Int = -1,
    val locked: Int = -1,
    val lights: Boolean = false,
    val on: Int = -1,
    val vin: String = "",
    val lastUpdateMs: Long = 0L,
    val connected: Boolean = false,
    val transport: String = "offline",
) {
    fun fresh(now: Long = System.currentTimeMillis()): Boolean =
        connected && lastUpdateMs > 0 && now - lastUpdateMs < 90_000L
}
