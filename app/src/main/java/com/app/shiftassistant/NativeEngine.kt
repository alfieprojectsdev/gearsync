package com.app.shiftassistant

object NativeEngine {

    init {
        System.loadLibrary("native-lib")
    }

    @JvmStatic external fun startEngine()
    @JvmStatic external fun stopEngine()

    /** Called from ShiftAssistantService at 1 Hz with GPS speed in m/s. */
    @JvmStatic external fun updateGpsSpeed(speed: Float)

    /**
     * Returns [needlePos, dominantHz, speedMps, gear (1-based, 0=unknown), confidence]
     * Called from VUMeterView at 60 FPS.
     */
    @JvmStatic external fun getVUMeterState(): FloatArray?

    /**
     * Restore Welford + gear-ratio state from a previous session.
     * Layout: [n_float, mean, m2, ratio0…ratio4]
     */
    @JvmStatic external fun resumeCalibrationState(stateArray: FloatArray)

    /**
     * Serialise the current calibration state for SharedPreferences persistence.
     * Layout: [n_float, mean, m2, ratio0…ratio4]
     */
    @JvmStatic external fun saveCalibrationState(): FloatArray?

    /**
     * Update the synthesized blip frequency at runtime (thread-safe).
     * Valid range: 20–22050 Hz. Values outside this range are clamped.
     * Intended to be wired to a settings UI slider.
     */
    @JvmStatic fun setAudioCueFrequency(hz: Float) {
        setAudioCueFrequencyNative(hz.coerceIn(MIN_CUE_FREQ_HZ, MAX_CUE_FREQ_HZ))
    }

    @JvmStatic private external fun setAudioCueFrequencyNative(hz: Float)

    private const val MIN_CUE_FREQ_HZ = 20f
    private const val MAX_CUE_FREQ_HZ = 22050f
}
