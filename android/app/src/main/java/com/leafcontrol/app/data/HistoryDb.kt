package com.leafcontrol.app.data

import android.content.ContentValues
import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper

data class Trip(
    val startMs: Long,
    val endMs: Long,
    val distKm: Double,
    val energyWh: Double,
    val whDay: Double,
    val whNight: Double,
    val startSoc: Int?,
    val endSoc: Int?,
    val source: String,
    val reason: String,
)

data class Charge(
    val startMs: Long,
    val endMs: Long,
    val energyWh: Double,
    val whDay: Double,
    val whNight: Double,
    val startSoc: Int?,
    val endSoc: Int?,
    val source: String,
    val reason: String,
)

private const val DB_VERSION = 4

// A phone-recorded session and the board's own record of the same session
// start within a minute of each other; these windows are deliberately loose.
private const val DEDUPE_START_MS = 10 * 60_000L
private const val DEDUPE_END_MS = 15 * 60_000L

object HistoryDb {
    private lateinit var helper: DB

    fun init(context: Context) {
        if (!::helper.isInitialized) helper = DB(context.applicationContext)
    }

    private class DB(context: Context) : SQLiteOpenHelper(context, "leaf_history.db", null, DB_VERSION) {
        override fun onCreate(db: SQLiteDatabase) {
            db.execSQL(
                """CREATE TABLE IF NOT EXISTS trips(
                    start_ms INTEGER PRIMARY KEY,
                    end_ms INTEGER NOT NULL,
                    dist_km REAL NOT NULL,
                    energy_wh REAL NOT NULL,
                    wh_day REAL NOT NULL,
                    wh_night REAL NOT NULL,
                    start_soc INTEGER,
                    end_soc INTEGER,
                    source TEXT NOT NULL,
                    reason TEXT NOT NULL
                )"""
            )
            db.execSQL(
                """CREATE TABLE IF NOT EXISTS charges(
                    start_ms INTEGER PRIMARY KEY,
                    end_ms INTEGER NOT NULL,
                    energy_wh REAL NOT NULL,
                    wh_day REAL NOT NULL,
                    wh_night REAL NOT NULL,
                    start_soc INTEGER,
                    end_soc INTEGER,
                    source TEXT NOT NULL,
                    reason TEXT NOT NULL
                )"""
            )
            db.execSQL("CREATE INDEX IF NOT EXISTS idx_trips_source ON trips(source)")
            db.execSQL("CREATE INDEX IF NOT EXISTS idx_charges_source ON charges(source)")
            createSamples(db)
            createSampleRanges(db)
        }

        private fun createSamples(db: SQLiteDatabase) {
            db.execSQL("""CREATE TABLE IF NOT EXISTS chart_samples(
                device TEXT NOT NULL, ts INTEGER NOT NULL, kw REAL, soc REAL, batt REAL, spd REAL,
                PRIMARY KEY(device, ts))""")
        }

        private fun createSampleRanges(db: SQLiteDatabase) {
            db.execSQL("""CREATE TABLE IF NOT EXISTS chart_sample_ranges(
                device TEXT NOT NULL, from_ts INTEGER NOT NULL, to_ts INTEGER NOT NULL,
                PRIMARY KEY(device, from_ts, to_ts))""")
        }

        override fun onUpgrade(db: SQLiteDatabase, oldVersion: Int, newVersion: Int) {
            // Additive migrations only: user history must survive app updates.
            if (oldVersion < 2) {
                db.execSQL("CREATE INDEX IF NOT EXISTS idx_trips_source ON trips(source)")
                db.execSQL("CREATE INDEX IF NOT EXISTS idx_charges_source ON charges(source)")
            }
            if (oldVersion < 3) createSamples(db)
            if (oldVersion < 4) createSampleRanges(db)
            // Future schema changes: ALTER TABLE ... ADD COLUMN here.
        }

        override fun onDowngrade(db: SQLiteDatabase, oldVersion: Int, newVersion: Int) {
            // Never drop history when an older build meets a newer database.
        }
    }

    fun saveSamples(device: String, from: Long, to: Long, points: List<com.leafcontrol.app.net.ServerApi.SamplePoint>) {
        val db = helper.writableDatabase
        db.beginTransaction()
        try {
            points.forEach { p ->
                val values = ContentValues().apply {
                    put("device", device); put("ts", p.ts)
                    put("kw", p.kw); put("soc", p.soc); put("batt", p.batt); put("spd", p.spd)
                }
                db.insertWithOnConflict("chart_samples", null, values, SQLiteDatabase.CONFLICT_REPLACE)
            }
            val range = ContentValues().apply {
                put("device", device); put("from_ts", from); put("to_ts", to)
            }
            db.insertWithOnConflict("chart_sample_ranges", null, range, SQLiteDatabase.CONFLICT_IGNORE)
            db.setTransactionSuccessful()
        } finally { db.endTransaction() }
    }

    fun hasSampleRange(device: String, from: Long, to: Long): Boolean {
        helper.readableDatabase.rawQuery(
            "SELECT 1 FROM chart_sample_ranges WHERE device=? AND from_ts<=? AND to_ts>=? LIMIT 1",
            arrayOf(device, from.toString(), to.toString()),
        ).use { return it.moveToFirst() }
    }

    fun samples(device: String, from: Long, to: Long): List<com.leafcontrol.app.net.ServerApi.SamplePoint> {
        val out = mutableListOf<com.leafcontrol.app.net.ServerApi.SamplePoint>()
        helper.readableDatabase.rawQuery(
            "SELECT ts,kw,soc,batt,spd FROM chart_samples WHERE device=? AND ts BETWEEN ? AND ? ORDER BY ts",
            arrayOf(device, from.toString(), to.toString())
        ).use { c ->
            while (c.moveToNext()) {
                fun value(i: Int): Double? = if (c.isNull(i)) null else c.getDouble(i)
                out.add(com.leafcontrol.app.net.ServerApi.SamplePoint(c.getLong(0), value(1), value(2), value(3), value(4)))
            }
        }
        return out
    }

    fun upsertTrip(t: Trip) {
        val db = helper.writableDatabase
        db.beginTransaction()
        try {
            // The board records the same trip independently of the phone. When
            // its copy arrives, drop the phone copy instead of counting twice.
            if (t.source == "board" || t.source == "server") {
                db.delete(
                    "trips",
                    "source = 'phone' AND ABS(start_ms - ?) <= $DEDUPE_START_MS " +
                        "AND ABS(end_ms - ?) <= $DEDUPE_END_MS",
                    arrayOf(t.startMs.toString(), t.endMs.toString()),
                )
            }
            db.insertWithOnConflict("trips", null, tripValues(t), SQLiteDatabase.CONFLICT_REPLACE)
            db.setTransactionSuccessful()
        } finally {
            db.endTransaction()
        }
    }

    fun upsertCharge(c: Charge) {
        val db = helper.writableDatabase
        db.beginTransaction()
        try {
            if (c.source == "board" || c.source == "server") {
                db.delete(
                    "charges",
                    "source = 'phone' AND ABS(start_ms - ?) <= $DEDUPE_START_MS " +
                        "AND ABS(end_ms - ?) <= $DEDUPE_END_MS",
                    arrayOf(c.startMs.toString(), c.endMs.toString()),
                )
            }
            db.insertWithOnConflict("charges", null, chargeValues(c), SQLiteDatabase.CONFLICT_REPLACE)
            db.setTransactionSuccessful()
        } finally {
            db.endTransaction()
        }
    }

    /** True when the board already stored a matching trip (MQTT duplicates). */
    fun hasBoardTripNear(startMs: Long, endMs: Long): Boolean = hasBoardNear("trips", startMs, endMs)

    fun hasBoardChargeNear(startMs: Long, endMs: Long): Boolean = hasBoardNear("charges", startMs, endMs)

    private fun hasBoardNear(table: String, startMs: Long, endMs: Long): Boolean {
        helper.readableDatabase.rawQuery(
            "SELECT 1 FROM $table WHERE source IN ('board', 'server') " +
                "AND ABS(start_ms - ?) <= $DEDUPE_START_MS AND ABS(end_ms - ?) <= $DEDUPE_END_MS LIMIT 1",
            arrayOf(startMs.toString(), endMs.toString()),
        ).use { return it.moveToFirst() }
    }

    private fun tripValues(t: Trip) = ContentValues().apply {
        put("start_ms", t.startMs)
        put("end_ms", t.endMs)
        put("dist_km", t.distKm)
        put("energy_wh", t.energyWh)
        put("wh_day", t.whDay)
        put("wh_night", t.whNight)
        put("start_soc", t.startSoc)
        put("end_soc", t.endSoc)
        put("source", t.source)
        put("reason", t.reason)
    }

    private fun chargeValues(c: Charge) = ContentValues().apply {
        put("start_ms", c.startMs)
        put("end_ms", c.endMs)
        put("energy_wh", c.energyWh)
        put("wh_day", c.whDay)
        put("wh_night", c.whNight)
        put("start_soc", c.startSoc)
        put("end_soc", c.endSoc)
        put("source", c.source)
        put("reason", c.reason)
    }

    fun trips(): List<Trip> {
        val out = ArrayList<Trip>()
        helper.readableDatabase.rawQuery("SELECT * FROM trips ORDER BY start_ms DESC", null).use { c ->
            while (c.moveToNext()) {
                out += Trip(
                    startMs = c.getLong(0),
                    endMs = c.getLong(1),
                    distKm = c.getDouble(2),
                    energyWh = c.getDouble(3),
                    whDay = c.getDouble(4),
                    whNight = c.getDouble(5),
                    startSoc = if (c.isNull(6)) null else c.getInt(6),
                    endSoc = if (c.isNull(7)) null else c.getInt(7),
                    source = c.getString(8),
                    reason = c.getString(9),
                )
            }
        }
        return out
    }

    fun charges(): List<Charge> {
        val out = ArrayList<Charge>()
        helper.readableDatabase.rawQuery("SELECT * FROM charges ORDER BY start_ms DESC", null).use { c ->
            while (c.moveToNext()) {
                out += Charge(
                    startMs = c.getLong(0),
                    endMs = c.getLong(1),
                    energyWh = c.getDouble(2),
                    whDay = c.getDouble(3),
                    whNight = c.getDouble(4),
                    startSoc = if (c.isNull(5)) null else c.getInt(5),
                    endSoc = if (c.isNull(6)) null else c.getInt(6),
                    source = c.getString(7),
                    reason = c.getString(8),
                )
            }
        }
        return out
    }

    fun clearTrips() = helper.writableDatabase.delete("trips", null, null)
    fun clearCharges() = helper.writableDatabase.delete("charges", null, null)
}
