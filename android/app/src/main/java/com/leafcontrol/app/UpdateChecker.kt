package com.leafcontrol.app

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings as AndroidSettings
import androidx.core.content.FileProvider
import com.leafcontrol.app.data.Settings
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * Checks the public GitHub releases for an APK. Firmware/PWA-only releases are
 * skipped, so a release without an Android asset does not hide the last APK.
 */
object UpdateChecker {
    private const val REPO = "wargaelanor/leafapp"
    private const val API = "https://api.github.com/repos/$REPO/releases?per_page=30"

    data class Update(
        val version: String,
        val url: String,
        val notes: String,
    )

    fun check(context: Context): Update? {
        // Updates come from the owner's server first (the same place the
        // firmware lives); GitHub is only a fallback for a fresh install.
        try {
            checkServer(context)?.let { return it }
        } catch (_: Exception) {
        }
        return checkGitHub(context)
    }

    /** GET {server}/api/app/latest -> {version, url, notes}. */
    private fun checkServer(context: Context): Update? {
        val base = Settings.serverUrl.trim().trimEnd('/')
        if (base.isEmpty()) return null
        val current = BuildConfig.VERSION_NAME
        val conn = URL("$base/api/app/latest").openConnection() as HttpURLConnection
        try {
            conn.connectTimeout = 6000
            conn.readTimeout = 10000
            conn.setRequestProperty("User-Agent", "LeafControl/$current")
            if (conn.responseCode !in 200..299) return null
            val body = conn.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
            val o = JSONObject(body)
            val version = o.optString("version").trim()
            val url = o.optString("url").trim()
            if (version.isEmpty() || url.isEmpty()) return null
            if (!isNewer(version, current)) return null
            return Update(version, url, o.optString("notes"))
        } finally {
            conn.disconnect()
        }
    }

    private fun checkGitHub(context: Context): Update? {
        val current = BuildConfig.VERSION_NAME
        val conn = URL(API).openConnection() as HttpURLConnection
        try {
            conn.connectTimeout = 8000
            conn.readTimeout = 12000
            conn.setRequestProperty("User-Agent", "LeafControl/$current")
            conn.setRequestProperty("Accept", "application/vnd.github+json")
            if (conn.responseCode !in 200..299) {
                throw java.io.IOException("GitHub HTTP ${conn.responseCode}")
            }
            val body = conn.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
            val releases = JSONArray(body)
            for (i in 0 until releases.length()) {
                val release = releases.getJSONObject(i)
                if (release.optBoolean("draft") || release.optBoolean("prerelease")) continue
                val assets = release.optJSONArray("assets") ?: continue
                for (j in 0 until assets.length()) {
                    val asset = assets.getJSONObject(j)
                    val name = asset.optString("name")
                    if (!name.endsWith(".apk", ignoreCase = true)) continue
                    val version = versionFrom(name)
                        ?: versionFrom(release.optString("tag_name"))
                        ?: continue
                    if (isNewer(version, current)) {
                        return Update(
                            version = version,
                            url = asset.optString("browser_download_url"),
                            notes = release.optString("body")
                        )
                    }
                }
            }
            return null
        } finally {
            conn.disconnect()
        }
    }

    /** Blocking download; call it from a background thread. */
    fun download(
        context: Context,
        update: Update,
        onProgress: (Int) -> Unit = {},
    ): File? {
        val dir = File(context.cacheDir, "updates").apply { mkdirs() }
        val target = File(dir, "LeafControl-${update.version}.apk")
        // A previous attempt may have downloaded the APK but stopped at the
        // "allow unknown sources" screen. Reuse it instead of fetching 18 MB again.
        if (target.exists() && target.length() > 1_000_000) {
            val info = context.packageManager.getPackageArchiveInfo(target.absolutePath, 0)
            if (info?.versionName == update.version) return target
        }
        if (target.exists()) target.delete()
        val conn = URL(update.url).openConnection() as HttpURLConnection
        try {
            conn.instanceFollowRedirects = true
            conn.connectTimeout = 10000
            conn.readTimeout = 30000
            conn.setRequestProperty("User-Agent", "LeafControl/${BuildConfig.VERSION_NAME}")
            if (conn.responseCode !in 200..299) return null
            val total = conn.contentLength
            var copied = 0L
            conn.inputStream.use { input ->
                target.outputStream().use { output ->
                    val buffer = ByteArray(64 * 1024)
                    while (true) {
                        val n = input.read(buffer)
                        if (n <= 0) break
                        output.write(buffer, 0, n)
                        copied += n
                        if (total > 0) onProgress(((copied * 100) / total).toInt())
                    }
                }
            }
            return if (target.length() > 0) target else null
        } finally {
            conn.disconnect()
        }
    }

    /** True when the file is a complete APK of the expected version. */
    fun isValid(context: Context, file: File, version: String): Boolean {
        val info = context.packageManager.getPackageArchiveInfo(file.absolutePath, 0) ?: return false
        return info.versionName == version
    }

    /**
     * Opens the system installer. Returns false when the user must first allow
     * installs from this app; the settings screen is opened in that case.
     */
    fun install(context: Context, file: File): Boolean {
        val apk = try {
            FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", file)
        } catch (_: Exception) {
            return false
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O &&
            !context.packageManager.canRequestPackageInstalls()
        ) {
            context.startActivity(
                Intent(
                    AndroidSettings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:${context.packageName}")
                ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            )
            return false
        }
        return try {
            context.startActivity(
                Intent(Intent.ACTION_VIEW)
                    .setDataAndType(apk, "application/vnd.android.package-archive")
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
            )
            true
        } catch (_: Exception) {
            false
        }
    }

    private fun versionFrom(value: String): String? =
        Regex("""(\d+(?:\.\d+)+)""").find(value)?.groupValues?.get(1)

    private fun isNewer(remote: String, local: String): Boolean {
        val a = remote.split('.').map { it.toIntOrNull() ?: 0 }
        val b = local.split('.').map { it.toIntOrNull() ?: 0 }
        for (i in 0 until maxOf(a.size, b.size)) {
            val x = a.getOrElse(i) { 0 }
            val y = b.getOrElse(i) { 0 }
            if (x != y) return x > y
        }
        return false
    }
}
