package dev.alfieprojects.gearsync

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import androidx.core.content.ContextCompat

/**
 * ADR 004 Milestone 0 — on-screen accelerometer rate readout (debug builds only).
 *
 * Exists in `src/debug/` so it never ships in release. Lets the M0 probe be read
 * with NO adb and NO cable: sideload the debug APK, open "GearSync Probe", read
 * the effective Hz on-screen. Starts the native engine (sensor thread) directly —
 * the foreground service is not required for a stationary bench probe.
 *
 * It measures sensor *capability*, not engine vibration: a desk run is valid.
 * See plans/accel-fft-sensor-fusion-implementation-plan.md (M0) and
 * NativeEngine.nativeAccelProbeStats().
 */
class DebugProbeActivity : Activity() {

    private val handler = Handler(Looper.getMainLooper())
    private lateinit var readout: TextView
    private var engineStarted = false

    private val poller = object : Runnable {
        override fun run() {
            render()
            handler.postDelayed(this, POLL_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        readout = TextView(this).apply {
            textSize = 18f
            setTextColor(Color.WHITE)
            setPadding(48, 48, 48, 48)
            typeface = android.graphics.Typeface.MONOSPACE
            gravity = Gravity.CENTER
            text = "Starting probe…"
        }
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setBackgroundColor(Color.parseColor("#0D0D1A"))
            addView(
                readout,
                LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT
                )
            )
        }
        setContentView(root)

        // startEngine opens the mic input stream too, so RECORD_AUDIO is required
        // even though the probe only reads the accelerometer.
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
            == PackageManager.PERMISSION_GRANTED
        ) {
            startEngine()
        } else {
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQ_AUDIO)
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_AUDIO &&
            grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED
        ) {
            startEngine()
        } else {
            readout.text = "RECORD_AUDIO denied — cannot start the engine/probe."
        }
    }

    private fun startEngine() {
        if (!engineStarted) {
            NativeEngine.startEngine()
            engineStarted = true
        }
        handler.post(poller)
    }

    private fun render() {
        val s = NativeEngine.nativeAccelProbeStats()
        if (s == null || s.size < 6) {
            readout.text = "No probe data yet…"
            return
        }
        val hz = s[0]; val minMs = s[1]; val maxMs = s[2]
        val jitterMs = s[3]; val samples = s[4].toLong(); val supported = s[5].toInt()

        if (supported == 0) {
            readout.setTextColor(Color.parseColor("#FF1744"))
            readout.text = "ACCELEROMETER unavailable on this device."
            return
        }
        if (supported < 0 || samples == 0L) {
            readout.text = "Warming up… (move/keep still on a desk)"
            return
        }

        val pass = hz >= GATE_HZ
        readout.setTextColor(if (pass) Color.parseColor("#00E676") else Color.parseColor("#FF1744"))
        readout.text = buildString {
            append("ADR 004 · M0 accel rate probe\n")
            append("(stationary desk run is valid)\n\n")
            append(String.format("effective:  %.1f Hz\n", hz))
            append(String.format("gate:       %.0f Hz  →  %s\n", GATE_HZ, if (pass) "PASS ✓" else "LOW ✗"))
            append(String.format("interval:   min %.2f / max %.2f ms\n", minMs, maxMs))
            append(String.format("jitter:     %.2f ms\n", jitterMs))
            append(String.format("samples:    %d\n", samples))
        }
    }

    override fun onDestroy() {
        handler.removeCallbacks(poller)
        if (engineStarted) {
            NativeEngine.stopEngine()
            engineStarted = false
        }
        super.onDestroy()
    }

    private companion object {
        const val REQ_AUDIO = 0x10
        const val POLL_MS = 500L
        const val GATE_HZ = 300f
    }
}
