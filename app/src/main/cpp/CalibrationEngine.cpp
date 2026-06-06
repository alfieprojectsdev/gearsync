#include "CalibrationEngine.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>

void CalibrationEngine::updateWelford(float ratio) {
    std::lock_guard<std::mutex> lk(m_mutex);

    m_state.n++;
    float delta  = ratio - m_state.mean;
    m_state.mean += delta / static_cast<float>(m_state.n);
    float delta2  = ratio - m_state.mean;
    m_state.m2   += delta * delta2;

    m_ratioSamples.push_back(ratio);

    if (static_cast<int>(m_ratioSamples.size()) >= MIN_SAMPLES_FOR_KMEANS &&
        m_state.n % KMEANS_UPDATE_INTERVAL == 0) {
        runKMeansInternal(m_ratioSamples);
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

int CalibrationEngine::classifyGear(float ratio) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_calibrated) return -1;

    int   best     = 0;
    float bestDist = FLT_MAX;
    for (int g = 0; g < NUM_GEARS; ++g) {
        float d = std::fabs(ratio - m_gearRatios[g]);
        if (d < bestDist) { bestDist = d; best = g; }
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
    std::lock_guard<std::mutex> lk(m_mutex);
    m_state.n    = static_cast<int>(in[0]);
    m_state.mean = in[1];
    m_state.m2   = in[2];
    for (int g = 0; g < NUM_GEARS; ++g) m_gearRatios[g] = in[3 + g];
    m_calibrated = (m_state.n >= MIN_SAMPLES_FOR_KMEANS);
}

void CalibrationEngine::reset() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_state      = {};
    m_gearRatios = {};
    m_ratioSamples.clear();
    m_calibrated = false;
}

void CalibrationEngine::runKMeans() {
    std::lock_guard<std::mutex> lk(m_mutex);
    runKMeansInternal(m_ratioSamples);
}

// 1-D K-Means with k=5, k-means++ seeding. Caller must hold m_mutex.
void CalibrationEngine::runKMeansInternal(std::vector<float>& samples) {
    const int n = static_cast<int>(samples.size());
    if (n < NUM_GEARS) return;

    float centroids[NUM_GEARS] = {};

    // k-means++ initialisation: pick first centroid randomly, then each next
    // centroid with probability proportional to the squared distance from the
    // nearest existing centroid.
    centroids[0] = samples[static_cast<std::size_t>(std::rand()) % samples.size()];

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

    // Sort descending so index 0 = gear 1 (highest ratio = lowest speed gear).
    std::sort(centroids, centroids + NUM_GEARS, std::greater<float>());
    for (int g = 0; g < NUM_GEARS; ++g) m_gearRatios[g] = centroids[g];
    m_calibrated = true;
}
