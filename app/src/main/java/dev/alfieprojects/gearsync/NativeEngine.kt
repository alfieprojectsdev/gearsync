package dev.alfieprojects.gearsync

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
     * Layout: [n_float, mean, m2, ratio0…ratio4, pin0…pin4] (13 floats).
     * Back-compat: an 8-float legacy array loads with pins defaulted to unpinned.
     */
    @JvmStatic external fun resumeCalibrationState(stateArray: FloatArray)

    /**
     * Serialise the current calibration state for SharedPreferences persistence.
     * Layout: [n_float, mean, m2, ratio0…ratio4, pin0…pin4] (13 floats).
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

    // ─── Guided calibration ───────────────────────────────────────────────────

    /** Begin guided per-gear calibration for the given 0-based gear index.
     *  Passive K-Means feeding is suspended until the capture locks or is cancelled. */
    @JvmStatic external fun beginGearCalibration(gear: Int)

    /** Cancel an in-progress guided calibration and discard all captured samples. */
    @JvmStatic external fun cancelCalibration()

    /** Capture progress in [0, 1].
     *  Value is the minimum of sample-count progress and speed-spread progress;
     *  both must reach 1.0 before RANSAC attempts a lock. */
    @JvmStatic external fun getCalibrationProgress(): Float

    /**
     * Called from C++ via the DSP worker JVM upcall when a gear lock completes.
     * Marshals the event to the main thread before notifying [calibrationListener].
     *
     * Must be @JvmStatic so the ProGuard -keep rule for NativeEngine retains it and
     * so the static jmethodID cached in startEngine resolves correctly. (ref: DL-003, C-007)
     *
     * @param gear 0-based gear index that was locked, or -1 when the fitted k_g
     *             broke monotonic order and was rejected. (ref: DL-005)
     */
    @JvmStatic
    fun onGearCalibrated(gear: Int) {
        android.os.Handler(android.os.Looper.getMainLooper()).post {
            calibrationListener?.onGearCalibrated(gear)
        }
    }

    /** Callback interface for guided-calibration lock events.
     *  Implementations must be registered before beginGearCalibration is called. */
    interface CalibrationListener {
        /** @param gear 0-based gear index; -1 signals a failed lock. */
        fun onGearCalibrated(gear: Int)
    }

    /** Register (or clear) the [CalibrationListener].
     *  CalibrationActivity sets this in onCreate and clears it in onDestroy. */
    @Volatile
    var calibrationListener: CalibrationListener? = null
}
