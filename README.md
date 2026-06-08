# GearSync

GearSync is a high-performance, local-first native Android application designed to assist manual transmission drivers. By analyzing real-time acoustic signatures and chassis vibrations, GearSync dynamically identifies optimal shifting thresholds and provides low-latency **visual** feedback—completely independent of an OBD-II interface or cloud connectivity.

> **Note (v2):** The procedural audio cue output has been deprecated. Earlier builds synthesized a high-frequency blip through a second Oboe output stream, which competed with the microphone-capture DSP pipeline for the low-latency audio path. The system is now **visual-only**: a hardware-accelerated VU meter conveys current gear, optimal-shift zone, and shift events without any audio output. See [Shift Decision Logic](#shift-decision-logic) and [Current Limitations](#current-limitations).

The core digital signal processing (DSP) pipeline and machine learning calibration engines are implemented in native C++ via the Android NDK to bypass virtual machine overhead and ensure sub-millisecond execution.

---

## Features

* **Acoustic & Vibration Tachometer:** Utilizes hardware microphone PCM data and high-frequency linear accelerometer tracking to extract engine firing frequencies without modifying vehicle electronics.
* **Glanceable Visual Interface:** A hardware-accelerated, custom VU meter UI that conveys current gear, the optimal-shift zone, calibration confidence, and shift events with zero audio distraction.
* **Edge ML Automated Calibration:** Integrates 1-D K-Means clustering seeded from a configurable per-vehicle profile to adapt to any vehicle's gear ratios and mechanical signature.
* **Configurable Vehicle Profile:** A JSON asset (`assets/vehicle_config.json`) defines transmission ratios, final drive, tire circumference, and tolerance bands — ships pre-tuned for the **Toyota Wigo 1.0 E M/T** but editable for any vehicle without recompiling.
* **Fragmented Session Stitching:** Built-in state persistence utilizing Welford's Online Algorithm to allow drivers to complete calibration across multiple, non-contiguous driving sessions.

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
[Native C++ Core] ◄──── [Oboe Input Stream] (Raw Audio PCM, mic)
       │          ◄──── [ASensorManager] (Linear Acceleration, 100 Hz)
       ├─► [FFT / Spectral Analysis Layer]   (DSP worker thread)
       ├─► [Edge ML Calibration Engine]       (Welford + K-Means)
       └─► [Shift Decision Logic] ──► [VU Meter UI] (60 FPS Canvas)
```

> The DSP runs on a dedicated worker thread fed by a **lock-free single-producer/single-consumer (SPSC) snapshot handoff** from the real-time audio input callback. The audio callback itself is wait-free (no locks, no allocation), and there is **no audio output stream** — the microphone path owns the low-latency audio resource exclusively.

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
2. **Welford confidence.** The running variance `σ² = M₂/(n−1)` is exposed as a confidence score `1/(1+σ²)`; the VU meter dims its zones until confidence rises, signalling that calibration is still converging.

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
    "steadyStateWindowSeconds": 4, "speedJitterThresholdMps": 0.5
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

On startup `VehicleConfig.kt` loads this JSON, computes the `k_g` seeds, and pushes them to the native engine via `NativeEngine.setVehicleConfig(...)`. If the file is missing or malformed, the engine falls back to open tolerances and learns purely from K-Means.

---

## Current Limitations

The shift-decision approach is deliberately lightweight (mic + GPS only, no OBD-II). That buys broad compatibility at the cost of several known constraints:

* **GPS speed is the weakest link.** Consumer GPS updates at ~1 Hz with latency and accuracy that degrade in tunnels, urban canyons, and parking structures. Because the gear observable is `f / v`, any error in `v` propagates directly into the ratio. The 4-second steady-state lock mitigates but does not eliminate this.
* **Temporal misalignment during transients.** Acoustic frames arrive ~12×/s but GPS only 1×/s. During hard acceleration or braking the two are out of sync, so ratios are only trusted during steady cruising — meaning the system effectively *cannot* recommend a shift in the middle of aggressive driving, which is exactly when shift timing matters most.
* **Single-peak FFT is fragile.** `findDominantHz` takes the single loudest bin in the 20–250 Hz band. Road noise, exhaust resonance, HVAC fans, music, or a passenger talking can momentarily outweigh the true firing peak, producing a spurious ratio. There is currently no harmonic-product-spectrum or peak-tracking confirmation.
* **No explicit shift-event detection in the model.** The accelerometer spike only drives a visual flash; it is *not* fed into the classifier to anticipate a gear change. Gear changes are inferred purely from the ratio jumping bands after the fact.
* **Clutch / neutral states are invisible.** When the clutch is depressed (or in neutral), `f` and `v` decouple and the `f = k_g·v` model is invalid. The tolerance gate rejects these as "unknown," but the system does not affirmatively detect a disengaged clutch.
* **Frequency resolution is coarse at idle.** A 4096-point FFT at 48 kHz yields ~11.7 Hz/bin. Near the 20 Hz low end, a one-bin error is a large fractional RPM error, so low-idle gears are the noisiest to classify.
* **K-Means can converge degenerately.** With only 5 centroids over a narrow 1-D range, an unlucky data distribution (e.g., mostly highway driving in 5th) can collapse centroids; there is no per-cluster occupancy floor enforced yet.
* **Single fixed sample rate.** The pipeline assumes 48 kHz; devices that negotiate a different rate are not yet handled, and aliasing/scaling math is hardcoded to that assumption.

## Areas for Improvement

* **Sensor fusion for speed.** Blend GPS with wheel-speed-derived accelerometer integration (or device IMU dead-reckoning) to get a higher-rate, lower-latency `v` estimate and unlock shift recommendations during transients.
* **Robust pitch detection.** Replace the single-bin peak with a Harmonic Product Spectrum or YIN-style estimator plus frame-to-frame peak tracking to reject transient noise and lock onto the true fundamental.
* **Predictive shift timing.** Feed the accelerometer and the needle's velocity (dNeedle/dt) into a small state machine so the UI can recommend *"upshift now"* slightly ahead of redline rather than reporting it after the fact.
* **Explicit clutch/neutral detection.** Use the sudden decoupling of `f` and `v` (frequency rises while speed holds, or vice-versa) to detect disengagement and freeze classification cleanly.
* **Adaptive tolerance.** Let the accept band tighten automatically as Welford confidence grows, instead of using static `tolLow`/`tolHigh` for the whole vehicle lifecycle.
* **Per-cluster validation.** Add an occupancy floor and silhouette/separation check to K-Means so degenerate centroid collapse is detected and the affected gear falls back to its theoretical seed.
* **Automatic gain control on input.** Normalize mic levels across devices and cabin environments to stabilize the FFT peak magnitude.
* **Settings UI for the vehicle profile.** Surface `vehicle_config.json` fields in-app (currently edit-the-asset) with validation, presets per make/model, and a live preview of the seeded `k_g` values.

---

## License

This project is licensed under the MIT License - see the `LICENSE` file for details.
