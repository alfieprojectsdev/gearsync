package dev.alfieprojects.gearsync

/**
 * ADR 006 M1 — pure zone-transition → cue-intent state machine (no Android deps,
 * unit-testable). Maps the VU needle position to a shift zone and emits a cue
 * intent only on a meaningful zone *transition*, debounced by a cooldown so the
 * cue fires once per shift rather than chattering at a zone boundary.
 *
 * Zone thresholds mirror VUMeterView (LUG_END 0.33, OPT_END 0.66). Cue language
 * (DL-AUD-1): entering REDLINE → UPSHIFT, entering LUG → DOWNSHIFT, OPTIMAL → silence.
 */
enum class CueIntent { NONE, UPSHIFT, DOWNSHIFT }

class CueState(private val cooldownMs: Long = DEFAULT_COOLDOWN_MS) {

    private enum class Zone { UNKNOWN, LUG, OPTIMAL, REDLINE }

    private var lastZone = Zone.UNKNOWN
    private var lastCueMs = Long.MIN_VALUE

    private fun zoneOf(needle: Float, gearKnown: Boolean): Zone = when {
        !gearKnown      -> Zone.UNKNOWN
        needle < LUG_END -> Zone.LUG
        needle < OPT_END -> Zone.OPTIMAL
        else            -> Zone.REDLINE
    }

    /**
     * Advance the machine with the latest needle/gear and current time.
     * Returns the cue to play this frame, or NONE. Pure given its inputs.
     */
    fun update(needle: Float, gearKnown: Boolean, nowMs: Long): CueIntent {
        val zone = zoneOf(needle, gearKnown)
        val prev = lastZone
        lastZone = zone

        // No cue across unknown boundaries (gear lost / regained) or no change.
        if (zone == Zone.UNKNOWN || prev == Zone.UNKNOWN || zone == prev) return CueIntent.NONE

        val intent = when (zone) {
            Zone.REDLINE -> CueIntent.UPSHIFT     // climbed into redline → shift up
            Zone.LUG     -> CueIntent.DOWNSHIFT   // dropped into lug → shift down
            else         -> CueIntent.NONE        // settled into optimal → stay quiet
        }
        if (intent == CueIntent.NONE) return CueIntent.NONE

        // Debounce: one cue per cooldown window. Guard the never-fired sentinel
        // explicitly — `nowMs - Long.MIN_VALUE` overflows and would wrap negative,
        // permanently suppressing the first (and thus every) cue.
        if (lastCueMs != Long.MIN_VALUE && nowMs - lastCueMs < cooldownMs) return CueIntent.NONE
        lastCueMs = nowMs
        return intent
    }

    fun reset() {
        lastZone = Zone.UNKNOWN
        lastCueMs = Long.MIN_VALUE
    }

    companion object {
        const val DEFAULT_COOLDOWN_MS = 1500L
        private const val LUG_END = 0.33f
        private const val OPT_END = 0.66f
    }
}
