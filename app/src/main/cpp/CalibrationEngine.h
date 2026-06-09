#pragma once

// CalibrationEngine — gear-ratio learning and classification.
//
// Passive path (default): Welford online statistics + seeded K-Means refine
// gear-ratio centroids from live (hz, speed) pairs; seeds come from
// vehicle_config.json via seedCentroids, so classifyGear works from first drive.
//
// Guided path (additive): RANSAC slope fit on a deliberate per-gear capture
// overwrites and pins one centroid; K-Means skips pinned gears thereafter.
// (ref: DL-001, DL-002, DL-005)

#include <array>
#include <mutex>
#include <random>
#include <vector>

static constexpr int NUM_GEARS = 5;
static constexpr int MIN_SAMPLES_FOR_KMEANS  = 20;
static constexpr int KMEANS_UPDATE_INTERVAL  = 10;
static constexpr int KMEANS_ITERATIONS       = 100;
static constexpr int MAX_KMEANS_SAMPLES      = 4096; // training window cap
static constexpr int KMEANS_PRUNE_BATCH      = 512;  // entries evicted when cap is exceeded

// Welford Online Algorithm state — persisted across non-contiguous sessions.
struct WelfordState {
    int   n    = 0;
    float mean = 0.0f;
    float m2   = 0.0f;
};

// Speed/frequency sample buffer for one guided-calibration capture session.
// Only one gear is captured at a time; cleared on begin, cancel, and lock.
// (ref: DL-006)
struct GearCapture {
    std::vector<float> v;  // GPS speed samples (m/s)
    std::vector<float> f;  // dominant FFT frequency samples (Hz)
    void clear() { v.clear(); f.clear(); }
};

// Tunable RANSAC parameters — adjust on device after host-unit validation.
// (ref: DL-001, DL-007)
static constexpr int   RANSAC_ITERATIONS  = 100;    // candidate slopes per fit attempt
static constexpr int   RANSAC_N_MIN       = 20;     // min inliers to accept a fit
static constexpr float RANSAC_EPS_FRACTION = 0.03f; // inlier band width: 3 % of f
static constexpr float RANSAC_INLIER_FRAC = 0.6f;   // min inlier fraction to lock
static constexpr float RANSAC_DELTA_V_MIN = 2.0f;   // m/s speed spread required for lock

class CalibrationEngine {
public:
    CalibrationEngine() = default;

    // Feed a new ratio observation (Hz / m·s⁻¹) into the running state.
    void updateWelford(float ratio);

    // Population variance from Welford state.
    float getVariance() const;
    float getMean()     const;
    int   getSampleCount() const;

    // Run 1-D K-Means over all stored ratio samples and lock in gear centroids.
    void runKMeans();

    // Seed initial centroids from vehicle config (theoretical k_g values, descending).
    // Also sets the asymmetric tolerance band used by classifyGear.
    // K-Means will refine these centroids as real observations accumulate.
    void seedCentroids(const float* seeds, int n, float tolLow, float tolHigh);

    // Given the current ratio, return the best-matching gear index (0-based).
    // Returns -1 when uncalibrated/unseeded, or when the ratio falls outside the
    // configured tolerance band [centroid * tolLow, centroid * tolHigh] for every gear.
    int classifyGear(float ratio) const;

    // Gear ratios sorted descending (gear 1 = highest ratio).
    std::array<float, NUM_GEARS> getGearRatios() const;

    // ── Guided calibration ────────────────────────────────────────────────────

    // Begin capture for gearIndex (0-based). Clears any previous capture data.
    // Passive Welford/K-Means feeding is suppressed until cancelCalibration or lock.
    // No-op if gearIndex is out of range. (ref: DL-005, DL-006)
    void beginGearCalibration(int gearIndex);

    // Discard capture buffer and return engine to passive mode. (ref: DL-006)
    void cancelCalibration();

    // Returns true while a capture is active (begun, not yet locked or cancelled).
    bool isCalibrating() const;

    // Returns the 0-based gear index under capture, or -1 when idle.
    int  calibratingGear() const;

    // Feed a gated (speed, hz) pair to the capture buffer.
    // Returns true only when RANSAC locks successfully; on true, the fitted
    // k_g replaces m_gearRatios[g], sets m_pinned[g], and resets m_calibGear.
    // The caller (DSP worker in native-lib) fires onGearCalibrated *after* this
    // returns to avoid holding m_mutex across the JVM upcall. (ref: DL-003, DL-005)
    bool feedCalibrationSample(float speed, float hz);

    // Capture progress in [0, 1]: min(sample_count/N_min, speed_spread/Delta_v_min).
    // Zero when no capture is active.
    float calibrationProgress() const;

    // ── Persistence ───────────────────────────────────────────────────────────

    // Serialise Welford state + gear ratios + pin flags into a flat float array.
    // Layout: [n_as_float, mean, m2, ratio0…ratio4, pin0…pin4]  (13 floats total)
    // Must match CALIBRATION_STATE_LEN in ShiftAssistantService. (ref: DL-002)
    void serialise(float* out, int len) const;

    // Restore Welford state + gear ratios + pin flags from a flat float array.
    // Back-compat: if len == 8 (old format), pin flags default to unpinned (0.0).
    // Validates every incoming float before mutating state. (ref: DL-002, C-003)
    void deserialise(const float* in, int len);

    void reset();

protected:
    // RANSAC line-through-origin slope fit on cap.
    // Samples one candidate k = f/v per iteration, counts inliers in a relative
    // residual band, then least-squares refits the best inlier set.
    // Returns false if N_min or RANSAC_INLIER_FRAC thresholds are not met.
    // Protected (not private) for access from host unit tests. (ref: DL-001, DL-007)
    bool ransacFit(const GearCapture& cap, float& kOut) const;

private:
    void runKMeansInternal(std::vector<float>& samples);

    mutable std::mutex              m_mutex;
    WelfordState                    m_state;
    std::vector<float>              m_ratioSamples;
    std::array<float, NUM_GEARS>    m_gearRatios = {};
    bool                            m_calibrated        = false;
    int                             m_lastKMeansSample  = 0;   // n at last K-Means run
    std::mt19937                    m_rng{std::random_device{}()};

    // Asymmetric tolerance band set by seedCentroids (0 = open/unconfigured).
    // classifyGear accepts ratio r for centroid c when c*tolLow <= r <= c*tolHigh.
    float                           m_tolLow  = 0.0f;
    float                           m_tolHigh = 0.0f;

    // Guided calibration state.
    GearCapture                     m_capture;
    int                             m_calibGear = -1;  // -1 = IDLE

    // Pin flags: pinned gears are skipped in K-Means reassignment. (ref: DL-002)
    std::array<bool, NUM_GEARS>     m_pinned = {};
};
