package com.leafcontrol.app

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Handler
import android.os.Looper
import java.net.HttpURLConnection
import java.net.Inet4Address
import java.net.NetworkInterface
import java.net.URL
import java.util.Collections
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/**
 * Finds the board without asking the user for an IP. mDNS is tried first
 * (the firmware advertises _http._tcp as "leaf"), then a short /24 scan on the
 * current Wi-Fi is available for the "Найти плату" button.
 */
object BoardDiscovery {
    data class Board(val host: String, val port: Int)

    fun discoverNsd(
        context: Context,
        timeoutMs: Long = 3500,
        onResult: (Board?) -> Unit,
    ) {
        val nsd = context.getSystemService(Context.NSD_SERVICE) as? NsdManager
        if (nsd == null) {
            onResult(null)
            return
        }
        val done = AtomicBoolean(false)
        val handler = Handler(Looper.getMainLooper())
        lateinit var listener: NsdManager.DiscoveryListener

        fun finish(board: Board?) {
            if (!done.compareAndSet(false, true)) return
            handler.removeCallbacksAndMessages(null)
            try {
                nsd.stopServiceDiscovery(listener)
            } catch (_: Exception) {
            }
            onResult(board)
        }

        listener = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String?) {}
            override fun onStartDiscoveryFailed(serviceType: String?, errorCode: Int) {
                finish(null)
            }
            override fun onStopDiscoveryFailed(serviceType: String?, errorCode: Int) {}
            override fun onDiscoveryStopped(serviceType: String?) {}
            override fun onServiceLost(service: NsdServiceInfo?) {}

            override fun onServiceFound(service: NsdServiceInfo?) {
                val info = service ?: return
                val name = info.serviceName ?: ""
                if (!info.serviceType.startsWith("_http._tcp")) return
                if (!name.contains("leaf", ignoreCase = true)) return
                try {
                    nsd.resolveService(info, object : NsdManager.ResolveListener {
                        override fun onResolveFailed(serviceInfo: NsdServiceInfo?, errorCode: Int) {}
                        override fun onServiceResolved(serviceInfo: NsdServiceInfo?) {
                            @Suppress("DEPRECATION")
                            val host = serviceInfo?.host?.hostAddress ?: return
                            finish(Board(host, serviceInfo.port))
                        }
                    })
                } catch (_: Exception) {
                }
            }
        }

        handler.postDelayed({ finish(null) }, timeoutMs)
        try {
            nsd.discoverServices("_http._tcp", NsdManager.PROTOCOL_DNS_SD, listener)
        } catch (_: Exception) {
            finish(null)
        }
    }

    /** Blocking /24 scan; call from a background thread. */
    fun scanSubnet(context: Context, timeoutMs: Int = 450): Board? {
        val wifi = wifiNetwork(context)
        for (prefix in localPrefixes()) {
            val found = AtomicReference<Board?>(null)
            val pool = Executors.newFixedThreadPool(48)
            for (i in 1..254) {
                val ip = "$prefix.$i"
                pool.submit {
                    if (found.get() == null && probe(ip, timeoutMs, wifi)) {
                        found.compareAndSet(null, Board(ip, 80))
                    }
                }
            }
            pool.shutdown()
            try {
                pool.awaitTermination(9, TimeUnit.SECONDS)
            } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
            }
            pool.shutdownNow()
            found.get()?.let { return it }
        }
        return null
    }

    private fun wifiNetwork(context: Context): Network? {
        val cm = context.getSystemService(ConnectivityManager::class.java) ?: return null
        return cm.allNetworks.firstOrNull { n ->
            cm.getNetworkCapabilities(n)?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
        }
    }

    private fun localPrefixes(): List<String> {
        val prefixes = LinkedHashSet<String>()
        try {
            for (ni in Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp || ni.isLoopback) continue
                for (address in Collections.list(ni.inetAddresses)) {
                    if (address !is Inet4Address) continue
                    if (address.isLoopbackAddress || !address.isSiteLocalAddress) continue
                    val b = address.address
                    prefixes.add(
                        "${b[0].toInt() and 0xff}.${b[1].toInt() and 0xff}.${b[2].toInt() and 0xff}"
                    )
                }
            }
        } catch (_: Exception) {
        }
        // The board's own access point is 192.168.4.x; try it last.
        prefixes.add("192.168.4")
        return prefixes.take(3)
    }

    private fun probe(host: String, timeoutMs: Int, network: Network?): Boolean {
        return try {
            val url = URL("http://$host/api/status")
            val c = (network?.openConnection(url) ?: url.openConnection()) as HttpURLConnection
            c.connectTimeout = timeoutMs
            c.readTimeout = timeoutMs
            c.requestMethod = "GET"
            val ok = c.responseCode == 200 &&
                c.inputStream.bufferedReader().use { it.readText().contains("\"board\"") }
            c.disconnect()
            ok
        } catch (_: Exception) {
            false
        }
    }
}
