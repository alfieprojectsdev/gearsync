#pragma once

#include "DspPrimitives.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>

static constexpr int   ACCEL_VIBRATION_FFT_SIZE = 256;
static constexpr int   ACCEL_VIBRATION_HOP_SIZE = 64;
static constexpr float ACCEL_VIBRATION_MIN_HZ   = 15.0f;
static constexpr float ACCEL_VIBRATION_MAX_HZ   = 160.0f;

// ─── ADR 004 M5 harmonic guard (required, DL-008) ───────────────────────────
// Car chassis vibration is harmonic-rich; the bare FFT peak can latch a 2x/3x
// firing harmonic that the mount resonates more strongly than the true
// fundamental (both within the 15..160 Hz band). A project-owned autocorrelation
// recovers the true period; if the FFT peak is ~N x the ACF fundamental, prefer
// the fundamental. (phyphox is GPLv3 prior art only — ideas, no copied code.)
static constexpr int   ACCEL_VIBRATION_MAX_HARMONIC = 3;     // check 2x, 3x
static constexpr float ACCEL_VIBRATION_HARMONIC_TOL = 0.15f; // |fftHz/fAcf - N| tolerance
static constexpr float ACCEL_VIBRATION_ACF_MIN      = 0.40f; // min normalized ACF strength to trust
static constexpr float ACCEL_VIBRATION_ACF_PEAKFRAC = 0.85f; // local-peak frac of ACF max → fundamental

struct AccelSample {
    int64_t timestampNs;
    float   magnitude;
};

struct AccelVibrationEstimate {
    bool  valid;
    float hz;
    float prominence;
    float bandMaxHz;
};

struct AccelVibrationScratch {
    float uniform[ACCEL_VIBRATION_FFT_SIZE];
    std::complex<float> fft[ACCEL_VIBRATION_FFT_SIZE];
    float bandMagnitudes[ACCEL_VIBRATION_FFT_SIZE / 2];
    float detrended[ACCEL_VIBRATION_FFT_SIZE];   // M5: DC-removed signal for autocorrelation
    float acf[ACCEL_VIBRATION_FFT_SIZE / 2];     // M5: normalized autocorrelation over lag range
};

// M5 harmonic guard: estimate the fundamental period of `x` (length n, DC-removed)
// via normalized autocorrelation, searching the [minHz, maxHz] band. Returns the
// fundamental Hz and writes its normalized ACF strength to outStrength; returns 0
// when the signal has no usable period. Picks the *smallest* lag whose ACF peak is
// within ACF_PEAKFRAC of the maximum — this avoids the classic ACF octave-down
// (subharmonic) error that a plain global-max would hit on periodic signals.
inline float estimateFundamentalAcfHz(
        const float* x, int n, float sampleRateHz,
        float minHz, float maxHz, float* acfScratch, float& outStrength) {
    outStrength = 0.0f;
    if (!x || n <= 0 || !std::isfinite(sampleRateHz) || sampleRateHz <= 0.0f) return 0.0f;

    int lagMin = static_cast<int>(std::floor(sampleRateHz / maxHz));
    int lagMax = static_cast<int>(std::ceil(sampleRateHz / minHz));
    lagMin = std::max(2, lagMin);
    lagMax = std::min(n / 2, lagMax);
    if (lagMax <= lagMin) return 0.0f;

    double energy = 0.0;
    for (int i = 0; i < n; ++i) energy += static_cast<double>(x[i]) * x[i];
    if (energy <= 1.0e-12) return 0.0f;

    float acfMax = 0.0f;
    for (int lag = lagMin; lag <= lagMax; ++lag) {
        double acc = 0.0;
        for (int i = 0; i + lag < n; ++i) acc += static_cast<double>(x[i]) * x[i + lag];
        const float r = static_cast<float>(acc / energy);
        acfScratch[lag - lagMin] = r;
        if (r > acfMax) acfMax = r;
    }
    if (acfMax <= 0.0f) return 0.0f;

    const float threshold = ACCEL_VIBRATION_ACF_PEAKFRAC * acfMax;
    const int count = lagMax - lagMin + 1;
    for (int idx = 1; idx < count - 1; ++idx) {
        const float r = acfScratch[idx];
        if (r >= threshold &&
            r >= acfScratch[idx - 1] && r >= acfScratch[idx + 1]) {
            outStrength = r;
            return sampleRateHz / static_cast<float>(lagMin + idx);
        }
    }
    return 0.0f;
}

inline AccelVibrationEstimate estimateAccelVibrationHz(
        const AccelSample* samples,
        int sampleCount,
        float sampleRateHz,
        AccelVibrationScratch& scratch) {
    if (!samples ||
        sampleCount < ACCEL_VIBRATION_FFT_SIZE ||
        !std::isfinite(sampleRateHz) ||
        sampleRateHz <= 0.0f) {
        return {false, 0.0f, 0.0f, 0.0f};
    }

    const float nyquist = sampleRateHz * 0.5f;
    const float bandMaxHz = std::min(ACCEL_VIBRATION_MAX_HZ, nyquist);
    if (bandMaxHz <= ACCEL_VIBRATION_MIN_HZ) {
        return {false, 0.0f, 0.0f, bandMaxHz};
    }

    const int start = sampleCount - ACCEL_VIBRATION_FFT_SIZE;
    const AccelSample* window = samples + start;
    const int64_t firstTs = window[0].timestampNs;
    const int64_t lastTs = window[ACCEL_VIBRATION_FFT_SIZE - 1].timestampNs;
    if (firstTs <= 0 || lastTs <= firstTs) {
        return {false, 0.0f, 0.0f, bandMaxHz};
    }

    const double stepNs = 1.0e9 / static_cast<double>(sampleRateHz);
    const double startNs = static_cast<double>(firstTs);
    int src = 0;
    float sum = 0.0f;
    for (int i = 0; i < ACCEL_VIBRATION_FFT_SIZE; ++i) {
        const double targetNs = startNs + static_cast<double>(i) * stepNs;
        while (src + 1 < ACCEL_VIBRATION_FFT_SIZE &&
               static_cast<double>(window[src + 1].timestampNs) < targetNs) {
            ++src;
        }

        float v = window[src].magnitude;
        if (src + 1 < ACCEL_VIBRATION_FFT_SIZE) {
            const double t0 = static_cast<double>(window[src].timestampNs);
            const double t1 = static_cast<double>(window[src + 1].timestampNs);
            if (t1 > t0 && targetNs > t0) {
                const double frac = std::min(1.0, std::max(0.0, (targetNs - t0) / (t1 - t0)));
                v = window[src].magnitude +
                    static_cast<float>(frac) * (window[src + 1].magnitude - window[src].magnitude);
            }
        }
        if (!std::isfinite(v)) {
            return {false, 0.0f, 0.0f, bandMaxHz};
        }
        scratch.uniform[i] = v;
        sum += v;
    }

    const float mean = sum / static_cast<float>(ACCEL_VIBRATION_FFT_SIZE);
    for (int i = 0; i < ACCEL_VIBRATION_FFT_SIZE; ++i) {
        const float w = 0.54f - 0.46f * std::cos(
                2.0f * static_cast<float>(M_PI) * i /
                static_cast<float>(ACCEL_VIBRATION_FFT_SIZE - 1));
        scratch.fft[i] = {(scratch.uniform[i] - mean) * w, 0.0f};
    }
    fft_inplace(scratch.fft, ACCEL_VIBRATION_FFT_SIZE);

    int binMin = static_cast<int>(std::ceil(
            ACCEL_VIBRATION_MIN_HZ * ACCEL_VIBRATION_FFT_SIZE / sampleRateHz));
    int binMax = static_cast<int>(std::floor(
            bandMaxHz * ACCEL_VIBRATION_FFT_SIZE / sampleRateHz));
    binMin = std::max(1, binMin);
    binMax = std::min(binMax, ACCEL_VIBRATION_FFT_SIZE / 2 - 1);
    if (binMax < binMin) {
        return {false, 0.0f, 0.0f, bandMaxHz};
    }

    float peakMag = 0.0f;
    int peakBin = binMin;
    int magCount = 0;
    for (int b = binMin; b <= binMax; ++b) {
        const float mag = std::abs(scratch.fft[b]);
        scratch.bandMagnitudes[magCount++] = mag;
        if (mag > peakMag) {
            peakMag = mag;
            peakBin = b;
        }
    }
    if (magCount <= 0 || peakMag <= 0.0f) {
        return {false, 0.0f, 0.0f, bandMaxHz};
    }

    const int mid = magCount / 2;
    std::nth_element(scratch.bandMagnitudes,
                     scratch.bandMagnitudes + mid,
                     scratch.bandMagnitudes + magCount);
    const float noiseFloor = std::max(scratch.bandMagnitudes[mid], 1.0e-6f);
    const float fftHz = static_cast<float>(peakBin) * sampleRateHz /
                        static_cast<float>(ACCEL_VIBRATION_FFT_SIZE);

    // ─── M5 harmonic guard ──────────────────────────────────────────────────
    // If the FFT peak is ~N x (N=2,3) a strong autocorrelation fundamental that
    // is itself plausible (>= MIN_HZ), the FFT latched a harmonic — correct down
    // to the fundamental. Otherwise keep the FFT estimate untouched.
    float hz = fftHz;
    for (int i = 0; i < ACCEL_VIBRATION_FFT_SIZE; ++i) {
        scratch.detrended[i] = scratch.uniform[i] - mean;
    }
    float acfStrength = 0.0f;
    const float fAcf = estimateFundamentalAcfHz(
            scratch.detrended, ACCEL_VIBRATION_FFT_SIZE, sampleRateHz,
            ACCEL_VIBRATION_MIN_HZ, bandMaxHz, scratch.acf, acfStrength);
    if (fAcf >= ACCEL_VIBRATION_MIN_HZ && acfStrength >= ACCEL_VIBRATION_ACF_MIN) {
        const float ratio = fftHz / fAcf;
        const int n = static_cast<int>(std::lround(ratio));
        if (n >= 2 && n <= ACCEL_VIBRATION_MAX_HARMONIC &&
            std::fabs(ratio - static_cast<float>(n)) <= ACCEL_VIBRATION_HARMONIC_TOL) {
            hz = fAcf;
        }
    }

    return {true, hz, peakMag / noiseFloor, bandMaxHz};
}
