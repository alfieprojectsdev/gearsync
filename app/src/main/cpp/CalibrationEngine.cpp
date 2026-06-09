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

// ── Guided calibration ──────────────────────────────────────────────────────

// Initialise a capture session for gearIndex. Any prior capture data is
// discarded so stale points cannot contaminate the new fit. (ref: DL-006)
void CalibrationEngine::beginGearCalibration(int gearIndex) {
    if (gearIndex < 0 || gearIndex >= NUM_GEARS) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    m_calibGear = gearIndex;
    m_capture.clear();
}

// Return to passive mode and discard all captured samples. (ref: DL-006)
void CalibrationEngine::cancelCalibration() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_calibGear = -1;
    m_capture.clear();
}

// Predicate: true while m_calibGear is a valid gear index.
bool CalibrationEngine::isCalibrating() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_calibGear >= 0;
}

// Returns the 0-based gear index under capture, or -1 when idle.
int CalibrationEngine::calibratingGear() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_calibGear;
}

// RANSAC line-through-origin slope estimator: f = k * v.
//
// Algorithm per iteration:
//   1. Pick a random point i; candidate slope k = f[i] / v[i].
//   2. Count inliers: |f[j] - k*v[j]| <= eps_frac * f[j].
//   3. Track best (highest inlier count) candidate.
// After all iterations: least-squares refit k = sum(v*f)/sum(v*v) over inliers.
//
// Prefers RANSAC over Welford-on-ratio because varied-speed capture spans a
// speed range; the fit must tolerate clutch-slip / pothole outliers that pass
// the GPS stability gate. (ref: DL-001)
//
// Must be called under m_mutex (uses m_rng).
bool CalibrationEngine::ransacFit(const GearCapture& cap, float& kOut) const {
    const int n = static_cast<int>(cap.v.size());
    if (n < RANSAC_N_MIN) return false;

    int   bestCount = 0;
    float bestSlope = 0.0f;

    // m_rng is exclusively accessed under m_mutex; const_cast is safe here.
    std::uniform_int_distribution<int> dist(0, n - 1);
    auto& rng = const_cast<CalibrationEngine*>(this)->m_rng;
    for (int iter = 0; iter < RANSAC_ITERATIONS; ++iter) {
        int   i = dist(rng);
        if (cap.v[i] <= 1e-6f) continue;  // guard divide-by-zero
        float k = cap.f[i] / cap.v[i];

        int count = 0;
        for (int j = 0; j < n; ++j) {
            float residual = std::fabs(cap.f[j] - k * cap.v[j]);
            float eps      = RANSAC_EPS_FRACTION * cap.f[j];  // relative band: 3 % of f
            if (residual <= eps) ++count;
        }
        if (count > bestCount) {
            bestCount = count;
            bestSlope = k;
        }
    }

    if (bestCount < RANSAC_N_MIN) return false;
    // Reject if outlier fraction is too high — e.g. mostly clutch-slip samples.
    if (static_cast<float>(bestCount) / static_cast<float>(n) < RANSAC_INLIER_FRAC)
        return false;

    // Least-squares refit over inlier consensus set for reduced noise.
    double sumVF = 0.0, sumVV = 0.0;
    for (int j = 0; j < n; ++j) {
        float residual = std::fabs(cap.f[j] - bestSlope * cap.v[j]);
        float eps      = RANSAC_EPS_FRACTION * cap.f[j];
        if (residual <= eps) {
            sumVF += static_cast<double>(cap.v[j]) * cap.f[j];
            sumVV += static_cast<double>(cap.v[j]) * cap.v[j];
        }
    }
    if (sumVV < 1e-9) return false;  // degenerate: all inliers at same speed
    kOut = static_cast<float>(sumVF / sumVV);
    return true;
}

// Progress is the minimum of two independent readiness signals:
//   sample_count/N_min — enough points for RANSAC inlier counting.
//   speed_spread/Delta_v_min — enough speed range for a well-conditioned slope.
// Both must reach 1.0 before a lock attempt proceeds. (ref: DL-001)
float CalibrationEngine::calibrationProgress() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_calibGear < 0) return 0.0f;

    const int n = static_cast<int>(m_capture.v.size());
    if (n == 0) return 0.0f;

    float inlierProg = std::min(1.0f, static_cast<float>(n) / static_cast<float>(RANSAC_N_MIN));

    float spreadProg = 0.0f;
    if (n >= 2) {
        float vMin = *std::min_element(m_capture.v.begin(), m_capture.v.end());
        float vMax = *std::max_element(m_capture.v.begin(), m_capture.v.end());
        spreadProg = std::min(1.0f, (vMax - vMin) / RANSAC_DELTA_V_MIN);
    }

    return std::min(inlierProg, spreadProg);
}

// Route a gated (speed, hz) sample to the capture buffer.
// Returns true exactly once — when RANSAC locks the gear — then resets
// m_calibGear so subsequent calls return false until beginGearCalibration
// is called again.
//
// Lock sequence:
//   1. Accumulate sample.
//   2. Gate: need N_min samples and Delta_v_min speed spread.
//   3. ransacFit: reject if inlier count or fraction too low.
//   4. Monotonicity guard: reject if fitted k_g breaks strict descending order.
//   5. Write m_gearRatios[g], set m_pinned[g], re-sort both arrays.
//   6. Release m_mutex before returning true (caller fires JVM upcall). (ref: DL-003)
bool CalibrationEngine::feedCalibrationSample(float speed, float hz) {
    if (speed <= 0.0f || hz <= 0.0f) return false;

    std::unique_lock<std::mutex> lk(m_mutex);
    if (m_calibGear < 0) return false;

    m_capture.v.push_back(speed);
    m_capture.f.push_back(hz);

    const int n = static_cast<int>(m_capture.v.size());
    if (n < RANSAC_N_MIN) return false;

    // Speed spread gate: require RANSAC_DELTA_V_MIN m/s range so the slope
    // is well-conditioned (not a single-point degeneracy). (ref: DL-001)
    float vMin = *std::min_element(m_capture.v.begin(), m_capture.v.end());
    float vMax = *std::max_element(m_capture.v.begin(), m_capture.v.end());
    if (vMax - vMin < RANSAC_DELTA_V_MIN) return false;

    float kFit = 0.0f;
    if (!ransacFit(m_capture, kFit)) return false;

    // Reject a fitted k_g that would break the strictly-descending invariant
    // that classifyGear and needle mapping depend on. (ref: DL-005, C-004)
    int  g       = m_calibGear;
    bool orderOk = true;
    if (g > 0 && kFit >= m_gearRatios[g - 1]) orderOk = false;
    if (g < NUM_GEARS - 1 && kFit <= m_gearRatios[g + 1]) orderOk = false;
    if (!orderOk) {
        m_calibGear = -1;
        m_capture.clear();
        return false;
    }

    m_gearRatios[g] = kFit;
    m_pinned[g]     = true;  // prevent K-Means from re-nudging this centroid (ref: DL-002)
    m_calibGear     = -1;
    m_capture.clear();

    // Re-sort descending: gear 1 (index 0) always has the highest ratio.
    // Pin flags move with their ratios through the permutation. (ref: C-004)
    int idx[NUM_GEARS];
    std::iota(idx, idx + NUM_GEARS, 0);
    std::sort(idx, idx + NUM_GEARS,
              [&](int a, int b) { return m_gearRatios[a] > m_gearRatios[b]; });
    std::array<float, NUM_GEARS> sortedRatios = {};
    std::array<bool,  NUM_GEARS> sortedPinned = {};
    for (int i = 0; i < NUM_GEARS; ++i) {
        sortedRatios[i] = m_gearRatios[idx[i]];
        sortedPinned[i] = m_pinned[idx[i]];
    }
    m_gearRatios = sortedRatios;
    m_pinned     = sortedPinned;

    // Unlock before returning: the DSP worker fires onGearCalibrated only after
    // this function returns, ensuring m_mutex is never held across the JVM upcall.
    // (ref: DL-003)
    lk.unlock();
    return true;
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

// Serialise layout: [n_float, mean, m2, ratio0..ratio4, pin0..pin4] = 13 floats.
// Must be kept in sync with CALIBRATION_STATE_LEN in ShiftAssistantService.kt.
// (ref: DL-002)
void CalibrationEngine::serialise(float* out, int len) const {
    if (len < 3 + NUM_GEARS + NUM_GEARS) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    out[0] = static_cast<float>(m_state.n);
    out[1] = m_state.mean;
    out[2] = m_state.m2;
    for (int g = 0; g < NUM_GEARS; ++g) out[3 + g] = m_gearRatios[g];
    for (int g = 0; g < NUM_GEARS; ++g) out[3 + NUM_GEARS + g] = m_pinned[g] ? 1.0f : 0.0f;
}

void CalibrationEngine::deserialise(const float* in, int len) {
    if (len < 3 + NUM_GEARS) return;  // reject blobs shorter than the 8-float legacy format

    // Validate all incoming values BEFORE acquiring the lock so that corrupted
    // or non-finite storage never partially overwrites engine state. (ref: C-003)
    const float nFloat = in[0];
    if (!std::isfinite(nFloat) || nFloat < 0.0f ||
        !std::isfinite(in[1])  ||
        !std::isfinite(in[2])  || in[2] < 0.0f) {
        return;  // malformed Welford state — leave engine untouched
    }
    for (int g = 0; g < NUM_GEARS; ++g) {
        if (!std::isfinite(in[3 + g])) return;  // non-finite ratio — abort
    }
    // Validate pin flags only when the 13-float layout is present.
    const bool hasPins = (len >= 3 + NUM_GEARS + NUM_GEARS);
    if (hasPins) {
        for (int g = 0; g < NUM_GEARS; ++g) {
            if (!std::isfinite(in[3 + NUM_GEARS + g])) return;
        }
    }

    std::lock_guard<std::mutex> lk(m_mutex);

    m_state.n    = static_cast<int>(nFloat);
    m_state.mean = in[1];
    m_state.m2   = in[2];
    for (int g = 0; g < NUM_GEARS; ++g) m_gearRatios[g] = in[3 + g];

    // Back-compat: old 8-float SharedPreferences blob has no pin flags; default
    // all gears to unpinned so guided locks from a prior install are treated as
    // passive seeds until re-calibrated. (ref: DL-002, R-002)
    for (int g = 0; g < NUM_GEARS; ++g) {
        m_pinned[g] = hasPins && in[3 + NUM_GEARS + g] > 0.5f;
    }

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
    m_pinned            = {};
    m_calibGear         = -1;
    m_capture.clear();
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
                // Skip pinned gears: their centroids were set by guided RANSAC fit
                // and must not be moved by passive samples. (ref: DL-002, DL-005)
                if (m_pinned[k]) continue;
                float d = std::fabs(s - centroids[k]);
                if (d < bestDist) { bestDist = d; best = k; }
            }
            sums[best]   += s;
            counts[best] += 1;
        }

        bool converged = true;
        for (int k = 0; k < NUM_GEARS; ++k) {
            if (m_pinned[k]) continue;  // pinned centroid is never updated (ref: DL-002)
            if (counts[k] == 0) continue;
            float updated = sums[k] / static_cast<float>(counts[k]);
            if (std::fabs(updated - centroids[k]) > 1e-6f) converged = false;
            centroids[k] = updated;
        }
        if (converged) break;
    }

    // Merge updated unpinned centroids back into m_gearRatios; pinned slots are
    // unchanged (their seeded/locked value is preserved).
    for (int g = 0; g < NUM_GEARS; ++g) {
        if (!m_pinned[g]) m_gearRatios[g] = centroids[g];
    }
    // Re-sort descending so the gear-1-highest-ratio invariant holds even when an
    // unpinned centroid crossed a pinned neighbour. Pin flags move with ratios.
    // (ref: C-004)
    int idx[NUM_GEARS];
    std::iota(idx, idx + NUM_GEARS, 0);
    std::sort(idx, idx + NUM_GEARS,
              [&](int a, int b) { return m_gearRatios[a] > m_gearRatios[b]; });
    std::array<float, NUM_GEARS> sorted    = {};
    std::array<bool,  NUM_GEARS> sortedPin = {};
    for (int i = 0; i < NUM_GEARS; ++i) {
        sorted[i]    = m_gearRatios[idx[i]];
        sortedPin[i] = m_pinned[idx[i]];
    }
    m_gearRatios = sorted;
    m_pinned     = sortedPin;
    m_calibrated = true;
}
