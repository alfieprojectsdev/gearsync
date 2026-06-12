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
     * Native engine VU payload:
     * [needlePos, dominantHz, speedMps, gear (1-based, 0=unknown),
     *  confidence, shiftDetected (1.0 = event pending, then cleared)].
     */
    @JvmStatic external fun nativeVUMeterState(): FloatArray?

    /**
     * ADR 004 Milestone 0 diagnostic — effective raw-ACCELEROMETER delivery rate.
     * Returns [effectiveHz, minIntervalMs, maxIntervalMs, jitterMs,
     *          cumulativeSamples, supported (1=yes, 0=no, -1=unknown)].
     * effectiveHz ≥ 300 means this device's sensor path clears the vibration-fusion
     * gate. Stationary on a desk is sufficient — this measures capability, not engine
     * vibration. (see plans/accel-fft-sensor-fusion-implementation-plan.md M0)
     */
    @JvmStatic external fun nativeAccelProbeStats(): FloatArray?

    /**
     * ADR 004 diagnostic — vibration-fusion feature gate/state plus M3 accel FFT estimate.
     * Returns float[12]:
     * [requestedAccelHz, measuredAccelHz, useVibrationFusion (1/0),
     *  fusionActive (1/0), disabledReasonCode, latestVibrationHz,
     *  vibrationProminence, sourceModeCode, accelRingWritten, accelRingRead,
     *  accelRingDropped, latestAccelMagnitude].
     *
     * Reason codes: 0 none, 1 config disabled, 2 accelerometer unsupported,
     * 3 low accel rate, 4 fusion policy pending (f_vib is diagnostic-only).
     * Source modes: 0 MIC_ONLY, 1 FUSED, 2 VIB_REJECTED_LOW_RATE,
     * 3 VIB_REJECTED_LOW_PROMINENCE.
     */
    @JvmStatic external fun nativeVibrationFusionStats(): FloatArray?

    /**
     * VU state consumed by VUMeterView at 60 FPS. Normally proxies the native
     * engine; in the `sweep` build type (BuildConfig.VU_SWEEP) it returns a
     * synthetic ramp from [DebugSweep] so a built APK self-demos the meter with
     * no mic / GPS / sensors. The flag is false in debug and release builds, so
     * production behavior is unchanged. (see app/build.gradle `sweep` build type)
     */
    @JvmStatic
    fun getVUMeterState(): FloatArray? =
        if (BuildConfig.VU_SWEEP) DebugSweep.nextFrame() else nativeVUMeterState()

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
     * @param useVibrationFusion       Opt-in ADR 004 vibration fusion flag. Defaults false in config.
     */
    @JvmStatic external fun setVehicleConfig(
        kSeeds: FloatArray,
        toleranceLow: Float,
        toleranceHigh: Float,
        stabilityWindowSamples: Int,
        speedJitterThresholdMps: Float,
        useVibrationFusion: Boolean
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
