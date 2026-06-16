# GearSync

GearSync is a high-performance, local-first native Android application designed to assist manual transmission drivers. By analyzing real-time acoustic signatures and chassis vibrations, GearSync dynamically identifies optimal shifting thresholds and provides low-latency **visual** feedback—completely independent of an OBD-II interface or cloud connectivity.

> **Note (v2 — visual-only by default):** The hardware-accelerated VU meter is the canonical channel — it conveys current gear, optimal-shift zone, and shift events with no audio required. The v1 procedural blip was removed because it used a second *Exclusive/low-latency* Oboe output that competed with the microphone-capture DSP for the fast-mixer resource. **Audio is now reintroduced safely as an opt-in (ADR 006, `useAudioCues`, default off):** non-verbal out-of-band chirps (a 1.5–2.9 kHz sweep, far above the 20–250 Hz engine FFT band) on a **Shared/normal-latency** AudioTrack that never claims the mic's exclusive resource — ascending = upshift, descending = downshift. Default builds remain audio-free. See [Shift Decision Logic](#shift-decision-logic) and [Current Limitations](#current-limitations).

The core digital signal processing (DSP) pipeline and machine learning calibration engines are implemented in native C++ via the Android NDK to bypass virtual machine overhead and ensure sub-millisecond execution.

---

## Features

* **Acoustic Tachometer with Opt-In Vibration Fusion:** Microphone PCM is the primary, default source. An opt-in (default-off) raw-accelerometer path adds a fully-implemented vibration estimate — timestamped SPSC ring → resampled FFT → autocorrelation harmonic guard (rejects aliased 2×/3× chassis harmonics) → mic-primary fusion policy that only refines or, when the mic is weak and vibration clearly stronger, replaces the acoustic estimate. Rate-gated; degrades to mic-only when the device can't sustain the sample rate.
* **Glanceable Visual Interface:** A hardware-accelerated, custom VU meter UI conveying current gear, the optimal-shift zone, calibration confidence, and shift events — with a bright needle-edge and an amber upshift-target marker for peripheral readability. A hidden **demo mode** (triple-tap the upper-right corner) animates it with synthetic data, no car needed.
* **Opt-In Audio Shift Cues (ADR 006):** Default-off non-verbal out-of-band chirps — ascending = upshift, descending = downshift — for eyes-on-road feedback without interfering with the mic.
* **Edge ML Automated Calibration:** 1-D K-Means clustering seeded from a configurable per-vehicle profile, refined online; optional guided per-gear RANSAC capture for tighter centroids.
* **Configurable Vehicle Profile:** A JSON asset (`assets/vehicle_config.json`) defines transmission ratios, final drive, tire circumference, tolerance bands, and the opt-in flags — ships pre-tuned for the **Toyota Wigo 1.0 E M/T**, editable for any vehicle without recompiling.
* **Fragmented Session Stitching:** State persistence via Welford's Online Algorithm to complete calibration across multiple, non-contiguous driving sessions.

---

## Architecture Overview

GearSync leverages a clear separation of concerns between high-level Android operating system interactions and low-level mathematical processing threads:

```
[Android System]
       │ (1 Hz GPS Speed Updates)
       ▼
[JNI Bridge Surface]
       │
       ▼
[Native C++ Core] ◄──── [Oboe Input Stream] (Raw Audio PCM, mic — Exclusive low-latency)
       │          ◄──── [ASensorManager] (Raw Accelerometer, fastest gated rate)
       ├─► [Mic FFT] + [Accel resample→FFT→harmonic guard] ─► [Mic-primary Fusion Policy]
       ├─► [Edge ML Calibration Engine]       (Welford + K-Means)
       └─► [Shift Decision Logic] ──► [VU Meter UI] (60 FPS Canvas)
                                          └─► [Audio Cue Player] (opt-in, Shared/normal-latency)
```

> The DSP runs on a dedicated worker thread fed by a **lock-free single-producer/single-consumer (SPSC) snapshot handoff** from the real-time audio input callback. The audio callback is wait-free (no locks, no allocation), and the **microphone input owns the low-latency (Exclusive) audio resource**. The optional ADR 006 audio cues play on a separate **Shared/normal-latency** output that never contends for that fast path; with cues off (default) there is no output stream at all.

### Module Responsibilities

* **Kotlin Layer (`/app/src/main/java`):** Handles lifecycle management, maps the UI Canvas pipeline, hosts the structural Foreground Service to prevent background execution suspension, and feeds GPS metrics downstream.
* **Native NDK Layer (`/app/src/main/cpp`):** Manages lock-free ring buffers, implements real-time Fourier transforms, calculates running statistical variances, classifies the current gear, and derives the shift recommendation.

---

## Shift Decision Logic

This is the heart of the system: how GearSync "understands" whether to recommend an **upshift**, a **downshift**, or **holding** the current gear. The logic is purely inferential — there is no OBD-II tachometer feed. Everything is reconstructed from two cheap signals: the microphone and the GPS.

### Step 1 — Reconstruct engine frequency from sound

The audio input stream captures mono PCM at 48 kHz. Every time `FFT_SIZE` (4096) fresh samples are available, the input callback hands a windowed snapshot to the DSP worker thread. The worker applies a **Hamming window** and a radix-2 Cooley–Tukey **FFT**, then searches the **20–250 Hz** band (the plausible firing-frequency range for a passenger engine) for the dominant spectral peak. That peak frequency `f` is the engine's fundamental firing frequency — a proxy for RPM:

$$\text{RPM} = \frac{120 \cdot f}{P}$$

where `P` is the engine's cylinder count (3 for the Wigo's 1KR-FE).

### Step 2 — Compute the gear ratio observable

When the clutch is fully engaged, engine frequency and road speed are rigidly proportional within a single gear:

$$f = k_g \cdot v \quad\Longrightarrow\quad k_g = \frac{f}{v}$$

The worker reads the latest GPS speed `v` (m/s, 1 Hz). If `v ≥ MIN_SPEED_MPS` (1 m/s, to reject GPS jitter at standstill), it computes the instantaneous ratio `r = f / v`. **This ratio `r` is the single observable that uniquely identifies which gear the car is in** — it is (nearly) constant within a gear and steps to a different value after every shift.

### Step 3 — Know the gears (seeded, then learned)

Each gear `g` has a theoretical slope derived directly from the vehicle profile:

$$k_g = \frac{\text{transmissionRatio}_g \times \text{finalDrive} \times \text{firingFactor}}{\text{tireCircumference}}, \qquad \text{firingFactor} = \frac{\text{cylinders}}{\text{strokeCycle}/2}$$

These theoretical `k_g` values are loaded from `assets/vehicle_config.json` at startup and **seed** the K-Means centroids, so the app classifies gears correctly from the very first drive. As real `(f, v)` observations accumulate, **1-D K-Means** (5 centroids, farthest-first init) refines those centroids to match the *actual* car — absorbing tire wear, load, and pressure drift over time.

### Step 4 — Classify the current gear (with a tolerance gate)

`classifyGear(r)` finds the nearest centroid to the live ratio `r`. Critically, it then applies an **asymmetric tolerance band** before accepting the match:

$$\text{accept gear } g \iff k_g \cdot \text{tolLow} \;\le\; r \;\le\; k_g \cdot \text{tolHigh}$$

For the Wigo this band is **[0.98, 1.025]** (−2% / +2.5%). The asymmetry is deliberate — it accommodates the real-world physics of this specific lightweight hatchback:

| Effect | Direction | Magnitude |
|---|---|---|
| Tire wear (8 mm → 1.6 mm tread) | `k_g` drifts **up** | up to **+2.2%** |
| Under-inflation (32 → 24 PSI) | `k_g` shifts | −0.5% to −1.5% |
| Passenger/cargo load (+300 kg) | flattens rolling radius, `k_g` **up** | small |
| Centrifugal tire expansion (highway) | `k_g` **down** | fraction of a percent |

If `r` falls **outside** the band for every gear (e.g., mid-shift, clutch slip, or a bad FFT peak), classification returns **−1 / unknown** rather than snapping to a spurious gear. This is what keeps the display from flickering between gears during a shift.

### Step 5 — Derive the shift recommendation

Once the gear is known, the worker maps `r` to a **needle position in [0, 1]** *within that gear's band*:

$$\text{needle} = \mathrm{clamp}\!\left(\frac{r - k_{g+1}}{k_g - k_{g+1}},\; 0,\; 1\right)$$

Because gears are sorted descending (`k_1 > k_2 > … > k_5`), a *higher* `r` means the engine is revving high relative to where the next gear would put it. The VU meter renders this as three zones:

| Needle zone | Meaning | Recommendation |
|---|---|---|
| **0–33%** (blue, "lugging") | RPM low for this gear | **Downshift** — engine is below its efficient band |
| **33–66%** (green, "optimal") | RPM in the sweet spot | **Hold** the current gear |
| **66–100%** (red, "redline") | RPM high, near the next gear's entry point | **Upshift** — you're over-revving |

In short: **lugging → downshift, optimal → hold, redline → upshift.** The needle drifts smoothly (exponential moving average, α = 0.18) so it never jitters, and resets to the lug end when the gear is unknown.

### Step 6 — Quality gating (when *not* to learn)

Two guards prevent the model from being poisoned by noisy data:

1. **GPS stability window.** GPS updates at 1 Hz while the acoustic pipeline runs at ~12 Hz, so during acceleration/braking the two signals are temporally misaligned and `r` is meaningless. The engine only feeds a ratio into Welford/K-Means after GPS speed has been **stable** (Δ ≤ `speedJitterThresholdMps`, default 0.5 m/s) for `steadyStateWindowSeconds` consecutive samples (default **4 s** of steady-state cruising).
2. **Welford confidence.** The running variance `σ² = M₂/n` is exposed as a confidence score `1/(1+σ²)`; the VU meter dims its zones until confidence rises, signalling that calibration is still converging.

### Why a tolerance *band* instead of exact slopes

Treating `k_g` as a rigid constant fails in practice because the rolling circumference is not fixed — it shrinks ~2.2% over a tire's life, sags 0.5–1.5% with low pressure, flattens further under load, and expands slightly at highway speed. The asymmetric band (`−2% / +2.5%`) and the 4-second steady-state lock window are sized to swallow all of these compounding drifts without ever mistaking one gear for its neighbour. All four numbers live in `vehicle_config.json` and can be widened or tightened per vehicle.

---

## Mathematical Foundations

When the vehicle's clutch is fully engaged, the relationship between the engine's fundamental frequency ($f$) and the actual vehicle speed ($v$) is strictly linear for each individual gear ($g$):

$$f = k_g \cdot v$$

Where $k_g$ represents a constant mechanical slope unique to that gear factor.

### 1. Engine RPM Estimation

For a standard four-stroke internal combustion engine, the operational revolutions per minute (RPM) is evaluated from the dominant spectral peak frequency ($f$) via:

$$\text{RPM} = \frac{120 \cdot f}{P}$$

Where $P$ is the explicit cylinder count of the engine block.

### 2. State Optimization (Welford's Algorithm)

To stitch fragmented calibration runs without logging thousands of raw historical coordinates, the application updates its structural confidence boundaries using Welford's method for calculating running variances:

$$\bar{x}_n = \bar{x}_{n-1} + \frac{x_n - \bar{x}_{n-1}}{n}$$

$$M_{2,n} = M_{2,n-1} + (x_n - \bar{x}_{n-1})(x_n - \bar{x}_n)$$

The tracking variance $\sigma^2 = \frac{M_{2,n}}{n}$ serves as the direct validation cutoff threshold. When variance drops below a set stability value and structural volume is achieved, the gear ratio constant is locked.

---

## Getting Started

### Prerequisites

* Android Studio Hedgehog (2023.3.1) or newer
* Android NDK (Version 26.x or newer)
* CMake 3.22.1+
* A physical Android device running API Level 26 (Android 8.0) or higher (Sensors and mic access are limited within virtual emulators).

### Installation & Build

1. Clone the repository down to your local developer workspace:
```bash
git clone https://github.com/your-repo/GearSync.git
cd GearSync

```


2. Open the project in Android Studio. The IDE will automatically sync the build files and recognize the native configuration paths designated inside `CMakeLists.txt`.
3. Ensure your local configuration points directly to your installed NDK path within your local properties file:
```properties
ndk.dir=/path/to/android-sdk/ndk/26.x.x

```


4. Compile and deploy the debug APK target onto your hardware testing device.

---

## Configuration & Calibration

GearSync ships **pre-configured for the Toyota Wigo 1.0 E M/T** and requires no manual per-gear calibration to start working — the theoretical `k_g` seeds get it classifying gears on the first drive, and K-Means refines them automatically as you cruise.

### Adapting to another vehicle

All vehicle-specific parameters live in `app/src/main/assets/vehicle_config.json`. No code changes or recompilation of the native layer are needed to retune them:

```json
{
  "engine":        { "cylinders": 3, "strokeCycle": 4 },
  "transmission":  { "gears": 5, "ratios": [3.545, 1.904, 1.233, 0.906, 0.738], "finalDriveRatio": 4.312 },
  "tire":          { "spec": "155/80 R13", "nominalCircumferenceMeters": 1.816 },
  "calibration":   {
    "ratioToleranceLow": 0.98, "ratioToleranceHigh": 1.025,
    "steadyStateWindowSeconds": 4, "speedJitterThresholdMps": 0.5,
    "useVibrationFusion": false, "useAudioCues": false
  }
}
```

| Field | Effect on the algorithm |
|---|---|
| `cylinders` / `strokeCycle` | Sets the firing factor that converts firing frequency ↔ RPM |
| `ratios` / `finalDriveRatio` | Seed the theoretical `k_g` centroids (one per gear) |
| `nominalCircumferenceMeters` | Scales `k_g`; the dominant source of real-world drift |
| `ratioToleranceLow/High` | Width of the accept band around each centroid |
| `steadyStateWindowSeconds` | Seconds of stable cruising before a ratio is learned |
| `speedJitterThresholdMps` | How much GPS speed may vary and still count as "stable" |
| `useVibrationFusion` | ADR 004 opt-in flag. Defaults false; mic-only remains the primary/default path |
| `useAudioCues` | ADR 006 opt-in flag. Defaults false; when true, out-of-band shift chirps play on the media stream |

On startup `VehicleConfig.kt` loads this JSON, computes the `k_g` seeds, and pushes them plus `useVibrationFusion` to the native engine via `NativeEngine.setVehicleConfig(...)`. If the file is missing or malformed, the engine falls back to open tolerances and learns purely from K-Means. Even when vibration fusion is requested, native diagnostics keep it rate-gated and mic-primary.

---

## Current Limitations

The shift-decision approach is deliberately lightweight (mic + GPS only, no OBD-II). That buys broad compatibility at the cost of several known constraints:

* **GPS speed is the weakest link.** Consumer GPS updates at ~1 Hz with latency and accuracy that degrade in tunnels, urban canyons, and parking structures. Because the gear observable is `f / v`, any error in `v` propagates directly into the ratio. The 4-second steady-state lock mitigates but does not eliminate this.
* **Temporal misalignment during transients.** Acoustic frames arrive ~12×/s but GPS only 1×/s. During hard acceleration or braking the two are out of sync, so ratios are only trusted during steady cruising. A raw-NMEA Doppler-speed path was investigated (ADR 007) but **rejected** (Galaxy A07/A56 cap NMEA at 1 Hz). **ADR 008** mitigates the *visible* symptom — gear-display hysteresis (3 consecutive consistent classifications to commit) plus a freeze when accelerometer magnitude is high — stopping the flicker without improving underlying accuracy. Inertial dead-reckoning was rejected (gravity on a tilted mount swamps real motion); OBD-II (ADR 005) remains the real speed-accuracy lever.
* **Single-peak FFT on the mic path.** `findDominantHz` takes the single loudest bin in the 20–250 Hz band, so road noise, exhaust resonance, HVAC, music, or talking can momentarily outweigh the true firing peak. The opt-in vibration path now has an autocorrelation harmonic guard (ADR 004 M5) against aliased 2×/3× harmonics, but the **mic path itself** still has no harmonic-product-spectrum or peak-tracking confirmation.
* **No explicit shift-event detection in the model.** The accelerometer spike only drives a visual flash; it is *not* fed into the classifier to anticipate a gear change. Gear changes are inferred purely from the ratio jumping bands after the fact.
* **Clutch / neutral states are invisible.** When the clutch is depressed (or in neutral), `f` and `v` decouple and the `f = k_g·v` model is invalid. The tolerance gate rejects these as "unknown," but the system does not affirmatively detect a disengaged clutch.
* **Frequency resolution is coarse at idle.** A 4096-point FFT at 48 kHz yields ~11.7 Hz/bin. Near the 20 Hz low end, a one-bin error is a large fractional RPM error, so low-idle gears are the noisiest to classify.
* **K-Means can converge degenerately.** With only 5 centroids over a narrow 1-D range, an unlucky data distribution (e.g., mostly highway driving in 5th) can collapse centroids; there is no per-cluster occupancy floor enforced yet.
* **Single fixed sample rate.** The pipeline assumes 48 kHz; devices that negotiate a different rate are not yet handled, and aliasing/scaling math is hardcoded to that assumption.

## Areas for Improvement

* **Sensor fusion for speed.** Higher-rate NMEA Doppler speed was tried and rejected (ADR 007 — 1 Hz hardware cap on the target phones). The rate-independent lever still open: time-align each GPS `v` to the engine `f` from its actual fix instant (a monotonic `(f, tNs)` ring) to cut the transient smear even at 1 Hz.
* **Robust pitch detection on the mic path.** The vibration path already has an autocorrelation harmonic guard (ADR 004 M5); extend a Harmonic Product Spectrum / YIN-style estimator + frame-to-frame peak tracking to the **mic** `findDominantHz` to reject transient noise and lock the true fundamental.
* **Predictive shift timing.** Feed the accelerometer and the needle's velocity (dNeedle/dt) into a small state machine so the UI can recommend *"upshift now"* slightly ahead of redline rather than reporting it after the fact.
* **Explicit clutch/neutral detection.** Use the sudden decoupling of `f` and `v` (frequency rises while speed holds, or vice-versa) to detect disengagement and freeze classification cleanly.
* **Adaptive tolerance.** Let the accept band tighten automatically as Welford confidence grows, instead of using static `tolLow`/`tolHigh` for the whole vehicle lifecycle.
* **Per-cluster validation.** Add an occupancy floor and silhouette/separation check to K-Means so degenerate centroid collapse is detected and the affected gear falls back to its theoretical seed.
* **Automatic gain control on input.** Normalize mic levels across devices and cabin environments to stabilize the FFT peak magnitude.
* **Settings UI for the vehicle profile.** Surface `vehicle_config.json` fields in-app (currently edit-the-asset) with validation, presets per make/model, and a live preview of the seeded `k_g` values.

---

## Contributing

Contributions are welcome — the permissive MIT license below is meant to let anyone
fork, improve, and ship changes. Workflow: branch off `main`, keep the opt-in defaults
off (`useVibrationFusion`, `useAudioCues`), run the host tests before opening a PR
(`./gradlew testDebugUnitTest` for Kotlin, the C++ `test/*_host.cpp` for DSP), and open
a PR. Architecture decisions are recorded in `adr.md`; milestone plans live in `plans/`.

## License

This project is licensed under the **MIT License** — see the [`LICENSE`](LICENSE) file
for the full text.

### Third-party components

* **Google Oboe** (audio I/O) — Apache License 2.0; consumed as a prefab AAR, MIT-compatible.
* **phyphox** — referenced as *prior-art inspiration only* for the vibration-DSP approach
  (ADR 004). phyphox is GPLv3; **no phyphox code, tables, or implementation structure are
  copied** into this project, so its copyleft does not extend here.
