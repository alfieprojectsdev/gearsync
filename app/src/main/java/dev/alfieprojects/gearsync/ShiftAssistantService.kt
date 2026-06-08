package dev.alfieprojects.gearsync

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.os.Looper
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
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(NOTIFICATION_ID, buildNotification())
        startLocationUpdates()
        return START_STICKY
    }

    override fun onDestroy() {
        fusedLocationClient.removeLocationUpdates(locationCallback)
        persistCalibrationState()
        NativeEngine.stopEngine()
        super.onDestroy()
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
                speedJitterThresholdMps  = cfg.speedJitterThresholdMps
            )
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
        // Must stay in sync with native: 3 Welford fields + NUM_GEARS (5) gear ratios.
        private const val CALIBRATION_STATE_LEN  = 8
    }
}
