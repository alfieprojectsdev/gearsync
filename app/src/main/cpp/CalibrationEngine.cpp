#include "CalibrationEngine.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <numeric>

void CalibrationEngine::updateWelford(float ratio) {
    std::lock_guard<std::mutex> lk(m_mutex);

    m_state.n++;
    float delta  = ratio - m_state.mean;
    m_state.mean += delta / static_cast<float>(m_state.n);
    float delta2  = ratio - m_state.mean;
    m_state.m2   += delta * delta2;

    m_ratioSamples.push_back(ratio);

    // Maintain a bounded sliding window so K-Means never iterates over an
    // unboundedly growing vector. Evict the oldest KMEANS_PRUNE_BATCH entries
    // in a single erase call (amortised O(1) per sample) when the cap is hit.
    if (static_cast<int>(m_ratioSamples.size()) > MAX_KMEANS_SAMPLES + KMEANS_PRUNE_BATCH) {
        m_ratioSamples.erase(m_ratioSamples.begin(),
                             m_ratioSamples.begin() + KMEANS_PRUNE_BATCH);
    }

    // Use (n - lastRun) so the interval is consistent even after deserialise restores
    // an arbitrary n, which would otherwise cause the plain `n % interval` check to skip.
    if (m_state.n >= MIN_SAMPLES_FOR_KMEANS &&
        (m_state.n - m_lastKMeansSample) % KMEANS_UPDATE_INTERVAL == 0) {
        runKMeansInternal(m_ratioSamples);
        m_lastKMeansSample = m_state.n;
    }
}

float CalibrationEngine::getVariance() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_state.n < 2) return 0.0f;
    return m_state.m2 / static_cast<float>(m_state.n - 1);
}

float CalibrationEngine::getMean() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_state.mean;
}

int CalibrationEngine::getSampleCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_state.n;
}

void CalibrationEngine::seedCentroids(const float* seeds, int n, float tolLow, float tolHigh) {
    if (n < NUM_GEARS || !seeds) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    for (int g = 0; g < NUM_GEARS; ++g) m_gearRatios[g] = seeds[g];
    m_tolLow     = tolLow;
    m_tolHigh    = tolHigh;
    m_calibrated = true;
}

int CalibrationEngine::classifyGear(float ratio) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_calibrated) return -1;

    int   best     = -1;
    float bestDist = FLT_MAX;
    for (int g = 0; g < NUM_GEARS; ++g) {
        float d = std::fabs(ratio - m_gearRatios[g]);
        if (d < bestDist) { bestDist = d; best = g; }
    }

    // When tolerance is configured, reject ratios outside [centroid*tolLow, centroid*tolHigh].
    if (best >= 0 && m_tolLow > 0.0f && m_tolHigh > 0.0f) {
        float c = m_gearRatios[best];
        if (ratio < c * m_tolLow || ratio > c * m_tolHigh) return -1;
    }

    return best;
}

std::array<float, NUM_GEARS> CalibrationEngine::getGearRatios() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_gearRatios;
}

// Serialise layout: [n_float, mean, m2, ratio0, ratio1, ratio2, ratio3, ratio4]
void CalibrationEngine::serialise(float* out, int len) const {
    if (len < 3 + NUM_GEARS) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    out[0] = static_cast<float>(m_state.n);
    out[1] = m_state.mean;
    out[2] = m_state.m2;
    for (int g = 0; g < NUM_GEARS; ++g) out[3 + g] = m_gearRatios[g];
}

void CalibrationEngine::deserialise(const float* in, int len) {
    if (len < 3 + NUM_GEARS) return;

    // Validate all incoming values BEFORE acquiring the lock so that corrupted
    // or non-finite storage never partially overwrites engine state.
    const float nFloat = in[0];
    if (!std::isfinite(nFloat) || nFloat < 0.0f ||
        !std::isfinite(in[1])  ||
        !std::isfinite(in[2])  || in[2] < 0.0f) {
        return;  // malformed Welford state — leave engine untouched
    }
    for (int g = 0; g < NUM_GEARS; ++g) {
        if (!std::isfinite(in[3 + g])) return;  // non-finite ratio — abort
    }

    std::lock_guard<std::mutex> lk(m_mutex);

    m_state.n    = static_cast<int>(nFloat);
    m_state.mean = in[1];
    m_state.m2   = in[2];
    for (int g = 0; g < NUM_GEARS; ++g) m_gearRatios[g] = in[3 + g];

    // Require at least one non-zero ratio; all-zero means K-Means never ran.
    bool hasValidRatios = false;
    for (int g = 0; g < NUM_GEARS; ++g) {
        if (std::fabs(m_gearRatios[g]) > 1e-6f) { hasValidRatios = true; break; }
    }
    m_calibrated = (m_state.n >= MIN_SAMPLES_FOR_KMEANS) && hasValidRatios;

    // Clear the in-session sample vector (it is not serialised) and reset the
    // K-Means trigger baseline so the next KMEANS_UPDATE_INTERVAL new samples
    // fire a run rather than skipping due to an arbitrary restored n.
    m_ratioSamples.clear();
    m_lastKMeansSample = m_state.n;
}

void CalibrationEngine::reset() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_state             = {};
    m_gearRatios        = {};
    m_ratioSamples.clear();
    m_calibrated        = false;
    m_lastKMeansSample  = 0;
    m_tolLow            = 0.0f;
    m_tolHigh           = 0.0f;
}

void CalibrationEngine::runKMeans() {
    std::lock_guard<std::mutex> lk(m_mutex);
    runKMeansInternal(m_ratioSamples);
}

// 1-D K-Means with farthest-first initialisation (deterministic max-distance
// heuristic — simpler than probabilistic k-means++ and well-suited to 1-D data).
// Caller must hold m_mutex.
void CalibrationEngine::runKMeansInternal(std::vector<float>& samples) {
    const int n = static_cast<int>(samples.size());
    if (n < NUM_GEARS) return;

    float centroids[NUM_GEARS] = {};

    // Pick first centroid uniformly at random using the engine-local mt19937.
    std::uniform_int_distribution<std::size_t> dist(0, samples.size() - 1);
    centroids[0] = samples[dist(m_rng)];

    // Each subsequent centroid is the sample farthest from all existing ones.
    for (int k = 1; k < NUM_GEARS; ++k) {
        float maxDist = -1.0f;
        for (float s : samples) {
            float minDist = FLT_MAX;
            for (int j = 0; j < k; ++j) {
                float d = std::fabs(s - centroids[j]);
                if (d < minDist) minDist = d;
            }
            if (minDist > maxDist) { maxDist = minDist; centroids[k] = s; }
        }
    }

    // Lloyd's iterations.
    for (int iter = 0; iter < KMEANS_ITERATIONS; ++iter) {
        float sums[NUM_GEARS]   = {};
        int   counts[NUM_GEARS] = {};

        for (float s : samples) {
            int   best     = 0;
            float bestDist = FLT_MAX;
            for (int k = 0; k < NUM_GEARS; ++k) {
                float d = std::fabs(s - centroids[k]);
                if (d < bestDist) { bestDist = d; best = k; }
            }
            sums[best]   += s;
            counts[best] += 1;
        }

        bool converged = true;
        for (int k = 0; k < NUM_GEARS; ++k) {
            if (counts[k] == 0) continue;
            float updated = sums[k] / static_cast<float>(counts[k]);
            if (std::fabs(updated - centroids[k]) > 1e-6f) converged = false;
            centroids[k] = updated;
        }
        if (converged) break;
    }

    // Sort descending: index 0 = gear 1 (highest ratio = lowest speed gear).
    std::sort(centroids, centroids + NUM_GEARS, std::greater<float>());
    for (int g = 0; g < NUM_GEARS; ++g) m_gearRatios[g] = centroids[g];
    m_calibrated = true;
}
