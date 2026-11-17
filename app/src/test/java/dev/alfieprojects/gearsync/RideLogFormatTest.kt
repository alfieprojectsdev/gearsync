package dev.alfieprojects.gearsync

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.util.Locale

/** JVM tests for the M0 ride-along CSV contract (RideLogFormat is pure Kotlin). */
class RideLogFormatTest {

    private lateinit var savedLocale: Locale

    @Before fun saveLocale() { savedLocale = Locale.getDefault() }
    @After fun restoreLocale() { Locale.setDefault(savedLocale) }

    private fun fields(row: String) = row.split(",")

    @Test
    fun rowsHaveSameColumnCountAsHeaders() {
        val gps = RideLogFormat.gpsRow(
            1L, 2L, "gps", 16.41234567, 120.59876543,
            1520.5, 1475.2, 8.3f, 271f, 3.9f, 6.0f, 0.4f, 5f
        )
        assertEquals(RideLogFormat.GPS_HEADER.size, fields(gps).size)
        assertEquals(RideLogFormat.IMU_HEADER.size,
            fields(RideLogFormat.imuRow(1L, 2L, RideLogFormat.IMU_ACCEL, 0.1f, 9.7f, 1.2f)).size)
        assertEquals(RideLogFormat.BARO_HEADER.size, fields(RideLogFormat.baroRow(1L, 2L, 850.25f)).size)
        assertEquals(RideLogFormat.LABEL_HEADER.size, fields(RideLogFormat.labelRow(1L, RideLabel.FLOOR)).size)
        assertEquals(RideLogFormat.ENGINE_HEADER.size,
            fields(RideLogFormat.engineRow(1L, 62.5f, 8.3f, 2, 0.8f, 0.4f)).size)
    }

    @Test
    fun commaDecimalLocaleDoesNotCorruptRows() {
        Locale.setDefault(Locale.GERMANY)   // formats 1.5 as "1,5" if Locale.ROOT is not used
        val row = RideLogFormat.imuRow(5L, 6L, RideLogFormat.IMU_GYRO, 1.5f, -0.25f, 0f)
        assertEquals(RideLogFormat.IMU_HEADER.size, fields(row).size)
        assertTrue(row.contains("1.50000"))
        val gps = RideLogFormat.gpsRow(1L, 2L, "gps", 16.5, 120.5, null, null, null, null, null, null, null, null)
        assertEquals(RideLogFormat.GPS_HEADER.size, fields(gps).size)
    }

    @Test
    fun missingOptionalValuesAreEmptyNotZero() {
        val row = RideLogFormat.gpsRow(1L, 2L, "gps", 16.5, 120.5, null, null, 4f, null, 5f, null, null, null)
        val f = fields(row)
        assertEquals("", f[RideLogFormat.GPS_HEADER.indexOf("alt_ellipsoid_m")])
        assertEquals("", f[RideLogFormat.GPS_HEADER.indexOf("alt_msl_m")])
        assertEquals("", f[RideLogFormat.GPS_HEADER.indexOf("bearing_deg")])
        assertEquals(4.0, f[RideLogFormat.GPS_HEADER.indexOf("speed_mps")].toDouble(), 1e-9)
    }

    @Test
    fun nonFiniteNumbersAreEmpty() {
        assertEquals("", RideLogFormat.num(Float.NaN))
        assertEquals("", RideLogFormat.num(Double.POSITIVE_INFINITY))
        assertEquals("", RideLogFormat.num(null as Double?))
    }

    @Test
    fun coordinatesKeepSubMetreResolution() {
        // 1e-7 deg ≈ 1 cm; %.7g alone would round 120.59876543 to 120.5988 (~10 m).
        val row = RideLogFormat.gpsRow(1L, 2L, "gps", 16.41234567, 120.59876543,
            null, null, null, null, null, null, null, null)
        val f = fields(row)
        assertEquals(120.59876543, f[RideLogFormat.GPS_HEADER.indexOf("lon")].toDouble(), 1e-8)
        assertEquals(16.41234567, f[RideLogFormat.GPS_HEADER.indexOf("lat")].toDouble(), 1e-8)
    }

    @Test
    fun providerWithCommaCannotShiftColumns() {
        val row = RideLogFormat.gpsRow(1L, 2L, "gps,fused", 16.5, 120.5,
            null, null, null, null, null, null, null, null)
        assertEquals(RideLogFormat.GPS_HEADER.size, fields(row).size)
    }

    @Test
    fun sessionDirNameIsUtcAndCarriesMode() {
        // 2026-11-17T08:30:00Z
        val name = RideLogFormat.sessionDirName(1_794_904_200_000L, RideMode.COACH_LABELS)
        assertEquals("ride_20261117T083000Z_coach-labels", name)
        assertFalse(name.contains(" "))
    }

    @Test
    fun labelCodesAreUniqueAndRoundTrip() {
        val codes = RideLabel.entries.map { it.code }
        assertEquals(codes.size, codes.toSet().size)
        RideLabel.entries.forEach { assertEquals(it, RideLabel.fromCode(it.code)) }
        assertNull(RideLabel.fromCode("NOPE"))
    }
}
