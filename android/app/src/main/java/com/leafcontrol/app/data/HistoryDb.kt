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

private const val DB_VERSION = 2

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
        }

        override fun onUpgrade(db: SQLiteDatabase, oldVersion: Int, newVersion: Int) {
            // Additive migrations only: user history must survive app updates.
            if (oldVersion < 2) {
                db.execSQL("CREATE INDEX IF NOT EXISTS idx_trips_source ON trips(source)")
                db.execSQL("CREATE INDEX IF NOT EXISTS idx_charges_source ON charges(source)")
            }
            // Future schema changes: ALTER TABLE ... ADD COLUMN here.
        }

        override fun onDowngrade(db: SQLiteDatabase, oldVersion: Int, newVersion: Int) {
            // Never drop history when an older build meets a newer database.
        }
    }

    fun upsertTrip(t: Trip) {
        val db = helper.writableDatabase
        db.beginTransaction()
        try {
            // The board records the same trip independently of the phone. When
            // its copy arrives, drop the phone copy instead of counting twice.
            if (t.source == "board") {
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
            if (c.source == "board") {
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
            "SELECT 1 FROM $table WHERE source = 'board' " +
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
