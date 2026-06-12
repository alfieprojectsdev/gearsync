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
};

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
    const float hz = static_cast<float>(peakBin) * sampleRateHz /
                     static_cast<float>(ACCEL_VIBRATION_FFT_SIZE);
    return {true, hz, peakMag / noiseFloor, bandMaxHz};
}
