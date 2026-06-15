#pragma once

// ADR 006 — pure out-of-band shift-cue tone synth (host-testable, no Android deps).
//
// Generates a linear chirp with a Hann amplitude envelope (no click transients).
// Cues live at 1.5–3 kHz so they sit far above the 20–250 Hz engine FFT band
// (`findDominantHz`) and cannot inject a false engine peak even via mic pickup.
// Pitch-direction = shift-direction (DL-AUD-1): ascending = upshift, descending =
// downshift. This is the reusable core a Shared-output CuePlayer (M2) would render.

#include <cmath>

struct ChirpSpec {
    float startHz;
    float endHz;
    float durationSec;
    float amplitude;   // 0..1
};

// Both cues occupy the same 1.5–2.2 kHz band (well above the 20–250 Hz engine
// band); they are distinguished purely by sweep DIRECTION (DL-AUD-1), so a
// descending mirror is used rather than dropping to a lower frequency.
inline ChirpSpec upshiftCue()   { return {1500.0f, 2200.0f, 0.12f, 0.6f}; }  // ascending
inline ChirpSpec downshiftCue() { return {2200.0f, 1500.0f, 0.12f, 0.6f}; }  // descending

// Renders the chirp into `out` (length n) at sampleRateHz: linear instantaneous
// frequency start→end across the buffer, Hann amplitude envelope. Pure, no alloc;
// the caller owns the buffer. Returns samples written (== n on success).
inline int renderChirp(const ChirpSpec& spec, float sampleRateHz, float* out, int n) {
    if (!out || n <= 0 || sampleRateHz <= 0.0f) return 0;
    const double twoPi = 2.0 * M_PI;
    double phase = 0.0;
    for (int i = 0; i < n; ++i) {
        const double frac   = (n > 1) ? static_cast<double>(i) / (n - 1) : 0.0;
        const double instHz = spec.startHz + (spec.endHz - spec.startHz) * frac;
        phase += twoPi * instHz / sampleRateHz;
        const double env = 0.5 * (1.0 - std::cos(twoPi * frac));  // Hann (zero at both ends)
        out[i] = static_cast<float>(spec.amplitude * env * std::sin(phase));
    }
    return n;
}

inline int chirpSampleCount(const ChirpSpec& spec, float sampleRateHz) {
    return static_cast<int>(spec.durationSec * sampleRateHz);
}
