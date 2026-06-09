# Design Note — Accelerometer-FFT Vibration Sensing & Sensor Fusion

**Status:** Proposed (design only — not yet implemented)
**Date:** 2026-06-09
**Relates to:** ADR 001 (native DSP core / hand-rolled FFT), ADR 002 (local-first edge ML), README "Shift Decision Logic", `session-notes.md` open items
**Prior art reviewed:** `research/phyphox-android` (`Analysis.java`) — **GPLv3, ideas only, no code copied** (see §6)

---

## ADR 004 (proposed): Accelerometer-FFT as a second engine-frequency source, fused with the acoustic estimate

* **Context:** Engine frequency is currently inferred **only** from the microphone FFT (`findDominantHz`, 20–250 Hz band). This is acoustically fragile — wind, music, passengers, road and exhaust noise all corrupt the dominant peak, and at low RPM the engine is acoustically quiet. The accelerometer is already polled at 100 Hz but used **only** as a magnitude-threshold shift-spike trigger for the VU flash (`SHIFT_ACCEL_THRESH`); its vibration signal carries the *same* engine firing frequency the mic hears, and is immune to acoustic noise.
* **Decision:** Add a **second DSP path** — accelerometer magnitude → resample to a uniform time grid → Hamming-windowed FFT (reusing the existing `fft_inplace`) → dominant vibration frequency `f_vib` — and **fuse** it with the acoustic `f_acoustic` into a confidence-weighted engine-frequency estimate that feeds the existing `r = f/v → classifyGear` pipeline unchanged. The mic stays primary and default; fusion is opt-in and degrades gracefully when the accelerometer can't sample fast enough or the mount isn't coupled.
* **Consequences:** Robustness in acoustic noise and better low-RPM (lugging) detection, at the cost of a higher accelerometer sample rate (device-dependent), timestamp resampling, and a fusion policy. No new heavy dependencies (own radix-2 FFT per ADR 001). Phone-to-chassis mount coupling becomes a signal-quality factor.

---

## 1. Motivation

The acoustic path is a single point of failure. `f = k_g · v` only holds when `f` is a trustworthy engine-frequency reading; the mic peak degrades exactly when the cabin is noisy (highway wind, audio, conversation) and when the engine is quiet (lugging, low RPM). A second, **independent** estimate of the same physical quantity — derived from a different transducer with uncorrelated noise — is the textbook fix: sensor fusion raises confidence when the two agree and flags ambiguity when they don't.

The accelerometer is already running; today it throws away everything except a 4 m/s² magnitude spike.

## 2. Physics & the Nyquist budget

For a 4-stroke engine the firing (fundamental vibration) frequency is:

```text
f_fire = RPM/60 · (cylinders / 2)
```

For the Wigo (3-cyl 1KR-FE): `f_fire = RPM · 0.025`. This is the **same** quantity the mic path recovers (`RPM = 120·f/P`, `P = 3` ⇒ `f = RPM · 0.025`), so `f_vib` and `f_acoustic` are directly comparable — fusion is averaging two estimates of one frequency, not reconciling two different signals.

| RPM | f_fire (Hz) | Coverage @100 Hz accel (Nyquist 50) | @500 Hz (Nyquist 250) |
|---|---|---|---|
| 850 (idle) | ~21 | ✅ | ✅ |
| 2000 | 50 | ⚠️ at the limit | ✅ |
| 4000 | 100 | ❌ aliased | ✅ |
| 6000 (redline) | 150 | ❌ aliased | ✅ |

**The current 100 Hz accel rate (`SENSOR_US = 10000`) is insufficient** — it only covers up to ~2000 RPM before aliasing. Full range needs **≥ 300–400 Hz** (request `SENSOR_US ≈ 2000–2500`, or `SENSOR_DELAY_FASTEST`), but the achievable rate is **device-capped** and must be measured on target hardware. If a device caps at ~100 Hz, fusion is disabled and the mic path is unaffected.

Resolution/latency trade: at 500 Hz, ~1 Hz bins need N≈512 samples ≈ 1 s window — versus the mic's 85 ms. The accel path is therefore a **slow confidence corrector**, not the fast needle driver (see §4).

## 3. What phyphox does — and what we take (license-safe)

Reviewed `research/phyphox-android/.../Analysis.java`:

- **Their FFT (`fftAM`) applies no window** — raw radix-2 with zero-pad to a power of two. gearsync already applies a Hamming window in `findDominantHz`; **keep it** (better leakage control ⇒ a cleaner peak). Nothing to borrow at the primitive level — their FFT is the same radix-2 DIT family.
- **They also ship autocorrelation (`autocorrelationAM`) and a sliding-window periodicity (`periodicityAM`)** with min/max-period gating. *Idea worth adopting:* a vibration signal is harmonic-rich, and a bare FFT-magnitude peak can latch onto the 2nd/3rd harmonic. Cross-checking the FFT peak against an autocorrelation **fundamental period** disambiguates harmonics — pick the sub-harmonic when they disagree.
- **They do not resample for the FFT** — they assume ~uniform sensor delivery and use the nominal rate. This is the **one place gearsync should be more careful than phyphox**: Android accelerometer event timestamps jitter, so we resample onto a uniform grid before transforming (§4 step 2) to avoid frequency smearing.

**License:** phyphox-android is GPLv3 (copyleft). This note records the *approach*; no code is copied. gearsync reimplements with its own radix-2 FFT from ADR 001.

## 4. Proposed pipeline (mirrors the mic path)

1. **Capture.** Raise the sensor thread rate (measure the device cap). Each event: compute magnitude `a = |linear_acceleration|` (gravity already removed by `LINEAR_ACCELERATION`), push `(timestamp_ns, a)` into a lock-free SPSC ring — same pattern as the PCM ring.
2. **Resample.** In a new accel-DSP worker, pull a window and **linearly interpolate onto a uniform grid** at the measured nominal rate using the event timestamps (corrects sensor jitter).
3. **Transform.** Subtract DC (mean), apply Hamming, run `fft_inplace`, pick the peak in an engine band (~15–160 Hz) ⇒ `f_vib`. Record peak prominence (peak magnitude / median magnitude).
4. **Harmonic check (optional, phyphox-inspired).** Autocorrelate the windowed signal; if the autocorrelation fundamental disagrees with the FFT peak by ~2× or 3×, prefer the fundamental.
5. **Fuse.** Combine `f_acoustic` (fast, mic) and `f_vib` (slow, accel) into `f_fused`, weighted by each estimate's prominence and their mutual agreement (§5). Feed `r = f_fused / v` into the unchanged `classifyGear` / needle path.

The mic remains the **responsive** driver of the VU needle; the accel path acts as a confirmer/disambiguator that nudges confidence and resolves gear ambiguity, so its ~1 s latency is acceptable.

## 5. Fusion strategy

Let `p_a`, `p_v` be the prominence (confidence) of the acoustic and vibration peaks.

- **Agree** (`|f_a − f_v| ≤ ε`, e.g. ε = 3 % per the existing tolerance band): `f_fused = (p_a·f_a + p_v·f_v)/(p_a+p_v)`; boost the confidence fed to the VU meter / K-Means gating.
- **Disagree:** take the higher-prominence estimate; if both are weak, **fall back to the mic** (status quo). Never let a noisy accel reading override a strong acoustic one.
- **Accel unavailable / low rate:** fusion off, mic-only — identical to today.

This makes mount coupling self-correcting: a loosely-mounted phone yields low `p_v`, so fusion automatically down-weights vibration.

## 6. Risks & open questions

- **Device sample-rate cap (R, high):** achievable accel rate is unknown until measured; below ~300 Hz the high-RPM range aliases. Mitigation: probe `SENSOR_DELAY_FASTEST` at startup, disable fusion (not the app) when too slow.
- **Mount coupling (M):** weak/absent chassis contact ⇒ no usable vibration. Mitigated by prominence-weighted fusion.
- **Latency mismatch (M):** ~1 s accel window vs 85 ms mic — handled by role separation (accel = slow corrector).
- **Harmonic latch (M):** addressed by the autocorrelation cross-check.
- **Battery (L):** higher accel polling draws more power; modest but non-zero.
- **Config surface:** add a `vehicle_config.json` / runtime flag (e.g. `useVibrationFusion`) to gate it. (Note: the guided-calibration work was constrained to *no new config fields*; that constraint was scoped to that feature and does not bind this ADR.)

## 7. Validation plan

- **Device probe:** log the achievable `LINEAR_ACCELERATION` rate per device.
- **Correctness:** compare `f_vib` vs `f_acoustic` (and a known RPM reference where available) across idle → redline; verify they track `f_fire = RPM·0.025`.
- **Benefit:** A/B `classifyGear` confidence with/without fusion in deliberately noisy acoustic conditions (windows down, music on) and at low RPM, where the mic is weakest.
- **Graceful degradation:** confirm a rate-capped device silently falls back to mic-only.
