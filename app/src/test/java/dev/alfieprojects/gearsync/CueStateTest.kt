package dev.alfieprojects.gearsync

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * JVM unit tests for the ADR 006 cue state machine. CueState is pure Kotlin
 * (no Android deps), so it runs on the host. Includes the regression for the
 * Long.MIN_VALUE cooldown overflow that silenced every cue.
 */
class CueStateTest {

    // Big base time, like System.currentTimeMillis(), to exercise the overflow path.
    private val t0 = 1_750_000_000_000L

    @Test
    fun firstUpshiftFires_noCooldownOverflow() {
        val s = CueState(cooldownMs = 1500L)
        // Establish a known zone first (UNKNOWN→LUG yields no cue).
        assertEquals(CueIntent.NONE, s.update(0.10f, true, t0))
        // Climb through optimal (silent) into redline → first real cue.
        assertEquals(CueIntent.NONE, s.update(0.50f, true, t0 + 100))
        assertEquals(CueIntent.UPSHIFT, s.update(0.80f, true, t0 + 200))
    }

    @Test
    fun downshiftFiresOnDropToLug() {
        val s = CueState(cooldownMs = 1500L)
        s.update(0.80f, true, t0)                       // start in redline
        assertEquals(CueIntent.DOWNSHIFT, s.update(0.10f, true, t0 + 2000))
    }

    @Test
    fun cooldownSuppressesRapidSecondCue() {
        val s = CueState(cooldownMs = 1500L)
        s.update(0.10f, true, t0)
        assertEquals(CueIntent.UPSHIFT, s.update(0.80f, true, t0 + 100))
        // Drop to lug within the cooldown window → suppressed.
        assertEquals(CueIntent.NONE, s.update(0.10f, true, t0 + 500))
        // Same transition after the window → allowed.
        s.update(0.80f, true, t0 + 1700)               // back to redline (also cued)
        assertEquals(CueIntent.DOWNSHIFT, s.update(0.10f, true, t0 + 3400))
    }

    @Test
    fun optimalZoneIsSilent() {
        val s = CueState(cooldownMs = 1500L)
        s.update(0.10f, true, t0)                       // lug
        assertEquals(CueIntent.NONE, s.update(0.50f, true, t0 + 100))   // → optimal: quiet
    }

    @Test
    fun unknownGearNeverCues() {
        val s = CueState(cooldownMs = 1500L)
        assertEquals(CueIntent.NONE, s.update(0.80f, false, t0))        // gear unknown
        assertEquals(CueIntent.NONE, s.update(0.10f, false, t0 + 2000)) // still unknown
    }

    @Test
    fun noCueWhenZoneUnchanged() {
        val s = CueState(cooldownMs = 1500L)
        s.update(0.80f, true, t0)                       // redline (UNKNOWN→REDLINE, no cue)
        assertEquals(CueIntent.NONE, s.update(0.90f, true, t0 + 2000)) // still redline
    }

    @Test
    fun resetClearsState() {
        val s = CueState(cooldownMs = 1500L)
        s.update(0.10f, true, t0)
        s.update(0.80f, true, t0 + 100)                 // UPSHIFT, lastCueMs set
        s.reset()
        // After reset, re-establishing a zone then transitioning fires again with no overflow.
        assertEquals(CueIntent.NONE, s.update(0.10f, true, t0 + 200))
        assertEquals(CueIntent.UPSHIFT, s.update(0.80f, true, t0 + 300))
    }
}
