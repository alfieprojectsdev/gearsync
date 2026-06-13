// Host-only tests for ADR 004 M4 mic-primary fusion policy.
//
// Build:
//   g++ -std=c++17 -I.. test_fusion_policy_host.cpp -o test_fusion_policy
// Run:
//   ./test_fusion_policy

#include "../FusionPolicy.h"

#include <cassert>
#include <cmath>
#include <cstdio>

// Fusion off: the gate is closed, so the mic estimate passes through untouched and
// behaviour is identical to the legacy mic-only path.
static void test_fusion_off_is_mic_only() {
    const auto d = selectFusedHz(100.0f, 5.0f, /*vibValid=*/true, 104.0f, 9.0f,
                                 /*gateOpen=*/false);
    assert(d.selectedHz == 100.0f);
    assert(d.sourceMode == FUSION_SRC_MIC_ONLY);
    assert(!d.fused);
    std::printf("PASS test_fusion_off_is_mic_only\n");
}

// Agreement: both sources within the tolerance band blend by prominence, landing
// between the two and biased toward the more prominent (vib here).
static void test_agreement_blends_by_prominence() {
    const auto d = selectFusedHz(100.0f, 4.0f, true, 104.0f, 12.0f, true);
    assert(d.fused);
    assert(d.sourceMode == FUSION_SRC_FUSED);
    // Weighted mean of 100 (w=4) and 104 (w=12) = 103.
    assert(std::fabs(d.selectedHz - 103.0f) <= 0.01f);
    assert(d.selectedHz > 100.0f && d.selectedHz < 104.0f);
    std::printf("PASS test_agreement_blends_by_prominence: hz=%.3f\n", d.selectedHz);
}

// Low vibration prominence: rejected, mic kept.
static void test_low_prominence_keeps_mic() {
    const auto d = selectFusedHz(100.0f, 5.0f, true, 101.0f, 2.0f, true);
    assert(d.selectedHz == 100.0f);
    assert(d.sourceMode == FUSION_SRC_REJECTED_LOW_PROMINENCE);
    assert(!d.fused);
    std::printf("PASS test_low_prominence_keeps_mic\n");
}

// Implausible vibration frequency (out of 15..160 band): rejected, mic kept.
static void test_implausible_vib_keeps_mic() {
    const auto d = selectFusedHz(100.0f, 5.0f, true, 320.0f, 20.0f, true);
    assert(d.selectedHz == 100.0f);
    assert(d.sourceMode == FUSION_SRC_REJECTED_LOW_PROMINENCE);
    assert(!d.fused);
    std::printf("PASS test_implausible_vib_keeps_mic\n");
}

// Disagreement with a confident mic: a strong-but-disagreeing vibration cannot
// override a confident mic estimate.
static void test_disagreement_strong_mic_keeps_mic() {
    const auto d = selectFusedHz(100.0f, 5.0f, true, 60.0f, 11.0f, true);
    assert(d.selectedHz == 100.0f);
    assert(d.sourceMode == FUSION_SRC_REJECTED_DISAGREEMENT);
    assert(!d.fused);
    std::printf("PASS test_disagreement_strong_mic_keeps_mic\n");
}

// Disagreement, vibration clearly stronger AND mic weak: vibration takes over.
static void test_disagreement_weak_mic_takes_vib() {
    const auto d = selectFusedHz(100.0f, 1.0f, true, 60.0f, 9.0f, true);
    assert(d.selectedHz == 60.0f);
    assert(d.sourceMode == FUSION_SRC_FUSED);
    assert(d.fused);
    std::printf("PASS test_disagreement_weak_mic_takes_vib\n");
}

int main() {
    test_fusion_off_is_mic_only();
    test_agreement_blends_by_prominence();
    test_low_prominence_keeps_mic();
    test_implausible_vib_keeps_mic();
    test_disagreement_strong_mic_keeps_mic();
    test_disagreement_weak_mic_takes_vib();
    std::printf("All fusion policy tests passed.\n");
    return 0;
}
