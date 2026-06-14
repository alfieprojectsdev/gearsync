// Host-only tests for ADR 004 M3 accelerometer vibration DSP.
//
// Build:
//   g++ -std=c++17 -I.. test_accel_vibration_host.cpp -o test_accel_vibration
// Run:
//   ./test_accel_vibration

#include "../AccelVibrationDsp.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>

static void fillTone(AccelSample* samples,
                     int count,
                     float sampleRateHz,
                     float toneHz,
                     float jitterMs,
                     float amplitude = 1.0f) {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> jitter(-jitterMs, jitterMs);
    const double stepNs = 1.0e9 / static_cast<double>(sampleRateHz);
    const double offsetNs = 1.0e9;
    for (int i = 0; i < count; ++i) {
        const double baseNs = offsetNs + static_cast<double>(i) * stepNs;
        const double jitterNs = static_cast<double>(jitter(rng)) * 1.0e6;
        const double tNs = std::max(i == 0 ? 0.0 : static_cast<double>(samples[i - 1].timestampNs + 1),
                                    baseNs + jitterNs);
        const double tSec = tNs / 1.0e9;
        samples[i].timestampNs = static_cast<int64_t>(std::llround(tNs));
        samples[i].magnitude = 9.81f + amplitude * std::sin(
                2.0f * static_cast<float>(M_PI) * toneHz * static_cast<float>(tSec));
    }
}

static void test_jittered_50hz_recovers_peak() {
    AccelSample samples[ACCEL_VIBRATION_FFT_SIZE]{};
    AccelVibrationScratch scratch{};
    fillTone(samples, ACCEL_VIBRATION_FFT_SIZE, 400.0f, 50.0f, 0.20f);

    const auto estimate = estimateAccelVibrationHz(
            samples, ACCEL_VIBRATION_FFT_SIZE, 400.0f, scratch);
    assert(estimate.valid);
    assert(std::fabs(estimate.hz - 50.0f) <= 2.0f);
    assert(estimate.prominence > 5.0f);
    std::printf("PASS test_jittered_50hz_recovers_peak: hz=%.2f prominence=%.2f\n",
                estimate.hz, estimate.prominence);
}

static void test_insufficient_samples_invalid() {
    AccelSample samples[ACCEL_VIBRATION_FFT_SIZE]{};
    AccelVibrationScratch scratch{};
    fillTone(samples, ACCEL_VIBRATION_FFT_SIZE / 2, 400.0f, 50.0f, 0.10f);

    const auto estimate = estimateAccelVibrationHz(
            samples, ACCEL_VIBRATION_FFT_SIZE / 2, 400.0f, scratch);
    assert(!estimate.valid);
    std::printf("PASS test_insufficient_samples_invalid\n");
}

static void test_nyquist_below_band_invalid() {
    AccelSample samples[ACCEL_VIBRATION_FFT_SIZE]{};
    AccelVibrationScratch scratch{};
    fillTone(samples, ACCEL_VIBRATION_FFT_SIZE, 20.0f, 5.0f, 0.0f);

    const auto estimate = estimateAccelVibrationHz(
            samples, ACCEL_VIBRATION_FFT_SIZE, 20.0f, scratch);
    assert(!estimate.valid);
    assert(estimate.bandMaxHz <= ACCEL_VIBRATION_MIN_HZ);
    std::printf("PASS test_nyquist_below_band_invalid: bandMax=%.2f\n",
                estimate.bandMaxHz);
}

static void test_band_hard_caps_at_160hz() {
    AccelSample samples[ACCEL_VIBRATION_FFT_SIZE]{};
    AccelVibrationScratch scratch{};
    fillTone(samples, ACCEL_VIBRATION_FFT_SIZE, 400.0f, 180.0f, 0.05f);

    const auto estimate = estimateAccelVibrationHz(
            samples, ACCEL_VIBRATION_FFT_SIZE, 400.0f, scratch);
    assert(estimate.valid);
    assert(estimate.bandMaxHz <= 160.0f);
    assert(estimate.hz <= 160.0f);
    std::printf("PASS test_band_hard_caps_at_160hz: hz=%.2f bandMax=%.2f\n",
                estimate.hz, estimate.bandMaxHz);
}

// M5: fundamental + dominant 2nd harmonic. The chassis resonates the 2x harmonic
// stronger than the firing fundamental, so the bare FFT peak sits at 2x.
static void fillHarmonic(AccelSample* samples,
                         int count,
                         float sampleRateHz,
                         float fundamentalHz,
                         float fundAmp,
                         float harmonicMult,
                         float harmonicAmp) {
    const double stepNs = 1.0e9 / static_cast<double>(sampleRateHz);
    const double offsetNs = 1.0e9;
    for (int i = 0; i < count; ++i) {
        const double tNs = offsetNs + static_cast<double>(i) * stepNs;
        const double tSec = tNs / 1.0e9;
        const float ph = 2.0f * static_cast<float>(M_PI) * static_cast<float>(tSec);
        samples[i].timestampNs = static_cast<int64_t>(std::llround(tNs));
        samples[i].magnitude = 9.81f +
                fundAmp * std::sin(ph * fundamentalHz) +
                harmonicAmp * std::sin(ph * fundamentalHz * harmonicMult);
    }
}

// M5: FFT peak at 80 Hz (dominant 2x), true fundamental 40 Hz — guard corrects down.
static void test_harmonic_guard_corrects_2x_to_fundamental() {
    AccelSample samples[ACCEL_VIBRATION_FFT_SIZE]{};
    AccelVibrationScratch scratch{};
    fillHarmonic(samples, ACCEL_VIBRATION_FFT_SIZE, 400.0f,
                 /*fundamental*/40.0f, /*fundAmp*/0.5f, /*mult*/2.0f, /*harmAmp*/1.6f);

    const auto estimate = estimateAccelVibrationHz(
            samples, ACCEL_VIBRATION_FFT_SIZE, 400.0f, scratch);
    assert(estimate.valid);
    assert(std::fabs(estimate.hz - 40.0f) <= 3.0f);  // corrected to fundamental, not 80
    std::printf("PASS test_harmonic_guard_corrects_2x_to_fundamental: hz=%.2f\n", estimate.hz);
}

// M5: a clean single tone (no strong harmonic) must NOT be dragged to a subharmonic.
static void test_harmonic_guard_keeps_clean_fundamental() {
    AccelSample samples[ACCEL_VIBRATION_FFT_SIZE]{};
    AccelVibrationScratch scratch{};
    fillTone(samples, ACCEL_VIBRATION_FFT_SIZE, 400.0f, 60.0f, 0.10f);

    const auto estimate = estimateAccelVibrationHz(
            samples, ACCEL_VIBRATION_FFT_SIZE, 400.0f, scratch);
    assert(estimate.valid);
    assert(std::fabs(estimate.hz - 60.0f) <= 2.0f);  // unchanged
    std::printf("PASS test_harmonic_guard_keeps_clean_fundamental: hz=%.2f\n", estimate.hz);
}

int main() {
    test_jittered_50hz_recovers_peak();
    test_insufficient_samples_invalid();
    test_nyquist_below_band_invalid();
    test_band_hard_caps_at_160hz();
    test_harmonic_guard_corrects_2x_to_fundamental();
    test_harmonic_guard_keeps_clean_fundamental();
    std::printf("All accel vibration tests passed.\n");
    return 0;
}
