package dev.alfieprojects.gearsync

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import androidx.core.app.NotificationCompat
import com.google.android.gms.location.FusedLocationProviderClient
import com.google.android.gms.location.LocationCallback
import com.google.android.gms.location.LocationRequest
import com.google.android.gms.location.LocationResult
import com.google.android.gms.location.LocationServices
import com.google.android.gms.location.Priority

class ShiftAssistantService : Service() {

    private lateinit var fusedLocationClient: FusedLocationProviderClient

    private val locationCallback = object : LocationCallback() {
        override fun onLocationResult(result: LocationResult) {
            val speed = result.lastLocation?.speed ?: 0f
            NativeEngine.updateGpsSpeed(speed)
        }
    }

    override fun onCreate() {
        super.onCreate()
        fusedLocationClient = LocationServices.getFusedLocationProviderClient(this)
        createNotificationChannel()
        NativeEngine.startEngine()
        applyVehicleConfig()
        restoreCalibrationState()
        isRunning = true
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(NOTIFICATION_ID, buildNotification())
        startLocationUpdates()
        if (BuildConfig.DEBUG) diagHandler.post(diagLogger)
        return START_STICKY
    }

    override fun onDestroy() {
        fusedLocationClient.removeLocationUpdates(locationCallback)
        diagHandler.removeCallbacks(diagLogger)
        persistCalibrationState()
        NativeEngine.stopEngine()
        isRunning = false
        super.onDestroy()
    }

    // ─── M6 drive diagnostics (debug builds only) ─────────────────────────────
    // Periodic logcat of the ADR 004 fusion state so an on-road test drive can be
    // validated without a UI overlay: filter with `adb logcat -s ShiftAssistant`.
    // Off the realtime/audio path; does not touch the VU meter.

    private val diagHandler = Handler(Looper.getMainLooper())
    private val diagLogger = object : Runnable {
        override fun run() {
            logDriveDiagnostics()
            diagHandler.postDelayed(this, DIAG_INTERVAL_MS)
        }
    }

    private fun logDriveDiagnostics() {
        val f = NativeEngine.nativeVibrationFusionStats() ?: return
        if (f.size < 8) return
        val vu = NativeEngine.getVUMeterState()
        val needle = vu?.getOrNull(0) ?: 0f
        val micHz  = vu?.getOrNull(1) ?: 0f
        val speed  = vu?.getOrNull(2) ?: 0f
        val gear   = vu?.getOrNull(3) ?: 0f
        // f: [reqHz, measHz, useFusion, fusionActive, reason, vibHz, vibProm, srcMode, ...]
        Log.i(
            TAG,
            "drive accelHz=%.0f fusion=%.0f active=%.0f reason=%.0f vibHz=%.1f prom=%.2f src=%.0f | micHz=%.1f speed=%.1f gear=%.0f needle=%.2f".format(
                f[1], f[2], f[3], f[4], f[5], f[6], f[7], micHz, speed, gear, needle
            )
        )
    }

    override fun onBind(intent: Intent?): IBinder? = null

    // ─── Location ────────────────────────────────────────────────────────────

    @Suppress("MissingPermission")
    private fun startLocationUpdates() {
        val request = LocationRequest.Builder(
            Priority.PRIORITY_HIGH_ACCURACY,
            1_000L   // 1 Hz
        ).build()

        fusedLocationClient.requestLocationUpdates(
            request,
            locationCallback,
            Looper.getMainLooper()
        )
    }

    // ─── Vehicle config ───────────────────────────────────────────────────────

    private fun applyVehicleConfig() {
        try {
            val cfg = VehicleConfig.load(this)
            NativeEngine.setVehicleConfig(
                kSeeds                   = cfg.kSeeds,
                toleranceLow             = cfg.toleranceLow,
                toleranceHigh            = cfg.toleranceHigh,
                stabilityWindowSamples   = cfg.steadyStateWindowSeconds, // GPS = 1 Hz, so seconds == samples
                speedJitterThresholdMps  = cfg.speedJitterThresholdMps,
                useVibrationFusion       = cfg.useVibrationFusion
            )
            // ADR 006 audio cues are Kotlin-only (no JNI); expose the opt-in flag
            // for VUMeterView to read. Default off → visual-only.
            NativeEngine.audioCuesEnabled = cfg.useAudioCues
        } catch (e: Exception) {
            // Config load failure is non-fatal; engine continues unconfigured (open tolerances).
            android.util.Log.e("ShiftAssistant", "vehicle_config.json load failed: ${e.message}")
        }
    }

    // ─── Calibration state persistence ───────────────────────────────────────

    private fun persistCalibrationState() {
        val state = NativeEngine.saveCalibrationState() ?: return
        val prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        val editor = prefs.edit()
        state.forEachIndexed { i, v -> editor.putFloat("cal_$i", v) }
        editor.apply()
    }

    private fun restoreCalibrationState() {
        val prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        val array = FloatArray(CALIBRATION_STATE_LEN) { i -> prefs.getFloat("cal_$i", 0f) }
        if (array[0] > 0f) {   // n > 0 means we have prior data
            NativeEngine.resumeCalibrationState(array)
        }
    }

    // ─── Notification ────────────────────────────────────────────────────────

    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            CHANNEL_ID,
            "GearSync — Active",
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "Running shift-assistant in the background"
        }
        getSystemService(NotificationManager::class.java)
            .createNotificationChannel(channel)
    }

    private fun buildNotification(): Notification {
        val tapIntent = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("GearSync")
            .setContentText("Shift assistant active")
            .setSmallIcon(R.drawable.ic_notification)
            .setContentIntent(tapIntent)
            .setOngoing(true)
            .build()
    }

    companion object {
        private const val NOTIFICATION_ID        = 1
        private const val CHANNEL_ID             = "gearsync_fg"
        private const val PREFS_NAME             = "gearsync_calibration"
        private const val TAG                    = "ShiftAssistant"  // matches native LOG_TAG
        private const val DIAG_INTERVAL_MS       = 2_000L            // M6 drive diagnostics cadence
        // 3 Welford fields + 5 gear ratios + 5 pin flags = 13 floats.
        // CalibrationEngine.deserialise accepts the old 8-float format by defaulting
        // missing pin flags to 0.0 (unpinned). (ref: DL-002, R-002)
        private const val CALIBRATION_STATE_LEN  = 13

        /** True while the service is in the created-and-not-yet-destroyed state.
         *  Read by MainActivity to gate the "Calibrate" button. */
        @Volatile var isRunning: Boolean = false
    }
}
