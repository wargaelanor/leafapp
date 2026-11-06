package com.leafcontrol.app

import android.content.Intent
import android.content.res.Configuration
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.leafcontrol.app.data.LeafRepo
import com.leafcontrol.app.data.Settings
import com.leafcontrol.app.net.LocalApi
import com.leafcontrol.app.ui.LeafTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/**
 * Firmware update without a browser: checks the GitHub releases, downloads the
 * image for the detected board and streams it straight into the board's
 * web-OTA endpoint over the local network. A separate row keeps the web page
 * and a plain download into the phone's Downloads folder.
 */
class BoardUpdateActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val night = (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
            Configuration.UI_MODE_NIGHT_YES
        WindowInsetsControllerCompat(window, window.decorView).isAppearanceLightStatusBars = !night
        window.statusBarColor = Color.TRANSPARENT
        window.navigationBarColor = Color.TRANSPARENT
        setContent {
            LeafTheme {
                Surface(
                    Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) { BoardUpdateScreen() }
            }
        }
    }
}

@Composable
private fun BoardUpdateScreen() {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    val state = LeafRepo.state.value
    val kind = FirmwareChecker.boardKind(state)

    var currentFw by remember { mutableStateOf(state.fw) }
    var latest by remember { mutableStateOf<FirmwareChecker.Firmware?>(null) }
    var busy by remember { mutableStateOf(false) }
    var phase by remember { mutableStateOf("") }
    var progress by remember { mutableStateOf(0) }
    var result by remember { mutableStateOf("") }

    Column(
        Modifier
            .fillMaxSize()
            .safeDrawingPadding()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("Обновление прошивки платы", fontSize = 22.sp, color = MaterialTheme.colorScheme.onBackground)

        // A firmware push over Wi-Fi takes minutes: keep the screen awake.
        LaunchedEffect(busy) {
            val window = (ctx as? android.app.Activity)?.window
            if (window != null) {
                if (busy) {
                    window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                } else {
                    window.clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                }
            }
        }

        InfoCard {
            Text("Плата: ${FirmwareChecker.boardLabel(kind)}", fontSize = 13.sp)
            Text("Установлено: ${currentFw.ifEmpty { "—" }}", fontSize = 13.sp)
            Text(
                "Адрес: ${Settings.localHost} (телефон должен быть в одной сети с платой)",
                fontSize = 13.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            latest?.let {
                Text(
                    "Доступно: ${it.version} · ${it.sizeBytes / 1024} КБ · ${it.releaseTag}",
                    fontSize = 13.sp,
                    color = MaterialTheme.colorScheme.primary,
                )
            }
        }

        if (busy) {
            Text(phase, fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            LinearProgressIndicator(
                progress = { progress / 100f },
                modifier = Modifier.fillMaxWidth(),
            )
        }
        if (result.isNotEmpty()) {
            Text(
                result,
                fontSize = 13.sp,
                color = if (result.startsWith("Готово") || result.startsWith("Есть"))
                    MaterialTheme.colorScheme.primary
                else MaterialTheme.colorScheme.error,
            )
        }

        Button(
            enabled = !busy,
            onClick = {
                busy = true
                phase = "Проверка релизов…"
                progress = 0
                result = ""
                scope.launch {
                    val fw = withContext(Dispatchers.IO) { FirmwareChecker.latest(kind) }
                    busy = false
                    phase = ""
                    latest = fw
                    result = if (fw == null) {
                        "Не удалось получить список релизов — проверьте интернет"
                    } else {
                        ""
                    }
                }
            },
            modifier = Modifier.fillMaxWidth(),
        ) { Text("Проверить обновление") }

        Button(
            enabled = !busy && latest != null,
            onClick = {
                val target = latest ?: return@Button
                busy = true
                result = ""
                progress = 0
                scope.launch {
                    phase = "Скачивание ${target.version}…"
                    val file = File(ctx.cacheDir, "fw-${target.board}-${target.version}.bin")
                    val ok = withContext(Dispatchers.IO) {
                        FirmwareChecker.downloadToFile(target, file) { p -> progress = p }
                    }
                    if (!ok) {
                        busy = false
                        phase = ""
                        result = "Скачивание не удалось"
                        return@launch
                    }

                    phase = "Заливка в плату — не отключайте питание…"
                    progress = 0
                    val flashed = LocalApi.otaUpdate(ctx, file) { p -> progress = p }
                    if (!flashed) {
                        busy = false
                        phase = ""
                        result = "Плата не приняла прошивку — убедитесь, что она на связи"
                        return@launch
                    }

                    phase = "Плата перезагружается…"
                    progress = 0
                    // The board reboots into the new image; wait for /api/status
                    // to come back and report the new version.
                    var newFw: String? = null
                    var tries = 0
                    while (tries < 30 && newFw == null) {
                        delay(3000)
                        tries++
                        val s = LocalApi.status(ctx)
                        if (s != null) newFw = s.fw
                    }
                    busy = false
                    phase = ""
                    if (newFw != null) {
                        currentFw = newFw
                        result = "Готово: $newFw"
                        Toast.makeText(ctx, "Прошивка обновлена", Toast.LENGTH_SHORT).show()
                    } else {
                        result = "Загружено, но плата не ответила после перезагрузки — проверьте её страницу"
                    }
                }
            },
            modifier = Modifier.fillMaxWidth(),
        ) { Text("Скачать и прошить плату") }

        OutlinedButton(
            enabled = !busy && latest != null,
            onClick = {
                val target = latest ?: return@OutlinedButton
                busy = true
                phase = "Скачивание в «Загрузки»…"
                progress = 0
                result = ""
                scope.launch {
                    val path = withContext(Dispatchers.IO) {
                        FirmwareChecker.download(ctx, target) { p -> progress = p }
                    }
                    busy = false
                    phase = ""
                    result = if (path != null) "Сохранено: $path" else "Скачивание не удалось"
                }
            },
            modifier = Modifier.fillMaxWidth(),
        ) { Text("Только скачать (.bin в «Загрузки»)") }

        OutlinedButton(
            onClick = {
                LocalApi.ensureLocalHost(ctx)
                val url = "http://${Settings.localHost}:${Settings.localPort}/ota"
                try {
                    ctx.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
                } catch (_: Exception) {
                    Toast.makeText(ctx, "Не удалось открыть страницу платы", Toast.LENGTH_SHORT).show()
                }
            },
            modifier = Modifier.fillMaxWidth(),
        ) { Text("Открыть web-интерфейс платы") }

        Spacer(Modifier.height(12.dp))
    }
}

@Composable
private fun InfoCard(content: @Composable () -> Unit) {
    Surface(
        shape = RoundedCornerShape(12.dp),
        color = MaterialTheme.colorScheme.surface,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            content()
        }
    }
}
