package com.opendisplay.app

import android.app.Activity
import android.app.AlertDialog
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Bundle
import android.util.Log
import android.view.Gravity
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.Space
import android.widget.TextView
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.Socket
import kotlin.concurrent.thread

class MainActivity : Activity(), SurfaceHolder.Callback {
    companion object {
        private const val TAG = "OpenDisplay"
        private const val VERSION = "0.3beta"
    }

    // UI states
    enum class State { DISCONNECTED, CONNECTING, CONNECTED, STREAMING, ERROR }

    private lateinit var surfaceView: SurfaceView
    private lateinit var rootLayout: FrameLayout
    private lateinit var uiLayer: FrameLayout
    private lateinit var statusBar: LinearLayout

    // UI elements
    private lateinit var stateTitle: TextView
    private lateinit var stateSubtitle: TextView
    private lateinit var stateIcon: TextView
    private lateinit var btnAction: Button
    private lateinit var btnSettings: Button
    private lateinit var progressBar: ProgressBar
    private lateinit var statusText: TextView

    @Volatile private var surfaceReady = false
    @Volatile private var running = false
    @Volatile private var currentState = State.DISCONNECTED

    private var socket: Socket? = null
    @Volatile private var out: DataOutputStream? = null
    private var decoder: MediaCodec? = null

    private var videoW = 0
    private var videoH = 0

    // Colors
    private val bgColor = Color.parseColor("#1a1a2e")
    private val cardColor = Color.parseColor("#16213e")
    private val accentColor = Color.parseColor("#0f3460")
    private val accentBright = Color.parseColor("#e94560")
    private val textColor = Color.parseColor("#eaeaea")
    private val textDim = Color.parseColor("#8899aa")
    private val greenColor = Color.parseColor("#4ecca3")
    private val yellowColor = Color.parseColor("#f9ed69")

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        intent?.getIntExtra("port", -1)?.let { p ->
            if (p in 1..65535) Settings.setPort(this, p)
        }

        setupUI()
    }

    private fun setupUI() {
        rootLayout = FrameLayout(this).apply { setBackgroundColor(bgColor) }

        // SurfaceView for video
        surfaceView = SurfaceView(this).apply {
            holder.addCallback(this@MainActivity)
            visibility = View.GONE
        }
        rootLayout.addView(surfaceView, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT
        ))

        // Status bar (visible during streaming)
        statusBar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setBackgroundColor(Color.parseColor("#cc1a1a2e"))
            setPadding(24, 12, 24, 12)
            visibility = View.GONE
        }
        statusText = TextView(this).apply {
            text = ""
            setTextColor(textColor)
            textSize = 13f
            typeface = Typeface.MONOSPACE
        }
        statusBar.addView(statusText)
        rootLayout.addView(statusBar, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT
        ).apply { gravity = Gravity.TOP })

        // UI layer for menus
        uiLayer = FrameLayout(this)
        rootLayout.addView(uiLayer, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT
        ))

        setContentView(rootLayout)

        // Long-press on surface opens settings
        surfaceView.setOnLongClickListener {
            showSettings()
            true
        }

        showDisconnectedState()
    }

    private fun showDisconnectedState() {
        currentState = State.DISCONNECTED
        uiLayer.visibility = View.VISIBLE
        surfaceView.visibility = View.GONE
        statusBar.visibility = View.GONE
        uiLayer.removeAllViews()

        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT
            )
            setPadding(48, 48, 48, 48)
        }

        // App icon / logo
        stateIcon = TextView(this).apply {
            text = "🖥"
            textSize = 72f
            gravity = Gravity.CENTER
        }
        container.addView(stateIcon)

        addSpace(container, 24)

        stateTitle = TextView(this).apply {
            text = "OpenDisplay"
            setTextColor(textColor)
            textSize = 32f
            typeface = Typeface.DEFAULT_BOLD
            gravity = Gravity.CENTER
        }
        container.addView(stateTitle)

        addSpace(container, 8)

        TextView(this).apply {
            text = "v$VERSION"
            setTextColor(textDim)
            textSize = 14f
            gravity = Gravity.CENTER
        }.let { container.addView(it) }

        addSpace(container, 40)

        stateSubtitle = TextView(this).apply {
            text = "Подключите кабель USB\nк компьютеру"
            setTextColor(textDim)
            textSize = 18f
            gravity = Gravity.CENTER
            setLineSpacing(1.4f, 1f)
        }
        container.addView(stateSubtitle)

        addSpace(container, 48)

        btnAction = createButton("Подключить", accentBright) {
            startStream()
        }
        container.addView(btnAction)

        addSpace(container, 16)

        btnSettings = createButton("Настройки", cardColor) {
            showSettings()
        }
        container.addView(btnSettings)

        uiLayer.addView(container)
    }

    private fun showConnectingState() {
        currentState = State.CONNECTING
        uiLayer.visibility = View.VISIBLE
        surfaceView.visibility = View.GONE
        statusBar.visibility = View.GONE
        uiLayer.removeAllViews()

        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT
            )
        }

        progressBar = ProgressBar(this).apply {
            isIndeterminate = true
            indeterminateTintList = android.content.res.ColorStateList.valueOf(accentBright)
        }
        val pbParams = LinearLayout.LayoutParams(64, 64).apply { gravity = Gravity.CENTER }
        container.addView(progressBar, pbParams)

        addSpace(container, 32)

        stateTitle = TextView(this).apply {
            text = "Подключение..."
            setTextColor(textColor)
            textSize = 24f
            typeface = Typeface.DEFAULT_BOLD
            gravity = Gravity.CENTER
        }
        container.addView(stateTitle)

        addSpace(container, 12)

        stateSubtitle = TextView(this).apply {
            text = "Устанавливаем соединение\nс компьютером"
            setTextColor(textDim)
            textSize = 16f
            gravity = Gravity.CENTER
            setLineSpacing(1.4f, 1f)
        }
        container.addView(stateSubtitle)

        addSpace(container, 40)

        btnAction = createButton("Отмена", cardColor) {
            stopStream()
            showDisconnectedState()
        }
        container.addView(btnAction)

        uiLayer.addView(container)
    }

    private fun showConnectedState() {
        currentState = State.CONNECTED
        uiLayer.visibility = View.VISIBLE
        surfaceView.visibility = View.GONE
        statusBar.visibility = View.GONE
        uiLayer.removeAllViews()

        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT
            )
            setPadding(48, 48, 48, 48)
        }

        stateIcon = TextView(this).apply {
            text = "✓"
            textSize = 72f
            gravity = Gravity.CENTER
            setTextColor(greenColor)
        }
        container.addView(stateIcon)

        addSpace(container, 24)

        stateTitle = TextView(this).apply {
            text = "Дисплей подключен"
            setTextColor(greenColor)
            textSize = 28f
            typeface = Typeface.DEFAULT_BOLD
            gravity = Gravity.CENTER
        }
        container.addView(stateTitle)

        addSpace(container, 8)

        stateSubtitle = TextView(this).apply {
            text = "${videoW}x$videoH"
            setTextColor(textDim)
            textSize = 18f
            gravity = Gravity.CENTER
            typeface = Typeface.MONOSPACE
        }
        container.addView(stateSubtitle)

        addSpace(container, 48)

        btnAction = createButton("Нажмите для входа", accentBright) {
            showStreamingState()
        }
        container.addView(btnAction)

        addSpace(container, 16)

        createButton("Отключиться", cardColor) {
            stopStream()
            showDisconnectedState()
        }.let { container.addView(it) }

        uiLayer.addView(container)
    }

    private fun showStreamingState() {
        currentState = State.STREAMING
        uiLayer.visibility = View.GONE
        surfaceView.visibility = View.VISIBLE
        statusBar.visibility = View.VISIBLE
        statusText.text = "  ${videoW}x$videoH  | 长按 = настройки"

        // Hide status bar after 3 seconds
        statusBar.postDelayed({
            if (currentState == State.STREAMING) statusBar.visibility = View.GONE
        }, 3000)

        // Tap to show status bar
        surfaceView.setOnClickListener {
            statusBar.visibility = View.VISIBLE
            statusBar.postDelayed({
                if (currentState == State.STREAMING) statusBar.visibility = View.GONE
            }, 3000)
        }
    }

    private fun showErrorState(message: String) {
        currentState = State.ERROR
        uiLayer.visibility = View.VISIBLE
        surfaceView.visibility = View.GONE
        statusBar.visibility = View.GONE
        uiLayer.removeAllViews()

        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT
            )
            setPadding(48, 48, 48, 48)
        }

        stateIcon = TextView(this).apply {
            text = "⚠"
            textSize = 72f
            gravity = Gravity.CENTER
            setTextColor(accentBright)
        }
        container.addView(stateIcon)

        addSpace(container, 24)

        stateTitle = TextView(this).apply {
            text = "Ошибка"
            setTextColor(accentBright)
            textSize = 28f
            typeface = Typeface.DEFAULT_BOLD
            gravity = Gravity.CENTER
        }
        container.addView(stateTitle)

        addSpace(container, 8)

        stateSubtitle = TextView(this).apply {
            text = message
            setTextColor(textDim)
            textSize = 16f
            gravity = Gravity.CENTER
            setLineSpacing(1.4f, 1f)
        }
        container.addView(stateSubtitle)

        addSpace(container, 40)

        btnAction = createButton("Повторить", accentBright) {
            startStream()
        }
        container.addView(btnAction)

        addSpace(container, 16)

        createButton("Настройки", cardColor) {
            showSettings()
        }.let { container.addView(it) }

        uiLayer.addView(container)
    }

    private fun showSettings() {
        runOnUiThread {
            stopStream()

            val layout = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(48, 24, 48, 24)
                setBackgroundColor(bgColor)
            }

            // Quality section
            layout.addView(TextView(this).apply {
                text = "КАЧЕСТВО"
                setTextColor(textDim)
                textSize = 12f
                typeface = Typeface.DEFAULT_BOLD
                letterSpacing = 0.2f
            })
            addSpace(layout, 12)

            val qualityNames = Settings.QUALITY_PRESETS.keys.toList()
            val currentQuality = Settings.getQualityName(this)
            var selectedQuality = currentQuality

            val qualityGroup = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER
            }
            qualityNames.forEach { name ->
                val btn = Button(this).apply {
                    text = name
                    textSize = 13f
                    setPadding(16, 8, 16, 8)
                    setBackground(createChipDrawable(name == currentQuality))
                    setTextColor(if (name == currentQuality) Color.WHITE else textDim)
                    setOnClickListener {
                        selectedQuality = name
                        for (i in 0 until qualityGroup.childCount) {
                            val child = qualityGroup.getChildAt(i) as Button
                            val isSelected = child.text.toString() == name
                            child.setBackground(createChipDrawable(isSelected))
                            child.setTextColor(if (isSelected) Color.WHITE else textDim)
                        }
                    }
                }
                val params = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f).apply {
                    setMargins(4, 0, 4, 0)
                }
                qualityGroup.addView(btn, params)
            }
            layout.addView(qualityGroup)

            addSpace(layout, 24)

            // FPS section
            layout.addView(TextView(this).apply {
                text = "ЧАСТОТА КАДРОВ"
                setTextColor(textDim)
                textSize = 12f
                typeface = Typeface.DEFAULT_BOLD
                letterSpacing = 0.2f
            })
            addSpace(layout, 12)

            val currentFps = Settings.getFps(this)
            var selectedFps = currentFps

            val fpsGroup = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER
            }
            Settings.FPS_OPTIONS.forEach { fps ->
                val btn = Button(this).apply {
                    text = "$fps"
                    textSize = 14f
                    setPadding(20, 8, 20, 8)
                    setBackground(createChipDrawable(fps == currentFps))
                    setTextColor(if (fps == currentFps) Color.WHITE else textDim)
                    setOnClickListener {
                        selectedFps = fps
                        for (i in 0 until fpsGroup.childCount) {
                            val child = fpsGroup.getChildAt(i) as Button
                            val isSelected = child.text.toString() == "$fps"
                            child.setBackground(createChipDrawable(isSelected))
                            child.setTextColor(if (isSelected) Color.WHITE else textDim)
                        }
                    }
                }
                val params = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f).apply {
                    setMargins(4, 0, 4, 0)
                }
                fpsGroup.addView(btn, params)
            }
            layout.addView(fpsGroup)

            addSpace(layout, 32)

            // Info
            layout.addView(TextView(this).apply {
                text = "Длинное нажатие на видео = настройки"
                setTextColor(textDim)
                textSize = 13f
                gravity = Gravity.CENTER
            })

            addSpace(layout, 24)

            // Buttons
            val btnRow = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER
            }

            btnRow.addView(Button(this).apply {
                text = "Отмена"
                textSize = 15f
                setPadding(32, 12, 32, 12)
                setBackground(createChipDrawable(false))
                setTextColor(textDim)
                setOnClickListener { showDisconnectedState() }
            })

            addSpace(btnRow, 16)

            btnRow.addView(Button(this).apply {
                text = "Сохранить"
                textSize = 15f
                setPadding(32, 12, 32, 12)
                setBackground(createChipDrawable(true))
                setTextColor(Color.WHITE)
                setOnClickListener {
                    Settings.setQualityName(this@MainActivity, selectedQuality)
                    Settings.setFps(this@MainActivity, selectedFps)
                    showDisconnectedState()
                }
            })

            layout.addView(btnRow)

            // Wrap in ScrollView
            val scroll = ScrollView(this).apply {
                addView(layout)
            }

            uiLayer.visibility = View.VISIBLE
            surfaceView.visibility = View.GONE
            statusBar.visibility = View.GONE
            uiLayer.removeAllViews()
            uiLayer.addView(scroll)
        }
    }

    // --- Streaming ---

    override fun surfaceCreated(holder: SurfaceHolder) {
        surfaceReady = true
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
        showConnectingState()
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
                Log.i(TAG, "handshake: ${videoW}x${videoH} @${hz}Hz codec=$codec")

                runOnUiThread { showConnectedState() }

                // Wait for surface to be ready before starting decoder
                while (running && !surfaceReady) Thread.sleep(50)

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
                if (running) {
                    runOnUiThread { showErrorState("Не удалось подключиться.\nПроверьте кабель и запущенный хост.") }
                }
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

    // --- Touch ---

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (currentState != State.STREAMING) return super.onTouchEvent(event)
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

    // --- Helpers ---

    private fun createButton(text: String, bgColor: Int, onClick: () -> Unit): Button {
        return Button(this).apply {
            this.text = text
            textSize = 16f
            setTextColor(Color.WHITE)
            isAllCaps = false
            setBackground(createRoundDrawable(bgColor, 12f))
            setPadding(48, 16, 48, 16)
            setOnClickListener { onClick() }
            layoutParams = LinearLayout.LayoutParams(
                320, LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply { gravity = Gravity.CENTER }
        }
    }

    private fun createRoundDrawable(color: Int, radius: Float): GradientDrawable {
        return GradientDrawable().apply {
            setColor(color)
            cornerRadius = radius * resources.displayMetrics.density
        }
    }

    private fun createChipDrawable(selected: Boolean): GradientDrawable {
        return GradientDrawable().apply {
            setColor(if (selected) accentBright else cardColor)
            cornerRadius = 8f * resources.displayMetrics.density
            setStroke(1, if (selected) accentBright else Color.parseColor("#333355"))
        }
    }

    private fun addSpace(parent: LinearLayout, dp: Int) {
        parent.addView(Space(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp
            )
        })
    }

    // --- Protocol helpers ---

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
