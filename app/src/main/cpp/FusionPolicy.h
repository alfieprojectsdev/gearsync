#pragma once

// ADR 004 M4 — mic-primary fusion policy.
//
// Pure, header-only, allocation-free, and Android-free so the same logic the DSP
// worker runs is exercised by host tests. The mic acoustic estimate is always the
// primary source; the accelerometer vibration estimate (ADR 004 M3) can only
// refine or, in a narrow case, replace it. The vibration FFT *will* latch aliased
// harmonics on this device (200 Hz Nyquist, DL-008); the M5 guard cleans the
// estimate upstream — M4 only decides how much to trust whatever M3 published.

#include <algorithm>
#include <cmath>

// ─── Tunable thresholds ──────────────────────────────────────────────────────
static constexpr float FUSION_AGREE_TOL    = 0.10f;  // |micHz-vibHz|/micHz agreement band
static constexpr float FUSION_VIB_PROM_MIN = 4.0f;   // min vibration prominence to consider at all
static constexpr float FUSION_VIB_STRONGER = 2.0f;   // on disagreement, vib must beat mic prominence by this
static constexpr float FUSION_MIC_WEAK     = 2.0f;   // ... and mic prominence must be below this to be overridden
static constexpr float FUSION_VIB_MIN_HZ   = 15.0f;  // plausibility band, mirrors AccelVibrationDsp
static constexpr float FUSION_VIB_MAX_HZ   = 160.0f;

// Mirrors VibrationSourceMode in native-lib.cpp — kept numerically in lockstep so
// the decision's sourceMode can be stored straight into g_vibrationSourceMode.
enum FusionSourceMode : int {
    FUSION_SRC_MIC_ONLY = 0,
    FUSION_SRC_FUSED = 1,
    FUSION_SRC_REJECTED_LOW_RATE = 2,        // set by the gate, not by selectFusedHz
    FUSION_SRC_REJECTED_LOW_PROMINENCE = 3,  // also covers invalid/implausible vibration
    FUSION_SRC_REJECTED_DISAGREEMENT = 4
};

struct FusionDecision {
    float selectedHz;  // frequency fed into ratio = selectedHz / speed
    int   sourceMode;  // FusionSourceMode
    bool  fused;       // true only when vibration actually influenced selectedHz
};

// gateOpen == (opt-in && accelerometer supported && measured rate >= gate). When
// it is false, this returns the mic estimate untouched — the fusion-off path is
// bit-for-bit the legacy mic-only behaviour.
inline FusionDecision selectFusedHz(
        float micHz, float micProm,
        bool vibValid, float vibHz, float vibProm,
        bool gateOpen) {
    // Mic is primary. No fusion unless the gate is open and the mic estimate is sane.
    if (!gateOpen || !std::isfinite(micHz) || micHz <= 0.0f) {
        return {micHz, FUSION_SRC_MIC_ONLY, false};
    }

    // Vibration must be a valid, prominent, plausible estimate to be considered.
    if (!vibValid || !std::isfinite(vibHz) ||
        vibHz < FUSION_VIB_MIN_HZ || vibHz > FUSION_VIB_MAX_HZ ||
        vibProm < FUSION_VIB_PROM_MIN) {
        return {micHz, FUSION_SRC_REJECTED_LOW_PROMINENCE, false};
    }

    const float rel = std::fabs(micHz - vibHz) / micHz;
    if (rel <= FUSION_AGREE_TOL) {
        // Agreement: confidence-weighted blend by prominence. Both prominences are
        // peak/median-noise ratios from the same FFT recipe, so directly comparable.
        const float wMic = std::max(micProm, 0.0f);
        const float wVib = std::max(vibProm, 0.0f);
        const float wSum = wMic + wVib;
        const float hz = wSum > 1.0e-6f
                ? (wMic * micHz + wVib * vibHz) / wSum
                : micHz;
        return {hz, FUSION_SRC_FUSED, true};
    }

    // Disagreement: keep mic unless vibration is clearly stronger AND mic is weak.
    // A noisy/implausible vibration peak cannot override a confident mic estimate.
    if (vibProm > micProm * FUSION_VIB_STRONGER && micProm < FUSION_MIC_WEAK) {
        return {vibHz, FUSION_SRC_FUSED, true};
    }
    return {micHz, FUSION_SRC_REJECTED_DISAGREEMENT, false};
}
