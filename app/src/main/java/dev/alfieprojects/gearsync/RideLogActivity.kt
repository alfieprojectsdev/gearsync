package dev.alfieprojects.gearsync

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.os.VibrationEffect
import android.os.Vibrator
import android.view.WindowManager
import android.widget.Button
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import java.io.File
import java.util.Locale

/**
 * Mountain-mode M0 passenger tap panel. Starts/stops RideLogService and sends
 * labels to it. Each tap's timestamp is taken here, at the tap, so delivery delay
 * doesn't shift it. Keeps the screen on while visible.
 */
class RideLogActivity : AppCompatActivity() {

    private val requiredPermissions = buildList {
        add(Manifest.permission.ACCESS_FINE_LOCATION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            add(Manifest.permission.POST_NOTIFICATIONS)
    }.toTypedArray()

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { grants ->
        if (grants[Manifest.permission.ACCESS_FINE_LOCATION] == true) startLog()
        else Toast.makeText(this, R.string.ridelog_permissions_required, Toast.LENGTH_LONG).show()
    }

    private lateinit var status: TextView
    private lateinit var startStop: Button
    private lateinit var modeButton: Button
    private lateinit var labelButtons: List<Button>

    private var mode = RideMode.COACH_LABELS

    private val uiHandler = Handler(Looper.getMainLooper())
    private var lastRefreshNs = 0L
    private var lastAccel = 0L
    private var lastGyro = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_ride_log)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        status = findViewById(R.id.tvRideStatus)
        startStop = findViewById(R.id.btnRideStartStop)
        modeButton = findViewById(R.id.btnRideMode)

        mode = getPreferences(MODE_PRIVATE).getString(PREF_MODE, null)
            ?.let { code -> RideMode.entries.firstOrNull { it.code == code } }
            ?: RideMode.COACH_LABELS

        val bindings = listOf(
            R.id.btnBuild to RideLabel.BUILD,
            R.id.btnHold to RideLabel.HOLD,
            R.id.btnEase to RideLabel.EASE,
            R.id.btnFloor to RideLabel.FLOOR,
            R.id.btnBrake to RideLabel.BRAKE,
            R.id.btnRideGear1 to RideLabel.GEAR_1,
            R.id.btnRideGear2 to RideLabel.GEAR_2,
            R.id.btnRideGear3 to RideLabel.GEAR_3,
            R.id.btnRideGear4 to RideLabel.GEAR_4,
            R.id.btnRideGear5 to RideLabel.GEAR_5,
            R.id.btnMark to RideLabel.MARK,
            R.id.btnUndo to RideLabel.UNDO
        )
        labelButtons = bindings.map { (id, label) ->
            findViewById<Button>(id).apply { setOnClickListener { sendLabel(label) } }
        }

        modeButton.setOnClickListener {
            mode = RideMode.entries[(mode.ordinal + 1) % RideMode.entries.size]
            getPreferences(MODE_PRIVATE).edit().putString(PREF_MODE, mode.code).apply()
            render()
        }
        startStop.setOnClickListener {
            if (RideLogService.Stats.recording) stopLog() else checkAndStart()
        }
    }

    override fun onResume() {
        super.onResume()
        uiHandler.post(refresh)
    }

    override fun onPause() {
        uiHandler.removeCallbacks(refresh)
        super.onPause()
    }

    // ─── Actions ──────────────────────────────────────────────────────────────

    private fun sendLabel(label: RideLabel) {
        val tNs = SystemClock.elapsedRealtimeNanos()   // first: the tap instant is the datum
        if (!RideLogService.Stats.recording) return
        vibrate(if (label == RideLabel.UNDO) UNDO_BUZZ_MS else TAP_BUZZ_MS)
        startService(RideLogService.labelIntent(this, label, tNs))
    }

    private fun checkAndStart() {
        val missing = requiredPermissions.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) startLog() else permissionLauncher.launch(missing.toTypedArray())
    }

    private fun startLog() {
        val intent = Intent(this, RideLogService::class.java)
            .setAction(RideLogService.ACTION_START)
            .putExtra(RideLogService.EXTRA_MODE, mode.code)
        // Plain startService: this activity is in the foreground, so the service may
        // promote itself with startForeground. startForegroundService would crash the
        // app if the service has to stop before promoting (e.g. permission revoked).
        startService(intent)
    }

    private fun stopLog() {
        stopService(Intent(this, RideLogService::class.java))
    }

    // ─── Rendering ────────────────────────────────────────────────────────────

    private val refresh = object : Runnable {
        override fun run() {
            render()
            uiHandler.postDelayed(this, REFRESH_MS)
        }
    }

    private fun render() {
        val s = RideLogService.Stats
        val recording = s.recording
        val now = SystemClock.elapsedRealtimeNanos()

        startStop.setText(if (recording) R.string.ridelog_btn_stop else R.string.ridelog_btn_start)
        startStop.backgroundTintList = ContextCompat.getColorStateList(
            this, if (recording) R.color.button_stop else R.color.button_start
        )
        modeButton.text = getString(R.string.ridelog_btn_mode, modeName(mode))
        modeButton.isEnabled = !recording
        labelButtons.forEach { it.isEnabled = recording }

        val text = if (recording) {
            val dtS = (now - lastRefreshNs) / 1e9
            val accelHz = if (lastRefreshNs > 0 && dtS > 0) ((s.accelSamples - lastAccel) / dtS).toInt() else 0
            val gyroHz = if (lastRefreshNs > 0 && dtS > 0) ((s.gyroSamples - lastGyro) / dtS).toInt() else 0
            lastAccel = s.accelSamples; lastGyro = s.gyroSamples; lastRefreshNs = now

            val elapsedS = ((now - s.startNs) / 1_000_000_000L).coerceAtLeast(0)
            val acc = if (s.lastHAccM.isNaN()) getString(R.string.ridelog_unknown) else String.format(Locale.ROOT, "%.0f", s.lastHAccM)
            val baro = if (s.hasBaro) s.baroSamples.toString() else getString(R.string.ridelog_absent)
            val last = s.lastLabel?.let {
                getString(R.string.ridelog_last_label, it, ((now - s.lastLabelNs) / 1_000_000_000L).toInt())
            } ?: getString(R.string.ridelog_none)
            getString(
                R.string.ridelog_status_rec,
                String.format(Locale.ROOT, "%d:%02d", elapsedS / 60, elapsedS % 60),
                s.sessionName ?: "",
                s.gpsFixes, acc, accelHz, gyroHz, baro, s.engineRows, s.labels, last
            )
        } else {
            lastRefreshNs = 0L
            val path = s.sessionPath ?: File(getExternalFilesDir(null) ?: filesDir, RideLogService.LOG_ROOT).absolutePath
            getString(R.string.ridelog_status_idle, path)
        }
        status.text = s.error?.let { text + getString(R.string.ridelog_status_error, it) } ?: text
        status.setTextColor(ContextCompat.getColor(
            this, if (recording) R.color.ridelog_status_rec else R.color.ridelog_status_idle
        ))
    }

    private fun modeName(m: RideMode): String = getString(
        when (m) {
            RideMode.COACH_DRIVES -> R.string.ridelog_mode_coach_drives
            RideMode.COACH_LABELS -> R.string.ridelog_mode_coach_labels
            RideMode.COAST_DOWN -> R.string.ridelog_mode_coast_down
        }
    )

    // ─── Haptics ──────────────────────────────────────────────────────────────

    private val vibrator: Vibrator? by lazy {
        @Suppress("DEPRECATION")
        getSystemService(VIBRATOR_SERVICE) as? Vibrator
    }

    private fun vibrate(ms: Long) {
        vibrator?.takeIf { it.hasVibrator() }
            ?.vibrate(VibrationEffect.createOneShot(ms, VibrationEffect.DEFAULT_AMPLITUDE))
    }

    private companion object {
        const val PREF_MODE = "ride_mode"
        const val REFRESH_MS = 500L
        const val TAP_BUZZ_MS = 25L
        const val UNDO_BUZZ_MS = 80L
    }
}
