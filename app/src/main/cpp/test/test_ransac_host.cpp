// Host-only unit tests for CalibrationEngine RANSAC logic.
//
// Run without Android SDK or NDK — validates the numeric fit before device testing.
// Device testing then focuses on JNI ref scope and end-to-end capture. (ref: DL-007)
//
// Build: g++ -std=c++17 -I.. test_ransac_host.cpp ../CalibrationEngine.cpp -o test_ransac
// Run:   ./test_ransac
//
// Tests:
//   test_ransac_clean_line        — perfect inlier set; slope recovered to < 0.01
//   test_ransac_with_outliers     — 40 inliers + 10 gross outliers; slope within 0.5
//   test_ransac_too_few_samples   — N < N_min returns false
//   test_feed_calibration_sample_locks_gear — full capture-to-lock integration
//   test_order_break_reject       — fit that breaks descending order is rejected
//   test_legacy_deserialise       — 8-float blob leaves pins false; 13-float restores pins
//   test_serialise_roundtrip      — 13-float serialise/deserialise is identity

#include "CalibrationEngine.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>

// Expose protected ransacFit + guided methods for testing via a thin subclass.
class CalibrationEngineTestable : public CalibrationEngine {
public:
    bool ransacFitPublic(const GearCapture& cap, float& kOut) const {
        return ransacFit(cap, kOut);
    }
};

static void test_ransac_clean_line() {
    // Perfect f = k*v points: slope should be recovered exactly.
    const float kTrue = 15.0f;
    GearCapture cap;
    for (int i = 0; i < 50; ++i) {
        float v = 5.0f + i * 0.5f;
        cap.v.push_back(v);
        cap.f.push_back(kTrue * v);
    }
    float kOut = 0.0f;
    CalibrationEngineTestable eng;
    bool ok = eng.ransacFitPublic(cap, kOut);
    assert(ok);
    assert(std::fabs(kOut - kTrue) < 0.01f);
    std::printf("PASS test_ransac_clean_line: k=%.4f (expected %.4f)\n", kOut, kTrue);
}

static void test_ransac_with_outliers() {
    // 40 inliers + 10 gross outliers; RANSAC should still recover slope.
    const float kTrue = 12.5f;
    GearCapture cap;
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> speedDist(8.0f, 22.0f);
    std::uniform_real_distribution<float> noiseDist(-0.1f, 0.1f);
    std::uniform_real_distribution<float> outlierDist(50.0f, 200.0f);
    for (int i = 0; i < 40; ++i) {
        float v = speedDist(rng);
        cap.v.push_back(v);
        cap.f.push_back(kTrue * v + noiseDist(rng));
    }
    for (int i = 0; i < 10; ++i) {
        cap.v.push_back(speedDist(rng));
        cap.f.push_back(outlierDist(rng));
    }
    float kOut = 0.0f;
    CalibrationEngineTestable eng;
    bool ok = eng.ransacFitPublic(cap, kOut);
    assert(ok);
    assert(std::fabs(kOut - kTrue) < 0.5f);
    std::printf("PASS test_ransac_with_outliers: k=%.4f (expected %.4f)\n", kOut, kTrue);
}

static void test_ransac_too_few_samples() {
    GearCapture cap;
    for (int i = 0; i < RANSAC_N_MIN - 1; ++i) {
        float v = 5.0f + i;
        cap.v.push_back(v);
        cap.f.push_back(10.0f * v);
    }
    float kOut = 0.0f;
    CalibrationEngineTestable eng;
    bool ok = eng.ransacFitPublic(cap, kOut);
    assert(!ok);
    std::printf("PASS test_ransac_too_few_samples\n");
}

static void test_feed_calibration_sample_locks_gear() {
    CalibrationEngineTestable eng;
    float seeds[5] = {20.0f, 15.0f, 11.0f, 8.0f, 6.0f};
    eng.seedCentroids(seeds, 5, 0.98f, 1.025f);

    eng.beginGearCalibration(2);  // 3rd gear, k ~ 11.0
    assert(eng.isCalibrating());

    const float kTrue = 11.0f;
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> speedDist(8.0f, 14.0f);
    bool locked = false;
    for (int i = 0; i < 100; ++i) {
        float v = speedDist(rng);
        locked = eng.feedCalibrationSample(v, kTrue * v);
        if (locked) break;
    }
    assert(locked);
    assert(!eng.isCalibrating());
    auto ratios = eng.getGearRatios();
    assert(std::fabs(ratios[2] - kTrue) < 0.5f);
    std::printf("PASS test_feed_calibration_sample_locks_gear: k=%.4f\n", ratios[2]);
}

static void test_order_break_reject() {
    CalibrationEngineTestable eng;
    // Seeds: gear0=20, gear1=15, gear2=11, gear3=8, gear4=6
    float seeds[5] = {20.0f, 15.0f, 11.0f, 8.0f, 6.0f};
    eng.seedCentroids(seeds, 5, 0.98f, 1.025f);
    eng.beginGearCalibration(1);  // gear index 1, expected ~15

    // Feed samples that would produce k=22 > gear0=20 -> breaks descending order.
    const float kBad = 22.0f;
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> speedDist(5.0f, 15.0f);
    bool locked = false;
    for (int i = 0; i < 200; ++i) {
        float v = speedDist(rng);
        locked = eng.feedCalibrationSample(v, kBad * v);
        if (locked) break;
    }
    assert(!locked);
    assert(!eng.isCalibrating());
    std::printf("PASS test_order_break_reject\n");
}

static void test_legacy_deserialise() {
    // 8-float legacy blob: pins must default to false.
    CalibrationEngineTestable eng;
    float legacy[8] = {25.0f, 12.0f, 3.0f, 20.0f, 15.0f, 11.0f, 8.0f, 6.0f};
    eng.deserialise(legacy, 8);
    float out[13]{};
    eng.serialise(out, 13);
    for (int g = 0; g < 5; ++g) assert(out[8 + g] == 0.0f);  // all pins false

    // 13-float blob with pin flags set on gears 0 and 2 must restore them.
    CalibrationEngineTestable eng2;
    float full[13] = {25.0f, 12.0f, 3.0f, 20.0f, 15.0f, 11.0f, 8.0f, 6.0f,
                      1.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    eng2.deserialise(full, 13);
    float out2[13]{};
    eng2.serialise(out2, 13);
    assert(out2[8 + 0] == 1.0f);
    assert(out2[8 + 1] == 0.0f);
    assert(out2[8 + 2] == 1.0f);
    std::printf("PASS test_legacy_deserialise\n");
}

static void test_serialise_roundtrip() {
    CalibrationEngineTestable eng;
    float full[13] = {30.0f, 13.5f, 4.2f, 20.0f, 15.0f, 11.0f, 8.0f, 6.0f,
                      0.0f, 1.0f, 0.0f, 1.0f, 0.0f};
    eng.deserialise(full, 13);
    float out[13]{};
    eng.serialise(out, 13);
    for (int i = 0; i < 13; ++i) assert(std::fabs(out[i] - full[i]) < 1e-4f);
    std::printf("PASS test_serialise_roundtrip\n");
}

int main() {
    test_ransac_clean_line();
    test_ransac_with_outliers();
    test_ransac_too_few_samples();
    test_feed_calibration_sample_locks_gear();
    test_order_break_reject();
    test_legacy_deserialise();
    test_serialise_roundtrip();
    std::printf("All tests passed.\n");
    return 0;
}
