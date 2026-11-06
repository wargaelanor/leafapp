package com.leafcontrol.app.net

import com.leafcontrol.app.data.Settings
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import java.io.ByteArrayOutputStream
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import kotlin.random.Random

/**
 * Minimal MQTT 3.1.1 client over WebSocket (WSS), matching the PWA's broker:
 * wss://host:port, topics ovms/<user>/<vehicle>/metric/# and client/#.
 *
 * Only the packets this app needs are implemented: CONNECT/CONNACK,
 * SUBSCRIBE/SUBACK, PUBLISH (QoS 0), PINGREQ/PINGRESP and DISCONNECT.
 */
class MqttWssClient(
    private val host: String,
    private val port: Int,
    private val path: String,
    private val user: String,
    private val pass: String,
    private val vehicleId: String,
    private val onConnected: (Boolean) -> Unit,
    private val onMetric: (String, String) -> Unit,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val http = OkHttpClient.Builder()
        .connectTimeout(12, TimeUnit.SECONDS)
        .readTimeout(0, TimeUnit.MILLISECONDS)
        .pingInterval(0, TimeUnit.MILLISECONDS)
        .build()
    private val socket = AtomicReference<WebSocket?>(null)
    private val running = AtomicBoolean(false)
    private val mqttReady = AtomicBoolean(false)
    private var reconnectJob: Job? = null
    private var keepaliveJob: Job? = null
    private var lastPongMs = 0L
    private var generation = 0
    private var alternatePath = false
    private val clientId = "leaf-android-" + Random.nextInt(0x100000, 0xFFFFFF).toString(16)

    private val base get() = "ovms/$user/${vehicleId.ifEmpty { "leaf" }}/"

    fun start() {
        if (!running.compareAndSet(false, true)) return
        connect()
        keepaliveJob = scope.launch {
            while (running.get()) {
                delay(25_000)
                if (!mqttReady.get()) continue
                send(packet(0xC0, ByteArray(0))) // PINGREQ
                if (lastPongMs > 0 && System.currentTimeMillis() - lastPongMs > 90_000) {
                    // Broker stopped answering; rebuild the socket.
                    socket.get()?.close(4000, "keepalive")
                }
            }
        }
    }

    fun stop() {
        running.set(false)
        reconnectJob?.cancel()
        keepaliveJob?.cancel()
        mqttReady.set(false)
        onConnected(false)
        val ws = socket.getAndSet(null)
        try {
            ws?.send(bytesToByteString(packet(0xE0, ByteArray(0)))) // DISCONNECT
            ws?.close(1000, "bye")
        } catch (_: Exception) {
        }
    }

    fun publishCommand(payload: String): Boolean {
        if (!mqttReady.get()) return false
        return send(publishPacket(base + "client/pwa/command/c", payload))
    }

    private fun connect() {
        if (!running.get()) return
        val gen = ++generation
        // The PWA uses "/" but some brokers expect "/mqtt"; try both.
        val firstPath = if (path.startsWith("/")) path else "/$path"
        val wsPath = if (alternatePath) "/mqtt" else firstPath
        val cleanHost = host.trim()
            .removePrefix("wss://").removePrefix("ws://")
            .substringBefore('/').substringBefore(':')
        val url = "wss://$cleanHost:${if (port > 0) port else 15871}$wsPath"
        val req = Request.Builder()
            .url(url)
            .header("Sec-WebSocket-Protocol", "mqtt")
            .build()
        lastPongMs = 0
        val listener = object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                if (gen != generation) { webSocket.close(1000, "stale"); return }
                socket.set(webSocket)
                send(connectPacket())
            }

            override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
                if (gen != generation) return
                handlePackets(bytes.toByteArray())
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                // MQTT is binary; ignore text frames from proxies.
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                if (gen != generation) return
                mqttReady.set(false)
                alternatePath = !alternatePath
                onConnected(false)
                socket.compareAndSet(webSocket, null)
                scheduleReconnect()
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                if (gen != generation) return
                mqttReady.set(false)
                alternatePath = !alternatePath
                onConnected(false)
                socket.compareAndSet(webSocket, null)
                scheduleReconnect()
            }
        }
        http.newWebSocket(req, listener)
    }

    private fun scheduleReconnect() {
        if (!running.get()) return
        if (reconnectJob?.isActive == true) return
        reconnectJob = scope.launch {
            delay(5_000)
            if (running.get()) connect()
        }
    }

    private fun send(data: ByteArray): Boolean {
        val ws = socket.get() ?: return false
        return try {
            ws.send(bytesToByteString(data))
        } catch (_: Exception) {
            false
        }
    }

    private fun handlePackets(data: ByteArray) {
        var off = 0
        while (off < data.size) {
            val first = data[off].toInt() and 0xFF
            val rl = decodeRemainingLength(data, off + 1) ?: return
            val headerLen = 1 + rl.second
            val end = headerLen + rl.first
            if (end > data.size) return
            val body = data.copyOfRange(headerLen, end)
            when (first and 0xF0) {
                0x20 -> { // CONNACK
                    if (body.size >= 2 && body[1].toInt() == 0) {
                        mqttReady.set(true)
                alternatePath = false
                        lastPongMs = System.currentTimeMillis()
                        onConnected(true)
                        subscribe(base + "metric/#")
                        subscribe(base + "client/#")
                    } else {
                        onConnected(false)
                        socket.get()?.close(4001, "mqtt refused")
                    }
                }
                0x30 -> handlePublish(body) // PUBLISH
                0xD0 -> lastPongMs = System.currentTimeMillis() // PINGRESP
            }
            off = end
        }
    }

    private fun handlePublish(body: ByteArray) {
        if (body.size < 2) return
        val topicLen = ((body[0].toInt() and 0xFF) shl 8) or (body[1].toInt() and 0xFF)
        if (body.size < 2 + topicLen) return
        val topic = String(body, 2, topicLen, Charsets.UTF_8)
        val payload = String(body, 2 + topicLen, body.size - 2 - topicLen, Charsets.UTF_8)
        onMetric(topic, payload)
    }

    private fun subscribe(topic: String) {
        val body = ByteArrayOutputStream()
        body.write(0x00); body.write(0x01) // packet id
        writeUtf(body, topic)
        body.write(0x00) // QoS 0
        send(packet(0x82, body.toByteArray()))
    }

    private fun publishPacket(topic: String, payload: String): ByteArray {
        val body = ByteArrayOutputStream()
        writeUtf(body, topic)
        body.write(payload.toByteArray(Charsets.UTF_8))
        return packet(0x30, body.toByteArray())
    }

    private fun connectPacket(): ByteArray {
        val body = ByteArrayOutputStream()
        writeUtf(body, "MQTT")
        body.write(0x04) // level 3.1.1
        var flags = 0x02 // clean session
        if (user.isNotEmpty()) flags = flags or 0x80
        if (pass.isNotEmpty()) flags = flags or 0x40
        body.write(flags)
        body.write(0x00); body.write(0x3C) // keepalive 60 s
        writeUtf(body, clientId)
        if (user.isNotEmpty()) writeUtf(body, user)
        if (pass.isNotEmpty()) writeUtf(body, pass)
        return packet(0x10, body.toByteArray())
    }

    private fun packet(first: Int, body: ByteArray): ByteArray {
        val rl = encodeRemainingLength(body.size)
        val out = ByteArray(1 + rl.size + body.size)
        out[0] = first.toByte()
        rl.copyInto(out, 1)
        body.copyInto(out, 1 + rl.size)
        return out
    }

    private fun writeUtf(out: ByteArrayOutputStream, value: String) {
        val bytes = value.toByteArray(Charsets.UTF_8)
        out.write((bytes.size shr 8) and 0xFF)
        out.write(bytes.size and 0xFF)
        out.write(bytes)
    }

    private fun encodeRemainingLength(length: Int): ByteArray {
        var l = length
        val out = ArrayList<Byte>()
        do {
            var b = l % 128
            l /= 128
            if (l > 0) b = b or 0x80
            out.add(b.toByte())
        } while (l > 0)
        return out.toByteArray()
    }

    /** Returns (remainingLength, bytesUsed). */
    private fun decodeRemainingLength(data: ByteArray, start: Int): Pair<Int, Int>? {
        var multiplier = 1
        var value = 0
        var i = start
        while (i < data.size) {
            val b = data[i].toInt() and 0xFF
            value += (b and 0x7F) * multiplier
            if ((b and 0x80) == 0) return value to (i - start + 1)
            multiplier *= 128
            if (multiplier > 128 * 128 * 128) return null
            i++
        }
        return null
    }

    private fun bytesToByteString(data: ByteArray): ByteString = ByteString.of(*data)
}
