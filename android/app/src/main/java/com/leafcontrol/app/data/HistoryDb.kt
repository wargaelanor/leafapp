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

object HistoryDb {
    private lateinit var helper: DB

    fun init(context: Context) {
        if (!::helper.isInitialized) helper = DB(context.applicationContext)
    }

    private class DB(context: Context) : SQLiteOpenHelper(context, "leaf_history.db", null, 1) {
        override fun onCreate(db: SQLiteDatabase) {
            db.execSQL(
                """CREATE TABLE trips(
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
                """CREATE TABLE charges(
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
        }

        override fun onUpgrade(db: SQLiteDatabase, oldVersion: Int, newVersion: Int) {
            db.execSQL("DROP TABLE IF EXISTS trips")
            db.execSQL("DROP TABLE IF EXISTS charges")
            onCreate(db)
        }
    }

    fun upsertTrip(t: Trip) {
        val v = ContentValues().apply {
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
        helper.writableDatabase.insertWithOnConflict("trips", null, v, SQLiteDatabase.CONFLICT_REPLACE)
    }

    fun upsertCharge(c: Charge) {
        val v = ContentValues().apply {
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
        helper.writableDatabase.insertWithOnConflict("charges", null, v, SQLiteDatabase.CONFLICT_REPLACE)
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
