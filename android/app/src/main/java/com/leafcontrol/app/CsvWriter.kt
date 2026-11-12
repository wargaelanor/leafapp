package com.leafcontrol.app

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import java.io.File

/** Результат сохранения файла: получилось ли и его адрес (для «Открыть»). */
data class SavedFile(val ok: Boolean, val uri: Uri?)

/** Writes the PWA's CSV export into the phone's Downloads folder. */
object CsvWriter {
    fun save(context: Context, name: String, text: String): Boolean {
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                val values = ContentValues().apply {
                    put(MediaStore.Downloads.DISPLAY_NAME, name)
                    put(MediaStore.Downloads.MIME_TYPE, "text/csv")
                    put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
                }
                val uri = context.contentResolver.insert(
                    MediaStore.Downloads.EXTERNAL_CONTENT_URI, values
                ) ?: return false
                context.contentResolver.openOutputStream(uri)?.use {
                    it.write(text.toByteArray(Charsets.UTF_8))
                }
                true
            } else {
                val dir = context.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS) ?: context.filesDir
                File(dir, name).writeText(text, Charsets.UTF_8)
                true
            }
        } catch (_: Exception) {
            false
        }
    }

    /** Сохраняет готовый файл (xlsx) в Загрузки. */
    fun saveBytes(context: Context, name: String, bytes: ByteArray, mime: String): SavedFile {
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                val values = ContentValues().apply {
                    put(MediaStore.Downloads.DISPLAY_NAME, name)
                    put(MediaStore.Downloads.MIME_TYPE, mime)
                    put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
                }
                val uri = context.contentResolver.insert(
                    MediaStore.Downloads.EXTERNAL_CONTENT_URI, values
                ) ?: return SavedFile(false, null)
                context.contentResolver.openOutputStream(uri)?.use { it.write(bytes) }
                    ?: return SavedFile(false, null)
                SavedFile(true, uri)
            } else {
                val dir = context.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS) ?: context.filesDir
                File(dir, name).writeBytes(bytes)
                SavedFile(true, null)
            }
        } catch (_: Exception) {
            SavedFile(false, null)
        }
    }
}
