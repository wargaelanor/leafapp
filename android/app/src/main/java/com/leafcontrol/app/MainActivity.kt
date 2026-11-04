package com.leafcontrol.app

import android.Manifest
import android.app.DownloadManager
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.webkit.CookieManager
import android.webkit.JavascriptInterface
import android.webkit.URLUtil
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import com.leafcontrol.app.data.Settings
import com.leafcontrol.app.service.LeafMonitorService
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import kotlin.concurrent.thread

/**
 * The main UI is the real PWA, not a simplified Compose copy: the user asked for
 * exactly the same design and features. This activity is only a shell; the
 * native foreground service still does background MQTT/history/notifications.
 */
class MainActivity : ComponentActivity() {
    private lateinit var web: WebView
    private var fileCallback: ValueCallback<Array<Uri>>? = null

    private val notificationPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    private val storagePermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    private val filePicker =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            fileCallback?.onReceiveValue(
                WebChromeClient.FileChooserParams.parseResult(result.resultCode, result.data)
            )
            fileCallback = null
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
        if (Build.VERSION.SDK_INT <= Build.VERSION_CODES.P &&
            ContextCompat.checkSelfPermission(
                this, Manifest.permission.WRITE_EXTERNAL_STORAGE
            ) != PackageManager.PERMISSION_GRANTED
        ) {
            storagePermission.launch(Manifest.permission.WRITE_EXTERNAL_STORAGE)
        }
        if (Settings.monitorEnabled) {
            try { LeafMonitorService.start(this) } catch (_: Exception) { }
        }

        web = WebView(this)
        setContentView(web)
        if (BuildConfig.DEBUG) WebView.setWebContentsDebuggingEnabled(true)
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (web.canGoBack()) {
                    web.goBack()
                } else {
                    isEnabled = false
                    onBackPressedDispatcher.onBackPressed()
                }
            }
        })
        web.settings.apply {
            javaScriptEnabled = true
            domStorageEnabled = true
            databaseEnabled = true
            allowFileAccess = true
            allowContentAccess = true
            mediaPlaybackRequiresUserGesture = false
            mixedContentMode = WebSettings.MIXED_CONTENT_ALWAYS_ALLOW
            cacheMode = WebSettings.LOAD_DEFAULT
            textZoom = 100
        }
        web.addJavascriptInterface(Bridge(), "LeafNative")
        web.webViewClient = object : WebViewClient() {
            override fun shouldOverrideUrlLoading(view: WebView?, request: WebResourceRequest?): Boolean {
                val u = request?.url ?: return false
                val host = u.host ?: ""
                val local = listOf("leaf.local", "leaf", Settings.localHost).any { it == host }
                val pages = host.endsWith("github.io")
                if (local || pages || u.scheme == "about" || u.scheme == "data") return false
                try {
                    startActivity(Intent(Intent.ACTION_VIEW, u))
                } catch (_: Exception) { }
                return true
            }

            override fun onPageFinished(view: WebView?, url: String?) {
                super.onPageFinished(view, url)
                injectBridge()
            }

            override fun onReceivedError(
                view: WebView?,
                request: WebResourceRequest?,
                error: WebResourceError?,
            ) {
                super.onReceivedError(view, request, error)
                if (request?.isForMainFrame != true) return
                val current = view?.url ?: return
                if (!current.contains("github.io")) {
                    // Board page unavailable: open the internet PWA instead.
                    Toast.makeText(
                        this@MainActivity,
                        "Плата недоступна, открываю PWA",
                        Toast.LENGTH_SHORT
                    ).show()
                    view.loadUrl(Settings.appUrl)
                }
            }
        }
        web.webChromeClient = object : WebChromeClient() {
            override fun onShowFileChooser(
                webView: WebView?,
                callback: ValueCallback<Array<Uri>>?,
                params: FileChooserParams?,
            ): Boolean {
                fileCallback?.onReceiveValue(null)
                fileCallback = callback
                return try {
                    filePicker.launch(Intent(Intent.ACTION_GET_CONTENT).apply {
                        addCategory(Intent.CATEGORY_OPENABLE)
                        type = "*/*"
                    })
                    true
                } catch (_: Exception) {
                    fileCallback = null
                    false
                }
            }
        }
        // The PWA links to /api/developer/canlog/download and other files.
        // WebView would otherwise ignore those clicks.
        web.setDownloadListener { url, userAgent, contentDisposition, mimeType, _ ->
            try {
                val name = URLUtil.guessFileName(url, contentDisposition, mimeType)
                val request = DownloadManager.Request(Uri.parse(url))
                    .setTitle(name)
                    .setNotificationVisibility(
                        DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED
                    )
                    .setDestinationInExternalPublicDir(Environment.DIRECTORY_DOWNLOADS, name)
                    .addRequestHeader("User-Agent", userAgent)
                CookieManager.getInstance().getCookie(url)?.let {
                    request.addRequestHeader("Cookie", it)
                }
                (getSystemService(DOWNLOAD_SERVICE) as DownloadManager).enqueue(request)
                Toast.makeText(this, "Скачивание: $name", Toast.LENGTH_SHORT).show()
            } catch (_: Exception) {
                Toast.makeText(this, "Не удалось скачать файл", Toast.LENGTH_SHORT).show()
            }
        }
        loadStart(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        loadStart(intent)
    }

    /**
     * The PWA served by the board has every feature (tariff, history, OTA).
     * The public GitHub Pages copy is the fallback when the board is not on
     * this Wi-Fi. An explicit extra from a notification or settings screen wins.
     */
    private fun loadStart(intent: Intent?) {
        val wantBoard = intent?.getBooleanExtra("open_board", false) == true
        val wantPwa = intent?.getBooleanExtra("open_pwa", false) == true
        when {
            wantBoard -> loadBoard()
            wantPwa -> web.loadUrl(Settings.appUrl)
            else -> probeBoardThenLoad()
        }
    }

    private fun loadBoard() {
        web.loadUrl("http://${Settings.localHost}:${Settings.localPort}/app")
    }

    private fun probeBoardThenLoad() {
        val host = Settings.localHost
        val port = Settings.localPort
        thread(isDaemon = true) {
            val ok = try {
                val c = URL("http://$host:$port/api/status").openConnection() as HttpURLConnection
                c.connectTimeout = 1500
                c.readTimeout = 1500
                c.requestMethod = "GET"
                val code = c.responseCode
                c.disconnect()
                code in 200..399
            } catch (_: Exception) {
                false
            }
            runOnUiThread {
                if (isFinishing || isDestroyed) return@runOnUiThread
                if (ok) loadBoard() else web.loadUrl(Settings.appUrl)
            }
        }
    }

    /** Keep the native service in sync with the PWA's own settings. */
    private fun injectBridge() {
        val js = """
            (function(){
              function sync(){
                try {
                  var c = localStorage.getItem('leafcfg');
                  if (c && window.LeafNative) LeafNative.saveConfig(c);
                  if (window.LeafNative) LeafNative.setLocalHost(location.hostname);
                } catch(e) {}
              }
              sync(); if (!window.__leafSync) { window.__leafSync = setInterval(sync, 5000); }
              // Native-only entry point, styled exactly like the PWA's own rows.
              // Everything else (design, history, tariff, OTA, developer tools)
              // remains the real PWA.
              try {
                if (window.LeafNative && !document.getElementById('leaf-native-settings')) {
                  var screen = document.getElementById('screen-more');
                  var anchor = document.getElementById('more-settings-label');
                  var row = anchor ? anchor.closest('.more-item') : null;
                  if (screen) {
                    var item = document.createElement('div');
                    item.className = 'more-item';
                    item.id = 'leaf-native-settings';
                    item.innerHTML = '<div class="mi"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6"><rect x="5" y="2" width="14" height="20" rx="2"/><path d="M9 18h6"/></svg></div>Настройки Android<div class="chev"><svg viewBox="0 0 24 24" fill="currentColor"><path d="M10 6L8.59 7.41 13.17 12l-4.58 4.59L10 18l6-6z"/></svg></div>';
                    item.onclick = function(){ LeafNative.openSettings(); };
                    if (row && row.parentNode) row.parentNode.insertBefore(item, row.nextSibling);
                    else screen.insertBefore(item, screen.firstChild);
                  }
                }
              } catch(e) {}
              if (window.LeafNative && typeof downloadCsv === 'function' && !window.__leafCsv) {
                window.__leafCsv = true;
                var orig = downloadCsv;
                window.downloadCsv = function(name, head, rows){
                  try {
                    var lines = [head].concat(rows).map(function(r){
                      return r.map(function(v){
                        var s = (v === null || v === undefined) ? '' : String(v);
                        return /[";\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
                      }).join(';');
                    });
                    LeafNative.saveCsv(name, '\ufeff' + lines.join('\r\n'));
                    if (typeof toast === 'function') toast('Файл сохранён в Загрузки');
                  } catch(e) { orig(name, head, rows); }
                };
              }
            })();
        """.trimIndent()
        web.evaluateJavascript(js, null)
    }

    inner class Bridge {
        @JavascriptInterface
        fun saveConfig(json: String) {
            try {
                val o = JSONObject(json)
                val old = listOf(
                    Settings.mqttHost, Settings.mqttPort.toString(), Settings.mqttUser,
                    Settings.mqttPass, Settings.vehicleId
                )
                Settings.mqttHost = o.optString("host", Settings.mqttHost)
                o.optInt("port", Settings.mqttPort).let { if (it > 0) Settings.mqttPort = it }
                Settings.mqttUser = o.optString("user", Settings.mqttUser)
                Settings.mqttPass = o.optString("pass", Settings.mqttPass)
                Settings.vehicleId = o.optString("vid", Settings.vehicleId)
                val now = listOf(
                    Settings.mqttHost, Settings.mqttPort.toString(), Settings.mqttUser,
                    Settings.mqttPass, Settings.vehicleId
                )
                if (old != now && Settings.monitorEnabled) {
                    LeafMonitorService.reload(this@MainActivity)
                }
            } catch (_: Exception) { }
        }

        @JavascriptInterface
        fun setLocalHost(host: String) {
            if (host.isBlank() || host.endsWith("github.io")) return
            if (Settings.localHost != host) {
                Settings.localHost = host
                Settings.localPort = 80
            }
        }

        @JavascriptInterface
        fun openSettings() {
            runOnUiThread { startActivity(Intent(this@MainActivity, SettingsActivity::class.java)) }
        }

        @JavascriptInterface
        fun saveCsv(name: String, text: String) {
            runOnUiThread {
                val ok = CsvWriter.save(this@MainActivity, name, text)
                Toast.makeText(
                    this@MainActivity,
                    if (ok) "Файл сохранён в Загрузки" else "Не удалось сохранить файл",
                    Toast.LENGTH_SHORT
                ).show()
            }
        }

        @JavascriptInterface
        fun toast(text: String) {
            runOnUiThread { Toast.makeText(this@MainActivity, text, Toast.LENGTH_SHORT).show() }
        }
    }

    override fun onResume() {
        super.onResume()
        web.onResume()
    }

    override fun onPause() {
        web.onPause()
        super.onPause()
    }

    override fun onDestroy() {
        web.destroy()
        super.onDestroy()
    }
}
