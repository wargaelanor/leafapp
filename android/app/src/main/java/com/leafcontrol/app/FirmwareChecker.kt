package com.leafcontrol.app

import android.content.ContentValues
import android.content.Context
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import com.leafcontrol.app.data.LeafState
import org.json.JSONArray
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.net.HttpURLConnection
import java.net.URL

/**
 * Looks up the firmware binaries on the owner's LeafControl server
 * (/api/fw/latest) and saves the one matching the board.
 */
object FirmwareChecker {
    data class Firmware(
        val version: String,
        val board: String,       // "CMD" (T-2CAN) or "TCALL"
        val assetName: String,
        val url: String,
        val sizeBytes: Long,
        val releaseTag: String,
    )

    /** "TCALL" for a T-Call, otherwise "CMD" (T-2CAN). */
    fun boardKind(state: LeafState): String = when {
        state.fw.contains("TCALL", ignoreCase = true) -> "TCALL"
        state.board.contains("T-Call", ignoreCase = true) -> "TCALL"
        state.fw.contains("CMD", ignoreCase = true) -> "CMD"
        state.board.contains("T-2CAN", ignoreCase = true) -> "CMD"
        else -> "CMD"
    }

    fun boardLabel(board: String): String =
        if (board == "TCALL") "T-Call" else "T-2CAN"

    /** Newest firmware for the given board, from the owner's server. */
    fun latest(board: String): Firmware? {
        val base = com.leafcontrol.app.data.Settings.serverUrl.trim().trimEnd('/')
        if (base.isEmpty()) return null
        val conn = URL("$base/api/fw/latest?board=$board").openConnection() as HttpURLConnection
        try {
            conn.connectTimeout = 8000
            conn.readTimeout = 12000
            conn.setRequestProperty("User-Agent", "LeafControl/${BuildConfig.VERSION_NAME}")
            if (conn.responseCode !in 200..299) return null
            val body = conn.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
            val o = org.json.JSONObject(body)
            if (!o.optBoolean("ok", false)) return null
            val url = o.optString("url")
            val file = o.optString("file")
            if (url.isEmpty() || file.isEmpty()) return null
            return Firmware(
                version = o.optString("version", "?"),
                board = board,
                assetName = file,
                url = url,
                sizeBytes = o.optLong("size", 0L),
                releaseTag = "server",
            )
        } catch (_: Exception) {
            return null
        } finally {
            conn.disconnect()
        }
    }

    /**
     * Downloads the firmware into a plain file (app cache) so it can be
     * streamed straight into the board's OTA endpoint. Blocking call.
     */
    fun downloadToFile(firmware: Firmware, dest: File, onProgress: (Int) -> Unit): Boolean {
        val conn = URL(firmware.url).openConnection() as HttpURLConnection
        try {
            conn.instanceFollowRedirects = true
            conn.connectTimeout = 10000
            conn.readTimeout = 30000
            if (conn.responseCode !in 200..299) return false
            val total = conn.contentLength.toLong()
            dest.outputStream().use { out ->
                val input = conn.inputStream
                val buf = ByteArray(16 * 1024)
                var done = 0L
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    out.write(buf, 0, n)
                    done += n
                    onProgress(if (total > 0) (done * 100 / total).toInt() else 0)
                }
            }
            return dest.length() > 0
        } catch (_: Exception) {
            dest.delete()
            return false
        } finally {
            conn.disconnect()
        }
    }

    /**
     * Downloads the firmware into the phone's Downloads folder. Blocking call:
     * run it from a background thread.
     *
     * @return display path on success, null otherwise.
     */
    fun download(
        context: Context,
        firmware: Firmware,
        onProgress: (Int) -> Unit = {},
    ): String? {
        val conn = URL(firmware.url).openConnection() as HttpURLConnection
        try {
            conn.instanceFollowRedirects = true
            conn.connectTimeout = 10000
            conn.readTimeout = 30000
            conn.setRequestProperty("User-Agent", "LeafControl/${BuildConfig.VERSION_NAME}")
            if (conn.responseCode !in 200..299) return null
            val total = conn.contentLength.toLong()
            return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                saveToMediaStore(context, firmware.assetName, conn.inputStream, total, onProgress)
            } else {
                saveToAppDownloads(context, firmware.assetName, conn.inputStream, total, onProgress)
            }
        } catch (_: Exception) {
            return null
        } finally {
            conn.disconnect()
        }
    }

    private fun saveToMediaStore(
        context: Context,
        name: String,
        input: InputStream,
        total: Long,
        onProgress: (Int) -> Unit,
    ): String? {
        val resolver = context.contentResolver
        val values = ContentValues().apply {
            put(MediaStore.Downloads.DISPLAY_NAME, name)
            put(MediaStore.Downloads.MIME_TYPE, "application/octet-stream")
            put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
            put(MediaStore.Downloads.IS_PENDING, 1)
        }
        val uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values) ?: return null
        return try {
            resolver.openOutputStream(uri)?.use { out ->
                copy(input, out, total, onProgress)
            } ?: return null
            values.clear()
            values.put(MediaStore.Downloads.IS_PENDING, 0)
            resolver.update(uri, values, null, null)
            "Download/$name"
        } catch (_: Exception) {
            resolver.delete(uri, null, null)
            null
        }
    }

    private fun saveToAppDownloads(
        context: Context,
        name: String,
        input: InputStream,
        total: Long,
        onProgress: (Int) -> Unit,
    ): String? {
        return try {
            val dir = context.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS) ?: context.filesDir
            val file = File(dir, name)
            file.outputStream().use { out -> copy(input, out, total, onProgress) }
            file.absolutePath
        } catch (_: Exception) {
            null
        }
    }

    private fun copy(input: InputStream, output: OutputStream, total: Long, onProgress: (Int) -> Unit) {
        val buffer = ByteArray(64 * 1024)
        var copied = 0L
        var lastPercent = -1
        while (true) {
            val read = input.read(buffer)
            if (read <= 0) break
            output.write(buffer, 0, read)
            copied += read
            if (total > 0) {
                val percent = ((copied * 100) / total).toInt()
                if (percent != lastPercent) {
                    lastPercent = percent
                    onProgress(percent)
                }
            }
        }
        if (total <= 0) onProgress(100)
    }
}

