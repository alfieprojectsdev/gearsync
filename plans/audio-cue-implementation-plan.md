# ADR 006 — Optional Out-of-Band Audio Shift Cues — Implementation Plan

Implements ADR 006 (`adr.md`). Optional, **default-off** non-verbal audio cues that survive the
two interference problems that killed v1 audio. Visual-only (VU meter) stays the default and the
only required path.

> **Hard rule:** with `useAudioCues` off, the app is byte-for-byte the current build — no output
> stream opened, no assets loaded. Cues never touch the mic-capture low-latency path.

## Key decisions

| ID | Decision | Rationale |
|----|----------|-----------|
| C-AUD-1 | **Milestone 0 is an on-device interference probe, and all later work is gated on it.** | The whole risk is mic contamination + glitches. Prove on a real device that a Shared-output 1.5–3 kHz cue plays *while the mic DSP runs* with no measurable energy added to the 20–250 Hz band and no input-stream xrun. Mirrors ADR 004 M0. |
| C-AUD-2 | Default-off behind `vehicle_config.json` `useAudioCues=false`. | Visual-only remains the v2 default; audio is additive opt-in. |
| C-AUD-3 | **Shared, normal-latency output only** (`SoundPool`/`AudioTrack` or Oboe Shared) — never Exclusive/LowLatency. | The v1 failure was an Exclusive output fighting the Exclusive mic input for the fast-mixer path. Cues tolerate ~50 ms jitter, so they don't need the low-latency resource. |
| C-AUD-4 | **Cue tones live at 1.5–3 kHz, far above the 20–250 Hz engine band.** | `findDominantHz` only scans 20–250 Hz, so an out-of-band tone cannot create a false engine peak even when the mic hears it. Frequency separation = mic safety by construction. |
| DL-AUD-1 | **Pitch-direction = shift-direction.** Ascending = upshift, descending = downshift, silence in the optimal zone. | Pre-cognitive mapping (rising=up/more, falling=down/less); non-verbal, eyes-free (POI "abbreviate audio prompts"). |
| DL-AUD-2 | Fire once per zone transition, ~1–2 s cooldown; never continuous. | A nagging cue is worse than none; only signal when an action changes. |
| DL-AUD-3 | Triggered Kotlin-side off the same VU zone state, on its own thread — not from the native realtime DSP. | Keeps the audio-callback/DSP hot paths untouched (ADR 001). |

## Constraints

- C-AUD-1 device gate satisfied before any cue-trigger logic is reviewed/merged.
- The tone synthesis (waveform/chirp generation) is **pure and host-testable**: generate the PCM,
  FFT it (reuse `DspPrimitives.h`), assert the spectral peak sits in 1.5–3 kHz and energy in
  20–250 Hz is negligible. (See the spike, below.)
- With the flag off: no output stream, no assets, no behaviour change.

---

### Milestone 0: On-device audio↔mic interference probe — **HARD GATE**

**Files**: throwaway spike (debug activity / instrumented), not merged as-is. A device companion to the
host spike `app/src/main/cpp/test/spike_tone_cue_host.cpp` (which already proves the *frequency* placement off-device).

**Requirements**:
- While the mic-capture DSP runs, emit a 1.5–3 kHz Shared-output chirp every ~1 s.
- Log `findDominantHz` and the 20–250 Hz band energy with cue ON vs OFF; log input-stream xrun count.

**Acceptance**:
- No measurable rise in 20–250 Hz band energy / no false peak while the cue plays.
- No mic input xruns attributable to the output stream.
- Go/no-go: if either fails, revisit (move further out of band, lower level, or A2DP-only).

**Tests**: `type:device/manual || normal: cue plays, engine-band FFT unaffected || edge: loud cabin + cue → still no false peak || error: output device absent → cues silently disabled, app unaffected`

### Milestone 1: Config flag + cue trigger state machine (silent)

**Files**: `assets/vehicle_config.json`, `VehicleConfig.kt`, `ShiftAssistantService.kt`.

**Requirements**:
- Add `useAudioCues` (default **false**); load + plumb (Kotlin-only).
- Zone-transition detector off the VU state (lug ↔ optimal ↔ redline) with per-transition debounce +
  cooldown. Emits cue *intents* (UPSHIFT / DOWNSHIFT / NONE) — no audio yet.

**Acceptance**: flag off → detector never runs; flag on → correct intents on synthetic zone sequences (unit-testable). `assembleDebug` passes.

**Tests**: `type:unit || normal: green→red yields one UPSHIFT then silence || edge: rapid flicker debounced to one cue || error: unknown gear → NONE`

### Milestone 2: Shared-output tone player

**Files**: new `audio/CuePlayer.kt` (+ `ToneCue.h`-equivalent synth or pre-rendered assets).

**Requirements**:
- Render the ascending/descending chirps (1500→2200 / 1500→900 Hz, ~120 ms, raised-cosine envelope to
  avoid clicks) — either synthesized once at startup or shipped as small WAV assets.
- Play via **Shared/normal-latency** `SoundPool`/`AudioTrack`; off the main + realtime threads; respect
  the M1 cooldown. Tear down cleanly on service stop.

**Acceptance**: with fusion/mic running, cues play without audio glitches; with flag off, no stream/assets. Verified against the M0 gate result.

**Tests**: `type:manual/device || normal: upshift→ascending audible, downshift→descending || edge: cue during active mic capture → no xrun || error: audio focus lost → cue skipped, no crash`

### Milestone 3: Tuning, escalation, optional A2DP

**Files**: `audio/CuePlayer.kt`, config.

**Requirements**:
- Tune frequency/level/duration against M0 data and real drives; optional escalating double-chirp when
  held in redline; optional preference to route to Bluetooth/A2DP (mic never hears it).
- Keep visual as the canonical channel (accessibility); audio strictly additive.

**Acceptance**: cues are clear over cabin/engine noise at a comfortable level; no engine-band contamination; visual unchanged.

**Tests**: `type:manual/device || normal: cues clear at highway noise || edge: A2DP connected → routed there || error: hearing-impaired path → visual still complete`

### Milestone 4: Docs + tenet reconciliation

**Files**: `README.md`, `CLAUDE.md`, `adr.md`.

**Requirements**: update the v2 "no audio output / visual-only" tenet to "visual-only by default;
optional out-of-band audio cues"; promote ADR 006 from Proposed; document the cue language + safety design.

**Acceptance**: docs coherent; no "no audio output" contradiction with the shipped opt-in.

**Tests**: `type:docs || normal: README/CLAUDE/adr agree on optional-audio scope`

---

## Sequencing & ownership

- All Claude. Each milestone: branch off `main`, host/JVM tests, PR, CodeRabbit, append `session-notes.md`. M0 is a throwaway spike, not a merged PR.
- **Device-free now:** the host tone-synth spike (frequency-placement proof), M1 trigger state machine, M2 synth rendering logic. **Device-gated:** M0 interference probe, and trusting/merging M2/M3 playback (needs a real device to confirm no mic contamination / xruns).
- Lower priority than ADR 004 M6 (the actual drive). Natural to validate M0 in the same on-car session as M6.
