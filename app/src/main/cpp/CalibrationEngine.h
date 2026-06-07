#pragma once

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

    // Given the current ratio, return the best-matching gear index (0-based, -1 = unknown).
    int classifyGear(float ratio) const;

    // Gear ratios sorted descending (gear 1 = highest ratio).
    std::array<float, NUM_GEARS> getGearRatios() const;

    // Serialise the Welford state + gear ratios into a flat float array for JNI.
    // Layout: [n_as_float, mean, m2, ratio0…ratio4]  (8 floats total)
    void serialise(float* out, int len) const;

    // Restore Welford state + gear ratios from a flat float array coming from JNI.
    void deserialise(const float* in, int len);

    void reset();

private:
    void runKMeansInternal(std::vector<float>& samples);

    mutable std::mutex              m_mutex;
    WelfordState                    m_state;
    std::vector<float>              m_ratioSamples;
    std::array<float, NUM_GEARS>    m_gearRatios = {};
    bool                            m_calibrated        = false;
    int                             m_lastKMeansSample  = 0;   // n at last K-Means run
    std::mt19937                    m_rng{std::random_device{}()};
};
