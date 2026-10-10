package com.opendisplay.app

import android.app.Activity
import android.app.AlertDialog
import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Bundle
import android.util.Log
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.Socket
import kotlin.concurrent.thread

class MainActivity : Activity(), SurfaceHolder.Callback {
    companion object {
        private const val TAG = "OpenDisplay"
    }

    private lateinit var surfaceView: SurfaceView
    @Volatile private var surfaceReady = false
    @Volatile private var running = false

    private var socket: Socket? = null
    @Volatile private var out: DataOutputStream? = null
    private var decoder: MediaCodec? = null

    private var videoW = 0
    private var videoH = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        // Read port from intent
        intent?.getIntExtra("port", -1)?.let { p ->
            if (p in 1..65535) Settings.setPort(this, p)
        }

        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        setContentView(surfaceView)

        // Long-press to open settings
        surfaceView.setOnLongClickListener {
            showSettingsDialog()
            true
        }
    }

    private fun showSettingsDialog() {
        stopStream()

        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 24, 48, 24)
        }

        val fpsLabel = TextView(this).apply { text = "FPS: ${Settings.getFps(this@MainActivity)}" }
        val fpsEdit = EditText(this).apply {
            setText(Settings.getFps(this@MainActivity).toString())
            inputType = android.text.InputType.TYPE_CLASS_NUMBER
        }

        val qpLabel = TextView(this).apply { text = "Quality (QP, lower = better): ${Settings.getQp(this@MainActivity)}" }
        val qpEdit = EditText(this).apply {
            setText(Settings.getQp(this@MainActivity).toString())
            inputType = android.text.InputType.TYPE_CLASS_NUMBER
        }

        val hostLabel = TextView(this).apply { text = "Host IP:" }
        val hostEdit = EditText(this).apply {
            setText(Settings.getHost(this@MainActivity))
            inputType = android.text.InputType.TYPE_CLASS_TEXT
        }

        val portLabel = TextView(this).apply { text = "Port:" }
        val portEdit = EditText(this).apply {
            setText(Settings.getPort(this@MainActivity).toString())
            inputType = android.text.InputType.TYPE_CLASS_NUMBER
        }

        layout.addView(fpsLabel)
        layout.addView(fpsEdit)
        layout.addView(qpLabel)
        layout.addView(qpEdit)
        layout.addView(hostLabel)
        layout.addView(hostEdit)
        layout.addView(portLabel)
        layout.addView(portEdit)

        AlertDialog.Builder(this)
            .setTitle("OpenDisplay Settings")
            .setView(layout)
            .setPositiveButton("Save") { _, _ ->
                val fps = fpsEdit.text.toString().toIntOrNull()?.coerceIn(15, 240) ?: 120
                val qp = qpEdit.text.toString().toIntOrNull()?.coerceIn(0, 51) ?: 12
                val host = hostEdit.text.toString().trim().ifEmpty { "127.0.0.1" }
                val port = portEdit.text.toString().toIntOrNull()?.coerceIn(1, 65535) ?: 27183

                Settings.setFps(this, fps)
                Settings.setQp(this, qp)
                Settings.setHost(this, host)
                Settings.setPort(this, port)

                fpsLabel.text = "FPS: $fps"
                qpLabel.text = "Quality (QP, lower = better): $qp"
                hostLabel.text = "Host IP: $host"
                portLabel.text = "Port: $port"

                stopStream()
                startStream()
            }
            .setNegativeButton("Cancel") { _, _ -> startStream() }
            .setOnCancelListener { startStream() }
            .show()
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        surfaceReady = true
        startStream()
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        surfaceReady = false
        stopStream()
    }

    override fun onDestroy() {
        stopStream()
        super.onDestroy()
    }

    private fun startStream() {
        if (running) return
        running = true
        thread(name = "od-stream") { streamLoop() }
    }

    private fun stopStream() {
        running = false
        try { socket?.close() } catch (_: Exception) {}
        try { decoder?.stop(); decoder?.release() } catch (_: Exception) {}
        decoder = null
        socket = null
        out = null
    }

    private fun streamLoop() {
        while (running) {
            try {
                val host = Settings.getHost(this)
                val port = Settings.getPort(this)
                Log.i(TAG, "connecting to $host:$port...")
                val s = Socket(host, port)
                socket = s
                s.tcpNoDelay = true
                val input = DataInputStream(s.getInputStream())
                val output = DataOutputStream(s.getOutputStream())
                out = output

                val fps = Settings.getFps(this)
                sendCaps(output, 1920, 1080, fps, Proto.CODEC_HEVC)
                Log.i(TAG, "caps sent, waiting for handshake...")

                // Read handshake
                val hs = ByteArray(Proto.HS_HEADER_SIZE)
                input.readFully(hs)
                val magic = readBe32(hs, 0)
                if (magic != Proto.MAGIC.toLong()) {
                    Log.e(TAG, "bad magic: $magic")
                    s.close()
                    continue
                }
                videoW = readBe16(hs, 8)
                videoH = readBe16(hs, 10)
                val hz = readBe16(hs, 12)
                val codec = hs[6].toInt() and 0xFF
                Log.i(TAG, "handshake: ${videoW}x$videoH @${hz}Hz codec=$codec")

                val mime = if (codec == Proto.CODEC_HEVC) MediaFormat.MIMETYPE_VIDEO_HEVC
                           else MediaFormat.MIMETYPE_VIDEO_AVC
                val fmt = MediaFormat.createVideoFormat(mime, videoW, videoH)
                fmt.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
                fmt.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, videoW * videoH)
                val mc = MediaCodec.createDecoderByType(mime)
                mc.configure(fmt, surfaceView.holder.surface, null, 0)
                mc.start()
                decoder = mc

                val info = MediaCodec.BufferInfo()

                while (running) {
                    val header = ByteArray(Proto.VID_HEADER_SIZE)
                    input.readFully(header)
                    val ptsFlags = readBe64(header, 0)
                    val size = readBe32(header, 8).toInt()
                    if (size <= 0 || size > 64 * 1024 * 1024) {
                        Log.e(TAG, "bad packet size $size")
                        break
                    }
                    val payload = ByteArray(size)
                    input.readFully(payload)

                    val isConfig = (ptsFlags and Proto.PACKET_FLAG_CONFIG) != 0L
                    val isKey = (ptsFlags and Proto.PACKET_FLAG_KEY_FRAME) != 0L

                    val inIdx = mc.dequeueInputBuffer(10_000)
                    if (inIdx >= 0) {
                        val inBuf = mc.getInputBuffer(inIdx)!!
                        inBuf.clear()
                        inBuf.put(payload)
                        val flags = if (isConfig) MediaCodec.BUFFER_FLAG_CODEC_CONFIG
                                     else if (isKey) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0
                        mc.queueInputBuffer(inIdx, 0, size, 0, flags)
                    }

                    var outIdx = mc.dequeueOutputBuffer(info, 0)
                    while (outIdx >= 0) {
                        mc.releaseOutputBuffer(outIdx, true)
                        outIdx = mc.dequeueOutputBuffer(info, 0)
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "stream error: ${e.message}")
            } finally {
                try { decoder?.stop(); decoder?.release() } catch (_: Exception) {}
                decoder = null
                try { socket?.close() } catch (_: Exception) {}
                socket = null
                out = null
            }
            if (running) {
                Log.i(TAG, "reconnecting in 2s...")
                Thread.sleep(2000)
            }
        }
    }

    private fun sendCaps(out: DataOutputStream, maxW: Int, maxH: Int, maxHz: Int, codecFlags: Int) {
        val payload = ByteArray(8)
        writeBe16(payload, 0, maxW)
        writeBe16(payload, 2, maxH)
        writeBe16(payload, 4, maxHz)
        payload[6] = codecFlags.toByte()
        payload[7] = 0

        out.writeByte(Proto.CTRL_CAPS)
        writeBe32(out, payload.size)
        out.write(payload)
        out.flush()
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        val o = out ?: return super.onTouchEvent(event)
        try {
            synchronized(o) {
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                        val idx = event.actionIndex
                        sendTouch(o, Proto.TOUCH_DOWN, event.getPointerId(idx),
                            event.getX(idx), event.getY(idx))
                    }
                    MotionEvent.ACTION_MOVE -> {
                        for (i in 0 until event.pointerCount) {
                            sendTouch(o, Proto.TOUCH_MOVE, event.getPointerId(i),
                                event.getX(i), event.getY(i))
                        }
                    }
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> {
                        val idx = event.actionIndex
                        sendTouch(o, Proto.TOUCH_UP, event.getPointerId(idx),
                            event.getX(idx), event.getY(idx))
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "touch send failed", e)
        }
        return true
    }

    private fun sendTouch(out: DataOutputStream, action: Int, pointerId: Int, x: Float, y: Float) {
        val rx = (x / surfaceView.width).coerceIn(0f, 1f)
        val ry = (y / surfaceView.height).coerceIn(0f, 1f)

        val payload = ByteArray(18)
        payload[0] = action.toByte()
        payload[1] = 0
        writeBe32(payload, 2, pointerId)
        writeFloatBE(payload, 6, rx)
        writeFloatBE(payload, 10, ry)
        writeFloatBE(payload, 14, 1f)

        out.writeByte(Proto.CTRL_TOUCH)
        writeBe32(out, payload.size)
        out.write(payload)
        out.flush()
    }

    private fun readBe16(b: ByteArray, off: Int): Int =
        ((b[off].toInt() and 0xFF) shl 8) or (b[off + 1].toInt() and 0xFF)

    private fun readBe32(b: ByteArray, off: Int): Long =
        ((b[off].toLong() and 0xFF) shl 24) or
        ((b[off + 1].toLong() and 0xFF) shl 16) or
        ((b[off + 2].toLong() and 0xFF) shl 8) or
        (b[off + 3].toLong() and 0xFF)

    private fun readBe64(b: ByteArray, off: Int): Long {
        var v = 0L
        for (i in 0 until 8) v = (v shl 8) or (b[off + i].toLong() and 0xFF)
        return v
    }

    private fun writeBe16(b: ByteArray, off: Int, v: Int) {
        b[off] = ((v shr 8) and 0xFF).toByte()
        b[off + 1] = (v and 0xFF).toByte()
    }

    private fun writeBe32(b: ByteArray, off: Int, v: Int) {
        b[off] = ((v shr 24) and 0xFF).toByte()
        b[off + 1] = ((v shr 16) and 0xFF).toByte()
        b[off + 2] = ((v shr 8) and 0xFF).toByte()
        b[off + 3] = (v and 0xFF).toByte()
    }

    private fun writeBe32(out: DataOutputStream, v: Int) {
        out.writeByte((v shr 24) and 0xFF)
        out.writeByte((v shr 16) and 0xFF)
        out.writeByte((v shr 8) and 0xFF)
        out.writeByte(v and 0xFF)
    }

    private fun writeFloatBE(b: ByteArray, off: Int, f: Float) {
        val bits = java.lang.Float.floatToIntBits(f)
        writeBe32(b, off, bits)
    }
}
