package dev.alfieprojects.gearsync

// CalibrationActivity — guided per-gear calibration UI.
//
// Flow: gear grid -> tap a gear -> capture screen (progress ring, ~10 Hz poll) ->
//       RANSAC locks -> confirmation shown; or user cancels at any point.
//
// Requires RECORD_AUDIO and location permissions (already granted to the running
// ShiftAssistantService); finishes immediately if permissions are absent.
// (ref: DL-004)

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.Choreographer
import android.view.View
import android.widget.Button
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat

class CalibrationActivity : AppCompatActivity(), NativeEngine.CalibrationListener {

    private lateinit var gearButtons: List<Button>
    private lateinit var statusText: TextView
    private lateinit var progressRing: ProgressRingView
    private lateinit var btnCancel: Button

    private var activeGear: Int = -1

    private val choreographerCallback = object : Choreographer.FrameCallback {
        // ~10 Hz poll of native capture progress while a gear is being captured.
        // Self-reposts only while activeGear >= 0, so it pauses when idle/not visible.
        override fun doFrame(frameTimeNanos: Long) {
            if (activeGear >= 0) {
                progressRing.setProgress(NativeEngine.getCalibrationProgress())
                Choreographer.getInstance().postFrameCallback(this)
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_calibration)

        if (!hasRequiredPermissions()) {
            Toast.makeText(this, R.string.calib_permissions_required, Toast.LENGTH_LONG).show()
            finish()
            return
        }

        gearButtons = listOf(
            findViewById(R.id.btnGear1),
            findViewById(R.id.btnGear2),
            findViewById(R.id.btnGear3),
            findViewById(R.id.btnGear4),
            findViewById(R.id.btnGear5)
        )
        statusText   = findViewById(R.id.tvCalibStatus)
        progressRing = findViewById(R.id.progressRing)
        btnCancel    = findViewById(R.id.btnCancelCalib)

        gearButtons.forEachIndexed { index, button ->
            button.setOnClickListener { startCapture(index) }
        }
        btnCancel.setOnClickListener { cancelCapture() }

        NativeEngine.calibrationListener = this
        showGearGrid()
    }

    override fun onDestroy() {
        NativeEngine.cancelCalibration()
        NativeEngine.calibrationListener = null
        Choreographer.getInstance().removeFrameCallback(choreographerCallback)
        super.onDestroy()
    }

    private fun startCapture(gearIndex: Int) {
        activeGear = gearIndex
        NativeEngine.beginGearCalibration(gearIndex)
        statusText.text = getString(R.string.calib_capturing, gearIndex + 1)
        progressRing.setProgress(0f)
        progressRing.visibility = View.VISIBLE
        btnCancel.visibility = View.VISIBLE
        gearButtons.forEach { it.isEnabled = false }
        Choreographer.getInstance().postFrameCallback(choreographerCallback)
    }

    private fun cancelCapture() {
        activeGear = -1
        NativeEngine.cancelCalibration()
        Choreographer.getInstance().removeFrameCallback(choreographerCallback)
        showGearGrid()
    }

    private fun showGearGrid() {
        activeGear = -1
        statusText.text = getString(R.string.calib_pick_gear)
        progressRing.visibility = View.GONE
        btnCancel.visibility = View.GONE
        gearButtons.forEach { it.isEnabled = true }
    }

    // NativeEngine.CalibrationListener implementation.
    // Called on the main thread (NativeEngine.onGearCalibrated marshals via Handler).
    // gear < 0 means the RANSAC fit was rejected for breaking monotonic order. (ref: DL-005)
    override fun onGearCalibrated(gear: Int) {
        activeGear = -1
        Choreographer.getInstance().removeFrameCallback(choreographerCallback)
        if (gear < 0) {
            progressRing.setProgress(0f)
            Toast.makeText(this, R.string.calib_lock_failed, Toast.LENGTH_LONG).show()
            showGearGrid()
        } else {
            progressRing.setProgress(1f)
            statusText.text = getString(R.string.calib_locked, gear + 1)
            gearButtons.getOrNull(gear)?.isEnabled = false
            btnCancel.visibility = View.GONE
        }
    }

    private fun hasRequiredPermissions(): Boolean {
        return ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) ==
                PackageManager.PERMISSION_GRANTED &&
               ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) ==
                PackageManager.PERMISSION_GRANTED
    }
}
