package dev.alfieprojects.gearsync

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import org.json.JSONObject
import java.io.BufferedWriter
import java.io.File
import java.io.FileWriter
import java.io.IOException

/**
 * Mountain-mode M0 ride-along logger (plans/mountain-mode-plan.md, docs/RIDE-ALONG.md).
 *
 * Records raw GNSS fixes, accelerometer + gyroscope at ~100 Hz, barometer if the
 * phone has one, passenger labels, and (only when ShiftAssistantService is also
 * running) the mic engine state, into one session directory of CSVs + meta.json.
 *
 * Independent of the native engine: no JNI changes, and the native sensor thread's
 * ~400 Hz accel stream for ADR 004 is untouched (this registers its own listeners).
 * All file I/O runs on one HandlerThread; sensor and location callbacks are
 * delivered on that thread's looper, so writers are thread-confined.
 *
 * Control is intent-based so a tap's timestamp is captured in the activity at the
 * moment of the tap, not when the service gets around to it.
 */
class RideLogService : Service() {

    private lateinit var sensorManager: SensorManager
    private lateinit var locationManager: LocationManager
    private var wakeLock: PowerManager.WakeLock? = null

    private var logThread: HandlerThread? = null
    private var logHandler: Handler? = null

    // Thread-confined to logThread.
    private var session: Session? = null

    // ─── Lifecycle ────────────────────────────────────────────────────────────

    override fun onCreate() {
        super.onCreate()
        sensorManager = getSystemService(SENSOR_SERVICE) as SensorManager
        locationManager = getSystemService(LOCATION_SERVICE) as LocationManager
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> {
                val mode = intent.getStringExtra(EXTRA_MODE)
                    ?.let { code -> RideMode.entries.firstOrNull { it.code == code } }
                    ?: RideMode.COACH_LABELS
                startRecording(mode)
            }
            ACTION_LABEL -> {
                val label = intent.getStringExtra(EXTRA_LABEL)?.let(RideLabel::fromCode)
                val tNs = intent.getLongExtra(EXTRA_T_NS, SystemClock.elapsedRealtimeNanos())
                val handler = logHandler
                if (handler == null) stopSelf()   // label without a session: don't linger
                else if (label != null) handler.post { session?.label(tNs, label) }
            }
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        stopRecording()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    // ─── Recording ───────────────────────────────────────────────────────────

    private fun startRecording(mode: RideMode) {
        if (logThread != null) return   // already recording; a second Start is a no-op

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION)
            != PackageManager.PERMISSION_GRANTED
        ) {
            Stats.error = getString(R.string.ridelog_err_no_location)
            stopSelf()
            return
        }
        try {
            startForeground(NOTIFICATION_ID, buildNotification(mode))
        } catch (e: RuntimeException) {
            // API 31+ ForegroundServiceStartNotAllowedException / API 34 type-permission mismatch.
            Log.e(TAG, "startForeground failed", e)
            Stats.error = e.message
            stopSelf()
            return
        }

        val thread = HandlerThread("RideLog").also { it.start() }
        val handler = Handler(thread.looper)
        logThread = thread
        logHandler = handler

        wakeLock = (getSystemService(POWER_SERVICE) as PowerManager)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "GearSync:RideLog")
            .apply { acquire(MAX_SESSION_MS) }

        val startEpochMs = System.currentTimeMillis()
        val startNs = SystemClock.elapsedRealtimeNanos()
        val baseDir = getExternalFilesDir(null) ?: filesDir
        val dir = File(File(baseDir, LOG_ROOT), RideLogFormat.sessionDirName(startEpochMs, mode))

        handler.post {
            try {
                session = Session(dir, mode, startEpochMs, startNs)
            } catch (e: IOException) {
                Log.e(TAG, "cannot open session in $dir", e)
                Stats.error = e.message
                Handler(mainLooper).post { stopSelf() }
                return@post
            }
            registerStreams(handler)
            handler.postDelayed(flushTick, FLUSH_INTERVAL_MS)
            handler.postDelayed(engineTick, ENGINE_INTERVAL_MS)
        }
    }

    private fun stopRecording() {
        val thread = logThread ?: return
        val handler = logHandler!!
        // Unregister on the log thread, queued after the start post: if Stop lands
        // before registerStreams has run, unregistering here on main would happen
        // first and the late registration would leak onto a dead looper.
        handler.post {
            sensorManager.unregisterListener(sensorListener)
            locationManager.removeUpdates(locationListener)
            handler.removeCallbacks(flushTick)
            handler.removeCallbacks(engineTick)
            session?.close(System.currentTimeMillis(), SystemClock.elapsedRealtimeNanos())
            session = null
            Stats.recording = false
        }
        thread.quitSafely()   // runs the close above before the looper exits
        logThread = null
        logHandler = null
        wakeLock?.takeIf { it.isHeld }?.release()
        wakeLock = null
        stopForeground(STOP_FOREGROUND_REMOVE)
    }

    @Suppress("MissingPermission")   // checked in startRecording
    private fun registerStreams(handler: Handler) {
        val accel = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
        val gyro  = sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
        val baro  = sensorManager.getDefaultSensor(Sensor.TYPE_PRESSURE)
        accel?.let { sensorManager.registerListener(sensorListener, it, IMU_PERIOD_US, handler) }
        gyro?.let  { sensorManager.registerListener(sensorListener, it, IMU_PERIOD_US, handler) }
        baro?.let  { sensorManager.registerListener(sensorListener, it, BARO_PERIOD_US, handler) }
        Stats.hasGyro = gyro != null
        Stats.hasBaro = baro != null

        // Raw GNSS fixes (Doppler speed), not the fused provider's smoothed speed (ADR 007).
        // Requested even when GPS is off, so fixes start as soon as it's switched on.
        locationManager.requestLocationUpdates(
            LocationManager.GPS_PROVIDER, GPS_INTERVAL_MS, 0f, locationListener, handler.looper
        )
        if (!locationManager.isProviderEnabled(LocationManager.GPS_PROVIDER)) {
            Stats.error = getString(R.string.ridelog_err_gps_off)
        }
    }

    private val sensorListener = object : SensorEventListener {
        override fun onSensorChanged(event: SensorEvent) {
            val s = session ?: return
            val rx = SystemClock.elapsedRealtimeNanos()
            when (event.sensor.type) {
                Sensor.TYPE_ACCELEROMETER ->
                    s.imu(event.timestamp, rx, RideLogFormat.IMU_ACCEL, event.values)
                Sensor.TYPE_GYROSCOPE ->
                    s.imu(event.timestamp, rx, RideLogFormat.IMU_GYRO, event.values)
                Sensor.TYPE_PRESSURE ->
                    s.baro(event.timestamp, rx, event.values[0])
            }
        }

        override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit
    }

    // Explicit object (not a SAM lambda): before API 30 the framework calls the
    // provider/status methods, which have no default implementation there.
    private val locationListener = object : LocationListener {
        override fun onLocationChanged(location: Location) { session?.gps(location) }
        override fun onProviderEnabled(provider: String) {
            if (Stats.error == getString(R.string.ridelog_err_gps_off)) Stats.error = null
        }
        override fun onProviderDisabled(provider: String) {
            Stats.error = getString(R.string.ridelog_err_gps_off)
        }
        @Deprecated("Deprecated in API 29")
        override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) = Unit
    }

    private val flushTick = object : Runnable {
        override fun run() {
            session?.flush()
            logHandler?.postDelayed(this, FLUSH_INTERVAL_MS)
        }
    }

    /** Engine state only when the mic path is genuinely running; never demo/sweep frames. */
    private val engineTick = object : Runnable {
        override fun run() {
            if (ShiftAssistantService.isRunning && !NativeEngine.demoMode && !BuildConfig.VU_SWEEP) {
                NativeEngine.peekVUMeterState()?.let { v ->
                    if (v.size >= 5) session?.engine(SystemClock.elapsedRealtimeNanos(), v)
                }
            }
            logHandler?.postDelayed(this, ENGINE_INTERVAL_MS)
        }
    }

    // ─── Session: files + counters, confined to the log thread ───────────────

    private inner class Session(
        private val dir: File,
        private val mode: RideMode,
        private val startEpochMs: Long,
        private val startNs: Long
    ) {
        private val writers = mutableListOf<BufferedWriter>()
        private val gpsOut: BufferedWriter
        private val imuOut: BufferedWriter
        private val baroOut: BufferedWriter
        private val labelOut: BufferedWriter
        private val engineOut: BufferedWriter
        private var failed = false

        init {
            try {
                gpsOut    = open(RideLogFormat.GPS_FILE, RideLogFormat.GPS_HEADER)
                imuOut    = open(RideLogFormat.IMU_FILE, RideLogFormat.IMU_HEADER)
                baroOut   = open(RideLogFormat.BARO_FILE, RideLogFormat.BARO_HEADER)
                labelOut  = open(RideLogFormat.LABEL_FILE, RideLogFormat.LABEL_HEADER)
                engineOut = open(RideLogFormat.ENGINE_FILE, RideLogFormat.ENGINE_HEADER)
            } catch (e: IOException) {
                // Don't leak the files that did open before the failure.
                writers.forEach { w -> try { w.close() } catch (_: IOException) { } }
                throw e
            }
            Stats.reset(dir.name, dir.absolutePath, startNs)
            writeMeta(endEpochMs = null, endNs = null)
        }

        private fun open(name: String, header: List<String>): BufferedWriter {
            if (!dir.exists() && !dir.mkdirs()) throw IOException("mkdirs failed: $dir")
            val w = BufferedWriter(FileWriter(File(dir, name)), WRITE_BUFFER_BYTES)
            writers += w
            w.write(RideLogFormat.header(header)); w.newLine()
            return w
        }

        private inline fun append(out: BufferedWriter, row: () -> String) {
            if (failed) return
            try {
                out.write(row()); out.newLine()
            } catch (e: IOException) {
                failed = true
                Stats.error = e.message
                Log.e(TAG, "write failed; logging stopped", e)
            }
        }

        fun gps(l: Location) {
            append(gpsOut) {
                val msl = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE && l.hasMslAltitude())
                    l.mslAltitudeMeters else null
                RideLogFormat.gpsRow(
                    tNs = l.elapsedRealtimeNanos, utcMs = l.time, provider = l.provider ?: "",
                    lat = l.latitude, lon = l.longitude,
                    altEllipsoidM = if (l.hasAltitude()) l.altitude else null,
                    altMslM = msl,
                    speedMps = if (l.hasSpeed()) l.speed else null,
                    bearingDeg = if (l.hasBearing()) l.bearing else null,
                    hAccM = if (l.hasAccuracy()) l.accuracy else null,
                    vAccM = if (l.hasVerticalAccuracy()) l.verticalAccuracyMeters else null,
                    speedAccMps = if (l.hasSpeedAccuracy()) l.speedAccuracyMetersPerSecond else null,
                    bearingAccDeg = if (l.hasBearingAccuracy()) l.bearingAccuracyDegrees else null
                )
            }
            Stats.gpsFixes++
            Stats.lastHAccM = if (l.hasAccuracy()) l.accuracy else Float.NaN
        }

        fun imu(tNs: Long, rxNs: Long, sensor: String, v: FloatArray) {
            append(imuOut) { RideLogFormat.imuRow(tNs, rxNs, sensor, v[0], v[1], v[2]) }
            if (sensor == RideLogFormat.IMU_ACCEL) Stats.accelSamples++ else Stats.gyroSamples++
        }

        fun baro(tNs: Long, rxNs: Long, hpa: Float) {
            append(baroOut) { RideLogFormat.baroRow(tNs, rxNs, hpa) }
            Stats.baroSamples++
        }

        fun label(tNs: Long, label: RideLabel) {
            append(labelOut) { RideLogFormat.labelRow(tNs, label) }
            labelOut.flush()   // labels are rare and precious: don't sit in the buffer
            Stats.labels++
            Stats.lastLabel = label.code
            Stats.lastLabelNs = tNs
        }

        /** v = [needlePos, dominantHz, speedMps, gear(1-based, 0 unknown), confidence, ...]. */
        fun engine(tNs: Long, v: FloatArray) {
            append(engineOut) {
                RideLogFormat.engineRow(tNs, v[1], v[2], v[3].toInt(), v[4], v[0])
            }
            Stats.engineRows++
        }

        fun flush() {
            if (failed) return
            try { writers.forEach { it.flush() } } catch (e: IOException) {
                failed = true; Stats.error = e.message
            }
        }

        fun close(endEpochMs: Long, endNs: Long) {
            writers.forEach { w -> try { w.close() } catch (_: IOException) { } }
            writeMeta(endEpochMs, endNs)
        }

        private fun writeMeta(endEpochMs: Long?, endNs: Long?) {
            val meta = JSONObject().apply {
                put("format_version", FORMAT_VERSION)
                put("session", dir.name)
                put("mode", mode.code)
                put("start_utc_ms", startEpochMs)
                put("start_elapsed_ns", startNs)
                put("end_utc_ms", endEpochMs ?: JSONObject.NULL)
                put("end_elapsed_ns", endNs ?: JSONObject.NULL)
                put("timebase", "t_ns = SystemClock.elapsedRealtimeNanos (SensorEvent.timestamp, Location.elapsedRealtimeNanos); rx_ns = callback receipt time")
                put("app_version", BuildConfig.VERSION_NAME)
                put("build_type", BuildConfig.BUILD_TYPE)
                put("device", JSONObject().apply {
                    put("manufacturer", Build.MANUFACTURER)
                    put("model", Build.MODEL)
                    put("sdk_int", Build.VERSION.SDK_INT)
                })
                put("sensors", JSONObject().apply {
                    put("accelerometer", sensorInfo(Sensor.TYPE_ACCELEROMETER, IMU_PERIOD_US))
                    put("gyroscope", sensorInfo(Sensor.TYPE_GYROSCOPE, IMU_PERIOD_US))
                    put("pressure", sensorInfo(Sensor.TYPE_PRESSURE, BARO_PERIOD_US))
                })
                put("location_provider", LocationManager.GPS_PROVIDER)
                put("gps_interval_ms", GPS_INTERVAL_MS)
                put("engine_logged_when", "ShiftAssistantService running, demo off")
                put("vehicle_config", vehicleConfigJson())
                put("rows", JSONObject().apply {
                    put("gps", Stats.gpsFixes)
                    put("accel", Stats.accelSamples)
                    put("gyro", Stats.gyroSamples)
                    put("baro", Stats.baroSamples)
                    put("labels", Stats.labels)
                    put("engine", Stats.engineRows)
                })
                put("write_error", Stats.error ?: JSONObject.NULL)
            }
            try {
                File(dir, RideLogFormat.META_FILE).writeText(meta.toString(2))
            } catch (e: IOException) {
                Log.e(TAG, "meta.json write failed", e)
            }
        }
    }

    private fun sensorInfo(type: Int, requestedPeriodUs: Int): JSONObject {
        val s = sensorManager.getDefaultSensor(type) ?: return JSONObject().put("present", false)
        return JSONObject().apply {
            put("present", true)
            put("name", s.name)
            put("vendor", s.vendor)
            put("min_delay_us", s.minDelay)
            put("max_range", s.maximumRange.toDouble())
            put("resolution", s.resolution.toDouble())
            put("requested_period_us", requestedPeriodUs)
        }
    }

    /** Embeds the shipped vehicle profile so a log is self-describing. */
    private fun vehicleConfigJson(): Any = try {
        JSONObject(assets.open("vehicle_config.json").bufferedReader().use { it.readText() })
    } catch (e: Exception) {
        JSONObject.NULL
    }

    // ─── Notification ────────────────────────────────────────────────────────

    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            CHANNEL_ID, getString(R.string.ridelog_channel_name), NotificationManager.IMPORTANCE_LOW
        )
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    private fun buildNotification(mode: RideMode): Notification {
        val tap = PendingIntent.getActivity(
            this, 0, Intent(this, RideLogActivity::class.java), PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.ridelog_notification_title))
            .setContentText(getString(R.string.ridelog_notification_text, mode.code))
            .setSmallIcon(R.drawable.ic_notification)
            .setContentIntent(tap)
            .setOngoing(true)
            .build()
    }

    /**
     * Live counters for the passenger panel. Written only by the log thread,
     * read by the UI; @Volatile is enough for a monotonically updated display.
     */
    object Stats {
        @Volatile var recording = false
        @Volatile var sessionName: String? = null
        @Volatile var sessionPath: String? = null
        @Volatile var startNs = 0L
        @Volatile var gpsFixes = 0L
        @Volatile var lastHAccM = Float.NaN
        @Volatile var accelSamples = 0L
        @Volatile var gyroSamples = 0L
        @Volatile var baroSamples = 0L
        @Volatile var labels = 0L
        @Volatile var lastLabel: String? = null
        @Volatile var lastLabelNs = 0L
        @Volatile var engineRows = 0L
        @Volatile var hasGyro = false
        @Volatile var hasBaro = false
        @Volatile var error: String? = null

        internal fun reset(name: String, path: String, start: Long) {
            sessionName = name; sessionPath = path; startNs = start
            gpsFixes = 0; lastHAccM = Float.NaN; accelSamples = 0; gyroSamples = 0
            baroSamples = 0; labels = 0; lastLabel = null; lastLabelNs = 0; engineRows = 0
            error = null
            recording = true
        }
    }

    companion object {
        const val ACTION_START = "dev.alfieprojects.gearsync.ridelog.START"
        const val ACTION_LABEL = "dev.alfieprojects.gearsync.ridelog.LABEL"
        const val EXTRA_MODE  = "mode"
        const val EXTRA_LABEL = "label"
        const val EXTRA_T_NS  = "t_ns"

        /** Under getExternalFilesDir(null): adb pull /sdcard/Android/data/<pkg>/files/ridelogs */
        const val LOG_ROOT = "ridelogs"

        private const val TAG = "RideLog"
        private const val FORMAT_VERSION = 1
        private const val NOTIFICATION_ID = 2          // ShiftAssistantService uses 1
        private const val CHANNEL_ID = "gearsync_ridelog"
        private const val IMU_PERIOD_US = 10_000       // 100 Hz: grade + curvature, not vibration DSP
        private const val BARO_PERIOD_US = 100_000     // 10 Hz request; most baros deliver less
        private const val GPS_INTERVAL_MS = 1_000L     // A07/A56 cap at 1 Hz anyway (ADR 007)
        private const val ENGINE_INTERVAL_MS = 100L
        private const val FLUSH_INTERVAL_MS = 2_000L
        private const val WRITE_BUFFER_BYTES = 1 shl 16
        private const val MAX_SESSION_MS = 6L * 60 * 60 * 1000   // wake-lock safety timeout

        fun labelIntent(context: Context, label: RideLabel, tNs: Long): Intent =
            Intent(context, RideLogService::class.java)
                .setAction(ACTION_LABEL)
                .putExtra(EXTRA_LABEL, label.code)
                .putExtra(EXTRA_T_NS, tNs)
    }
}
