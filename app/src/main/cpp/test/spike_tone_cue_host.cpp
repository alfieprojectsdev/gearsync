// ADR 006 throwaway spike — proves the audio shift-cue is mic-safe by FREQUENCY,
// off-device. Synthesizes each chirp, FFTs it with the project-owned primitive,
// and asserts the spectral peak sits in the 1.5–3 kHz cue band while energy inside
// the 20–250 Hz engine analysis band (findDominantHz) is negligible — so the cue
// cannot inject a false engine peak even if the mic picks it up.
//
// This is a design-validation spike, not a shipped test. The remaining interference
// risks (output↔input resource contention, real acoustic pickup, xruns) are the
// device gate (ADR 006 M0) and cannot be proven on the host.
//
// Build: g++ -std=c++17 -I.. spike_tone_cue_host.cpp -o spike_tone_cue
// Run:   ./spike_tone_cue

#include "../DspPrimitives.h"
#include "../ToneCue.h"

#include <cassert>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

static void analyze(const ChirpSpec& spec, const char* name) {
    const float fs = 48000.0f;
    const int   N  = 4096;  // power-of-two FFT window

    std::vector<float> pcm(N);
    renderChirp(spec, fs, pcm.data(), N);

    std::vector<std::complex<float>> buf(N);
    for (int i = 0; i < N; ++i) buf[i] = { pcm[i], 0.0f };
    fft_inplace(buf);

    float  peakMag = 0.0f;
    int    peakBin = 0;
    double engineBandEnergy = 0.0;
    double totalEnergy = 0.0;
    for (int b = 1; b < N / 2; ++b) {
        const float mag = std::abs(buf[b]);
        const float hz  = static_cast<float>(b) * fs / N;
        const double e  = static_cast<double>(mag) * mag;
        totalEnergy += e;
        if (hz >= 20.0f && hz <= 250.0f) engineBandEnergy += e;
        if (mag > peakMag) { peakMag = mag; peakBin = b; }
    }

    const float peakHz   = static_cast<float>(peakBin) * fs / N;
    const float bandFrac = totalEnergy > 0.0 ? static_cast<float>(engineBandEnergy / totalEnergy) : 0.0f;

    assert(peakHz >= 1400.0f && peakHz <= 3000.0f);   // cue sits in 1.5–3 kHz
    assert(bandFrac < 0.001f);                         // <0.1% energy in 20–250 Hz
    std::printf("PASS %s: peakHz=%.0f  engineBandEnergy=%.4f%%\n", name, peakHz, bandFrac * 100.0f);
}

int main() {
    analyze(upshiftCue(),   "upshift (ascending)");
    analyze(downshiftCue(), "downshift (descending)");
    std::printf("All tone-cue spike checks passed — cues are out of the 20-250 Hz engine band.\n");
    return 0;
}
