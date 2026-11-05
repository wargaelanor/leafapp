package com.leafcontrol.app

import android.Manifest
import android.app.AlertDialog
import android.app.ProgressDialog
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.drawable.ColorDrawable
import android.os.Build
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.leafcontrol.app.data.Settings
import com.leafcontrol.app.service.LeafMonitorService
import com.leafcontrol.app.ui.LeafTheme
import com.leafcontrol.app.ui.MainScreen
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * Native Compose UI, no WebView and no PWA dependency. The service feeds
 * LeafRepo; the screens observe the same state and never reload the page.
 */
class MainActivity : ComponentActivity() {
    private var updateDialog: AlertDialog? = null
    private val checking = AtomicBoolean(false)

    private val notificationPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        applySystemBars()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
        if (Settings.monitorEnabled) {
            try {
                LeafMonitorService.start(this)
            } catch (_: Exception) {
            }
        }

        setContent {
            LeafTheme {
                Surface(Modifier.fillMaxSize()) { MainScreen() }
            }
        }

        handleShortcut(intent)
        if (intent?.getBooleanExtra("check_update", false) == true) {
            forceCheckUpdate()
        } else {
            maybeCheckUpdate()
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleShortcut(intent)
        if (intent.getBooleanExtra("check_update", false)) forceCheckUpdate()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        applySystemBars()
    }

    private fun isNight(): Boolean =
        (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
            Configuration.UI_MODE_NIGHT_YES

    private fun applySystemBars() {
        val dark = isNight()
        val bg = if (dark) "#0F1216" else "#F4F5F7"
        window.statusBarColor = Color.TRANSPARENT
        window.navigationBarColor = Color.TRANSPARENT
        window.setBackgroundDrawable(ColorDrawable(Color.parseColor(bg)))
        WindowInsetsControllerCompat(window, window.decorView).apply {
            isAppearanceLightStatusBars = !dark
            isAppearanceLightNavigationBars = !dark
        }
    }

    /** App shortcuts send a command without opening a second UI. */
    private fun handleShortcut(intent: Intent?) {
        val command = intent?.getStringExtra("leaf_command") ?: return
        intent.removeExtra("leaf_command")
        val pair = when (command) {
            "unlock" -> "/unlock" to "unlock"
            "lock" -> "/lock" to "lock"
            "hvac_on" -> "/hvac_on?setpoint=22&minutes=15" to "climatecontrol on 22 15"
            "hvac_off" -> "/hvac_off" to "climatecontrol off"
            else -> return
        }
        LeafMonitorService.command(this, pair.first, pair.second)
        Toast.makeText(this, "Команда отправлена", Toast.LENGTH_SHORT).show()
    }

    private fun maybeCheckUpdate() {
        if (!Settings.autoUpdate) return
        if (System.currentTimeMillis() - Settings.lastUpdateCheck < 24 * 60 * 60 * 1000L) return
        checkUpdate(manual = false)
    }

    private fun forceCheckUpdate() {
        checkUpdate(manual = true)
    }

    private fun checkUpdate(manual: Boolean) {
        if (!checking.compareAndSet(false, true)) return
        if (manual) Toast.makeText(this, "Проверяем обновление…", Toast.LENGTH_SHORT).show()
        thread(isDaemon = true) {
            try {
                val update = UpdateChecker.check(this)
                Settings.lastUpdateCheck = System.currentTimeMillis()
                runOnUiThread {
                    checking.set(false)
                    if (isFinishing || isDestroyed) return@runOnUiThread
                    if (update == null) {
                        if (manual) {
                            Toast.makeText(this, "У вас последняя версия", Toast.LENGTH_SHORT).show()
                        }
                    } else {
                        showUpdateDialog(update)
                    }
                }
            } catch (_: Exception) {
                runOnUiThread {
                    checking.set(false)
                    if (manual && !isFinishing && !isDestroyed) {
                        Toast.makeText(this, "Не удалось проверить обновление", Toast.LENGTH_LONG).show()
                    }
                }
            }
        }
    }

    private fun showUpdateDialog(update: UpdateChecker.Update) {
        updateDialog?.dismiss()
        updateDialog = AlertDialog.Builder(this)
            .setTitle("Доступно обновление ${update.version}")
            .setMessage(
                update.notes.take(900).ifBlank {
                    "Новая версия приложения Leaf Control."
                }
            )
            .setPositiveButton("Скачать") { _, _ -> downloadUpdate(update) }
            .setNegativeButton("Позже", null)
            .show()
    }

    private fun downloadUpdate(update: UpdateChecker.Update) {
        val progress = ProgressDialog(this).apply {
            setTitle("Обновление ${update.version}")
            setMessage("Скачивание…")
            setProgressStyle(ProgressDialog.STYLE_HORIZONTAL)
            max = 100
            setCancelable(false)
            show()
        }
        thread(isDaemon = true) {
            val file = try {
                UpdateChecker.download(this, update) { p ->
                    runOnUiThread { if (progress.isShowing) progress.progress = p }
                }
            } catch (_: Exception) {
                null
            }
            runOnUiThread {
                if (progress.isShowing) progress.dismiss()
                if (file == null) {
                    Toast.makeText(this, "Не удалось скачать обновление", Toast.LENGTH_LONG).show()
                    return@runOnUiThread
                }
                if (!UpdateChecker.isValid(this, file, update.version)) {
                    file.delete()
                    Toast.makeText(this, "Файл обновления повреждён", Toast.LENGTH_LONG).show()
                    return@runOnUiThread
                }
                if (!UpdateChecker.install(this, file)) {
                    Toast.makeText(
                        this,
                        "Разрешите установку из этого приложения и повторите",
                        Toast.LENGTH_LONG
                    ).show()
                }
            }
        }
    }

    override fun onDestroy() {
        updateDialog?.dismiss()
        super.onDestroy()
    }
}
