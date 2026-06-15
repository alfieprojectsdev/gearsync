package dev.alfieprojects.gearsync

/**
 * Synthetic VU-meter state for the `sweep` build type only.
 *
 * Drives the meter through every zone (lug → optimal → redline) and every gear
 * (G1…G5) on a fixed timeline so a built APK self-demos the segmented bar, zone
 * colors, gear glyph, peripheral wash, and shift flash with no microphone, GPS,
 * or running service. Activated solely by [BuildConfig.VU_SWEEP], which is true
 * only in the `sweep` build type (false in debug/release) — see app/build.gradle.
 *
 * No engine, sensor, or VUMeterView coupling: NativeEngine.getVUMeterState()
 * substitutes [nextFrame] for the native payload when the flag is set.
 */
object DebugSweep {

    /** Seconds for one full triangle (lug→redline→lug); gear advances per cycle. */
    private const val SWEEP_SECONDS = 6.0

    /** Needle fraction beyond which a shift event flashes (top of the redline). */
    private const val SHIFT_FLASH_FROM = 0.97

    private const val NUM_GEARS = 5

    private var startNanos = 0L

    /**
     * @return [needlePos, dominantHz, speedMps, gear (1-based), confidence,
     *          shiftDetected] — the same 6-float contract VUMeterView reads.
     */
    fun nextFrame(): FloatArray {
        val now = System.nanoTime()
        if (startNanos == 0L) startNanos = now
        val t = (now - startNanos) / 1_000_000_000.0   // seconds since first frame

        val cyclePos = (t % SWEEP_SECONDS) / SWEEP_SECONDS          // 0 → 1 within a gear
        val gear = ((t / SWEEP_SECONDS).toInt() % NUM_GEARS) + 1    // 1 → 5, repeating

        // Triangle: lug→redline over the first half, redline→lug over the second.
        // A monotonic ramp+reset only ever crossed *up* (the reset down landed
        // inside the cue cooldown), so the demo never played the downshift cue.
        val needle = (if (cyclePos < 0.5) cyclePos * 2.0 else 2.0 - cyclePos * 2.0).toFloat()
        val shift = if (needle > SHIFT_FLASH_FROM) 1f else 0f
        val hz = 30f + needle * 90f                                // cosmetic 30 → 120 Hz
        val speed = 5f + gear * 4f                                 // cosmetic m/s
        val confidence = 0.9f

        //              [0]     [1] [2]    [3]            [4]         [5]
        return floatArrayOf(needle, hz, speed, gear.toFloat(), confidence, shift)
    }
}
