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
     * Returns [needlePos, dominantHz, speedMps, gear (1-based, 0=unknown),
     *          confidence, shiftDetected (1.0 = event pending, then cleared)]
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
     * Push vehicle-specific calibration parameters to the native engine.
     * Must be called after [startEngine] and before driving begins.
     *
     * @param kSeeds                  Theoretical k_g values (Hz/m·s⁻¹), one per gear, descending.
     * @param toleranceLow            Fraction below theoretical k_g that is still accepted (e.g. 0.98).
     * @param toleranceHigh           Fraction above theoretical k_g that is still accepted (e.g. 1.025).
     * @param stabilityWindowSamples  Consecutive stable GPS samples before feeding Welford.
     * @param speedJitterThresholdMps Speed delta (m/s) considered "stable" between GPS updates.
     */
    @JvmStatic external fun setVehicleConfig(
        kSeeds: FloatArray,
        toleranceLow: Float,
        toleranceHigh: Float,
        stabilityWindowSamples: Int,
        speedJitterThresholdMps: Float
    )
}
