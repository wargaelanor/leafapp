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
 * Looks up the firmware binaries published in the GitHub releases of the same
 * repository the app updates from, and saves the one matching the board.
 */
object FirmwareChecker {
    private const val API = "https://api.github.com/repos/wargaelanor/leafapp/releases?per_page=30"

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

    /** Newest non-prerelease firmware for the given board, or null. */
    fun latest(board: String): Firmware? {
        val conn = URL(API).openConnection() as HttpURLConnection
        try {
            conn.connectTimeout = 8000
            conn.readTimeout = 12000
            conn.setRequestProperty("User-Agent", "LeafControl/${BuildConfig.VERSION_NAME}")
            conn.setRequestProperty("Accept", "application/vnd.github+json")
            if (conn.responseCode !in 200..299) return null
            val body = conn.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
            val releases = JSONArray(body)
            val suffix = "-$board.bin"
            val versionPattern = Regex("""LeafControl-FW-(\d+(?:\.\d+){2,})-""")
            for (i in 0 until releases.length()) {
                val release = releases.getJSONObject(i)
                if (release.optBoolean("draft") || release.optBoolean("prerelease")) continue
                val assets = release.optJSONArray("assets") ?: continue
                for (j in 0 until assets.length()) {
                    val asset = assets.getJSONObject(j)
                    val name = asset.optString("name")
                    // ".partitions.bin" does not end with "-CMD.bin"/"-TCALL.bin".
                    if (!name.endsWith(suffix, ignoreCase = true)) continue
                    val version = versionPattern.find(name)?.groupValues?.get(1) ?: continue
                    val url = asset.optString("browser_download_url")
                    if (url.isEmpty()) continue
                    return Firmware(
                        version = version,
                        board = board,
                        assetName = name,
                        url = url,
                        sizeBytes = asset.optLong("size", 0L),
                        releaseTag = release.optString("tag_name"),
                    )
                }
            }
            return null
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
