package dev.alfieprojects.gearsync

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.media.AudioManager
import android.os.Build
import android.os.Bundle
import android.os.VibrationEffect
import android.os.Vibrator
import android.widget.Button
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat

class MainActivity : AppCompatActivity() {

    private val requiredPermissions = buildList {
        add(Manifest.permission.RECORD_AUDIO)
        add(Manifest.permission.ACCESS_FINE_LOCATION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            add(Manifest.permission.POST_NOTIFICATIONS)
    }.toTypedArray()

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { grants ->
        val denied = grants.filterValues { !it }.keys
        if (denied.isEmpty()) startShiftService()
        else Toast.makeText(
            this,
            getString(R.string.permissions_required, denied.joinToString()),
            Toast.LENGTH_LONG
        ).show()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        // Route the hardware volume rocker to the media stream so it adjusts the
        // ADR 006 audio cues (they play on STREAM_MUSIC) anytime the app is foreground.
        volumeControlStream = AudioManager.STREAM_MUSIC

        findViewById<Button>(R.id.btnStart).setOnClickListener { checkAndStart() }
        findViewById<Button>(R.id.btnStop).setOnClickListener  { stopShiftService() }

        val calibrate = findViewById<Button>(R.id.btnCalibrate)
        // Single tap → calibrate (one short buzz). Long-press → toggle the hidden
        // demo mode (distinct double buzz), replacing the old VU-meter triple-tap.
        calibrate.setOnClickListener {
            vibrate(CALIBRATE_TAP_MS)
            openCalibration()
        }
        calibrate.setOnLongClickListener {
            NativeEngine.demoMode = !NativeEngine.demoMode
            vibrateDemoToggle()
            Toast.makeText(
                this,
                if (NativeEngine.demoMode) R.string.demo_on else R.string.demo_off,
                Toast.LENGTH_SHORT
            ).show()
            true   // consume so the click (calibrate) does not also fire
        }
    }

    // ─── Haptics ───────────────────────────────────────────────────────────────

    private val vibrator: Vibrator? by lazy {
        @Suppress("DEPRECATION")
        getSystemService(VIBRATOR_SERVICE) as? Vibrator
    }

    private fun vibrate(ms: Long) {
        vibrator?.takeIf { it.hasVibrator() }
            ?.vibrate(VibrationEffect.createOneShot(ms, VibrationEffect.DEFAULT_AMPLITUDE))
    }

    /** Distinct double-buzz so demo-toggle feels different from a calibrate tap. */
    private fun vibrateDemoToggle() {
        vibrator?.takeIf { it.hasVibrator() }
            ?.vibrate(VibrationEffect.createWaveform(longArrayOf(0, 40, 60, 40), -1))
    }

    private fun checkAndStart() {
        val missing = requiredPermissions.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) startShiftService()
        else permissionLauncher.launch(missing.toTypedArray())
    }

    private fun startShiftService() {
        val intent = Intent(this, ShiftAssistantService::class.java)
        ContextCompat.startForegroundService(this, intent)
        Toast.makeText(this, R.string.service_started, Toast.LENGTH_SHORT).show()
    }

    private fun stopShiftService() {
        stopService(Intent(this, ShiftAssistantService::class.java))
        Toast.makeText(this, R.string.service_stopped, Toast.LENGTH_SHORT).show()
    }

    // Guard against opening CalibrationActivity when the service is not running:
    // NativeEngine.beginGearCalibration would silently no-op and the UI would
    // show no progress. (ref: DL-004)
    private fun openCalibration() {
        if (!ShiftAssistantService.isRunning) {
            Toast.makeText(this, R.string.calib_service_not_running, Toast.LENGTH_LONG).show()
            return
        }
        startActivity(Intent(this, CalibrationActivity::class.java))
    }

    private companion object {
        const val CALIBRATE_TAP_MS = 30L
    }
}
