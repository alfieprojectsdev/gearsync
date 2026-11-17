package dev.alfieprojects.gearsync

import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.util.Locale

/**
 * Mountain-mode M0 ride-along log format (plans/mountain-mode-plan.md).
 *
 * Pure Kotlin, no Android types, so the CSV contract is JVM-testable. Every
 * stream shares one timebase: `t_ns` is SystemClock.elapsedRealtimeNanos()
 * (SensorEvent.timestamp and Location.getElapsedRealtimeNanos() use it), so
 * streams align without wall-clock jumps. Numbers are formatted with
 * Locale.ROOT: a phone set to a comma-decimal locale must not corrupt the CSV.
 * Absent optional values (e.g. a fix with no altitude) are written as empty
 * fields, never as 0.
 */
object RideLogFormat {

    const val GPS_FILE    = "gps.csv"
    const val IMU_FILE    = "imu.csv"
    const val BARO_FILE   = "baro.csv"
    const val LABEL_FILE  = "labels.csv"
    const val ENGINE_FILE = "engine.csv"
    const val META_FILE   = "meta.json"

    /** `sensor` column of imu.csv. */
    const val IMU_ACCEL = "A"
    const val IMU_GYRO  = "G"

    val GPS_HEADER = listOf(
        "t_ns", "utc_ms", "provider", "lat", "lon",
        "alt_ellipsoid_m", "alt_msl_m", "speed_mps", "bearing_deg",
        "h_acc_m", "v_acc_m", "speed_acc_mps", "bearing_acc_deg"
    )

    /** One row per sample; `x,y,z` are m/s² for A (gravity included) and rad/s for G. */
    val IMU_HEADER = listOf("t_ns", "rx_ns", "sensor", "x", "y", "z")

    val BARO_HEADER = listOf("t_ns", "rx_ns", "hpa")

    val LABEL_HEADER = listOf("t_ns", "label")

    /** Only written while the shift-assistant engine is running (mic path on). */
    val ENGINE_HEADER = listOf("t_ns", "dominant_hz", "speed_mps", "gear", "confidence", "needle")

    fun header(columns: List<String>): String = columns.joinToString(",")

    /** Plain numbers only; NaN/Inf become empty so the CSV stays parseable. */
    fun num(v: Double?): String =
        if (v == null || v.isNaN() || v.isInfinite()) "" else String.format(Locale.ROOT, "%.7g", v)

    fun num(v: Float?): String = num(v?.toDouble())

    /** Lat/lon need fixed decimals: %.7g would drop to ~1 m resolution at 3-digit longitudes. */
    fun coord(v: Double): String = String.format(Locale.ROOT, "%.8f", v)

    fun gpsRow(
        tNs: Long, utcMs: Long, provider: String, lat: Double, lon: Double,
        altEllipsoidM: Double?, altMslM: Double?, speedMps: Float?, bearingDeg: Float?,
        hAccM: Float?, vAccM: Float?, speedAccMps: Float?, bearingAccDeg: Float?
    ): String = listOf(
        tNs.toString(), utcMs.toString(), csvSafe(provider), coord(lat), coord(lon),
        num(altEllipsoidM), num(altMslM), num(speedMps), num(bearingDeg),
        num(hAccM), num(vAccM), num(speedAccMps), num(bearingAccDeg)
    ).joinToString(",")

    fun imuRow(tNs: Long, rxNs: Long, sensor: String, x: Float, y: Float, z: Float): String =
        "$tNs,$rxNs,$sensor,${num(x)},${num(y)},${num(z)}"

    fun baroRow(tNs: Long, rxNs: Long, hpa: Float): String = "$tNs,$rxNs,${num(hpa)}"

    fun labelRow(tNs: Long, label: RideLabel): String = "$tNs,${label.code}"

    fun engineRow(tNs: Long, dominantHz: Float, speedMps: Float, gear: Int, confidence: Float, needle: Float): String =
        "$tNs,${num(dominantHz)},${num(speedMps)},$gear,${num(confidence)},${num(needle)}"

    private val SESSION_STAMP: DateTimeFormatter =
        DateTimeFormatter.ofPattern("yyyyMMdd'T'HHmmss'Z'", Locale.ROOT).withZone(ZoneOffset.UTC)

    /** e.g. `ride_20261117T083000Z_coach-labels`. UTC so names sort and never collide across DST. */
    fun sessionDirName(startEpochMs: Long, mode: RideMode): String =
        "ride_${SESSION_STAMP.format(Instant.ofEpochMilli(startEpochMs))}_${mode.code}"

    private fun csvSafe(s: String): String = s.replace(',', ';').replace('\n', ' ')
}

/** Who is driving and what the passenger's taps mean. Stored in meta.json. */
enum class RideMode(val code: String) {
    /** The experienced driver drives; the passenger tags what she does (gear she's in, phase). */
    COACH_DRIVES("coach-drives"),
    /** The learner drives; the experienced passenger taps what she would have said. */
    COACH_LABELS("coach-labels"),
    /** Coast-down tests: MARK at the start and end of each run (docs/RIDE-ALONG.md). */
    COAST_DOWN("coast-down")
}

/**
 * Passenger tap vocabulary. Phase labels mirror the plan's cue names; GEAR_n is an
 * absolute gear so a missed tap can't drift the count. UNDO cancels the previous
 * label at analysis time (rows are append-only).
 */
enum class RideLabel(val code: String) {
    BUILD("BUILD"),
    HOLD("HOLD"),
    EASE("EASE"),
    FLOOR("FLOOR"),
    BRAKE("BRAKE"),
    GEAR_1("GEAR_1"),
    GEAR_2("GEAR_2"),
    GEAR_3("GEAR_3"),
    GEAR_4("GEAR_4"),
    GEAR_5("GEAR_5"),
    MARK("MARK"),
    UNDO("UNDO");

    companion object {
        fun fromCode(code: String): RideLabel? = entries.firstOrNull { it.code == code }
    }
}
