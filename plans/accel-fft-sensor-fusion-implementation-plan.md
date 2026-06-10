# Plan

## Overview

The microphone FFT path is the primary engine-frequency source today: `InputCallback` writes PCM into `g_pcmRing`, snapshots into the lock-free SPSC handoff slot, and `dspWorkerFn` runs `findDominantHz` with the local radix-2 `fft_inplace` before feeding `f / v` into `CalibrationEngine`. The accelerometer path currently samples `LINEAR_ACCELERATION` at `SENSOR_US = 10000` (~100 Hz) and only uses magnitude spikes to set `g_shiftDetected`.

**Approach**: ADR 004 adds an opt-in vibration-frequency estimate as a second, slower confidence source. It must first prove the device can deliver a high enough accelerometer rate: below about 300 Hz, engine vibration above ~150 Hz aliases, so fusion remains disabled and the mic path behaves exactly as it does today. When the rate is adequate and the feature is enabled, the sensor thread pushes timestamped acceleration magnitudes into a lock-free SPSC ring, the DSP worker resamples a window onto a uniform grid, reuses the existing Hamming-window + own radix-2 FFT pattern, estimates `f_vib`, and fuses it additively with the acoustic estimate. No phyphox code is copied; phyphox is GPLv3 and is used only as prior-art inspiration.

> **M0 BENCH RESULT (2026-06-10, target device).** phyphox "Acceleration Spectrum" on the target reports **acquisition rate 400 Hz, Nyquist 200 Hz** (256 samples, 0.64 s window, 1.56 Hz resolution). 400 Hz > the 300 Hz gate, and 200 Hz Nyquist covers the full Wigo 1KR-FE firing band (3-cyl 4-stroke: firing Hz = RPM × 0.025 → idle ~19 Hz, redline ~6500 RPM ≈ 162 Hz). **Gate PASSED — fusion is viable on this device.** Three consequences fold into the plan below:
> 1. **Use RAW `ASENSOR_TYPE_ACCELEROMETER`, not `LINEAR_ACCELERATION`** — phyphox's 400 Hz is from the raw sensor; the fused `LINEAR_ACCELERATION` is commonly rate-throttled below raw on the same chip. Gravity is DC (~0 Hz) and is removed by the FFT band search (15–160 Hz), so the raw sensor needs no fusion. (DL-007)
> 2. **The harmonic guard (Milestone 5) is REQUIRED, not optional** — 2× firing at redline ≈ 324 Hz aliases past the 200 Hz Nyquist and folds to ~76 Hz as a ghost peak. (DL-008)
> 3. M0's remaining job shrinks to a **lightweight in-app rate assertion** confirming our own `ASensorEventQueue` path also lands ~400 Hz (different code path than phyphox) — a confirmation, no longer a blocking spike.
>
> **M0 IN-APP RESULT (2026-06-10).** First run read a hard **200.0 Hz / 0.00 ms jitter** — the Android 12+ (API 31+) sensor-rate cap (DL-011). After declaring `HIGH_SAMPLING_RATE_SENSORS`, the same device reads **399.2 Hz, 2.50 ms interval — PASS**, matching phyphox. **M0 complete; our code path clears the gate.** Verified with no cable/adb via the `src/debug/` `DebugProbeActivity` on-screen readout.

## Planning Context

### Decision Log

| ID | Decision | Reasoning Chain |
|---|---|---|
| DL-001 | Milestone 0 is a device sample-rate probe, and all later work is gated on its result. **RESULT 2026-06-10: PASSED** (400 Hz raw accel / 200 Hz Nyquist, bench-measured via phyphox on the target). | Android sensor delivery is device-capped and the current 100 Hz request cannot cover high-RPM firing frequencies -> any FFT before measuring the actual rate risks building an aliased signal path -> probe fastest-request first, compute effective rate/jitter, do not proceed unless usable rate >= ~300 Hz. Bench check satisfied the gate; the in-app probe now only confirms our own sensor code path lands ~400 Hz. |
| DL-002 | Mic path stays primary and default; vibration fusion is opt-in and additive | The existing mic pipeline already drives the VU meter and gear classification with acceptable latency -> accelerometer FFT needs a longer window and can fail when the mount is not mechanically coupled -> keep `findDominantHz` as the default source, expose fusion as a disabled-by-default config/runtime option, and fall back to mic-only on low rate, low vibration prominence, or disagreement. |
| DL-003 | Reuse the current native DSP style: SPSC handoff, DSP worker ownership, and local radix-2 `fft_inplace` | ADR 001 keeps DSP in native C++ with no heavy dependencies, and `native-lib.cpp` already has a wait-free PCM callback plus SPSC snapshot slot -> mirror that pattern for timestamped acceleration samples and run all FFT/resampling/fusion off callback/sensor hot paths -> no new DSP dependency, no copied phyphox implementation. |
| DL-004 | Estimate vibration as timestamped magnitude, then resample before FFT | Android sensor timestamps jitter and sensor delivery is batched/polled -> running FFT directly on irregular samples smears peaks -> store `(timestampNs, |accel|)` and linearly interpolate onto a uniform grid at the measured nominal rate before DC removal, Hamming window, and `fft_inplace`. |
| DL-005 | Fusion weights by signal quality and agreement, never by blind averaging | Mic and vibration can fail differently: cabin noise hurts mic, weak mount coupling hurts vibration, and harmonics can dominate either FFT -> compute peak prominence for each estimate, require frequency agreement for weighted averaging, and otherwise choose the higher-confidence estimate with a bias toward mic when confidence is weak or ambiguous. |
| DL-006 | Branch/PR workflow is mandatory for implementation | ADR 004 touches native realtime paths and user-facing behavior -> never commit to `main`; create feature branches for implementation milestones, open PRs, run local builds/device checks, and use CodeRabbit review before merge. |
| DL-007 | Use raw `ASENSOR_TYPE_ACCELEROMETER` for vibration FFT, not the fused `LINEAR_ACCELERATION` | M0 bench: phyphox's 400 Hz spectrum reads the raw sensor; the fused `LINEAR_ACCELERATION` is frequently throttled below raw on the same chip -> request raw accel at fastest rate -> gravity appears only as a DC (~0 Hz) term, already excluded by the 15–160 Hz band search, so no fusion/high-pass is needed. The existing shift-spike path keeps its own magnitude/threshold logic on the same raw stream. |
| DL-008 | The harmonic guard is REQUIRED on this device, not an optional later milestone | Nyquist is 200 Hz but 2×/3× firing harmonics exceed it at high RPM (2× redline ≈ 324 Hz) -> they alias and fold back as ghost peaks (~76 Hz) that a bare FFT-peak picker will latch -> band-limit the search to <=~160 Hz and ship the Milestone 5 autocorrelation/subharmonic + mic-agreement guard as part of the core fusion, not as a deferred add-on. |
| DL-009 | Adding a diagnostic JNI call must preserve the 10↔10 external/export parity invariant | `NativeEngine.kt` externals must match `Java_dev_alfieprojects_gearsync_NativeEngine_*` exports exactly (mismatch = UnsatisfiedLinkError) -> any new probe/diagnostic JNI method increments BOTH sides in lockstep, keeps `@JvmStatic`, and is retained by `proguard-rules.pro`; update the count + the JNI table in `CLAUDE.md`. |
| DL-011 | Declare `HIGH_SAMPLING_RATE_SENSORS` in the manifest | Android 12+ (API 31+) caps sensor sampling at **200 Hz** for any app lacking this permission, regardless of the sensor's true min-delay -> measured in-app as a hard 200.0 Hz / 0.00 ms-jitter signature while phyphox (which declares it) read 400 Hz on the same device -> 200 Hz → Nyquist 100 Hz aliases the ≤162 Hz firing band, so the permission is a **correctness** requirement. It is a normal/install-time permission (auto-granted, no runtime prompt). Confirmed: adding it lifted the target to 399 Hz. |
| DL-010 | Vibration FFT/resampling/fusion runs inside the existing `dspWorkerFn`, not a second worker thread | C-008 already mandates `dspWorkerFn` as the sole DSP mutation site, and a second worker would need its own JVM attach + ordering vs the mic path -> the accel SPSC ring is drained from the same `dspWorkerFn` loop iteration that handles the mic snapshot; the 0.64 s accel window is decoupled from the ~85 ms mic window by tracking its own fill counter, so the slower vibration estimate updates every N mic iterations without a new thread. |

### Constraints

- C-001 (user-specified): Milestone 0 must be a device sample-rate probe; all later milestones are gated on measured accelerometer rate, because the feature aliases below about 300 Hz. **Gate satisfied 2026-06-10 (400 Hz / 200 Hz Nyquist, bench).**
- C-010 (M0-derived): Vibration analysis reads raw `ASENSOR_TYPE_ACCELEROMETER` (DL-007), not `LINEAR_ACCELERATION`. Band search 15–160 Hz removes the gravity DC term in lieu of fusion.
- C-011 (M0-derived): Search band must stay <=~160 Hz (below the 200 Hz Nyquist guard); the harmonic guard (DL-008) is part of core fusion, not optional.
- C-002 (user-specified): Reuse the existing SPSC ring + `fft_inplace` + DSP-worker pattern. Use the project-owned radix-2 FFT only per ADR 001; add no heavy DSP dependency.
- C-003 (user-specified): phyphox is GPLv3. Use approaches and ideas only; copy no code, constants tables, or implementation structure from phyphox.
- C-004 (user-specified): Mic path remains primary and default. Fusion is additive, opt-in, and degrades gracefully when rate is too low or phone mount coupling is weak.
- C-005 (user-specified): Use a branch + PR + CodeRabbit workflow for implementation; never commit implementation work to `main`.
- C-006 (code-derived): `InputCallback` must remain wait-free: no mutex, allocation, blocking, or heavy DSP on the Oboe callback.
- C-007 (code-derived): `sensorThreadFn` currently owns `ASENSOR_TYPE_LINEAR_ACCELERATION`; shift-spike detection must keep working when the sensor is switched to raw `ACCELEROMETER` (DL-007) and samples are also routed to vibration analysis. Re-tune `SHIFT_ACCEL_THRESH` if needed — raw accel includes gravity, so spike detection should act on a high-passed or delta magnitude.
- C-008 (code-derived): `dspWorkerFn` is the only place that should run FFT, Welford/K-Means updates, gear classification, needle mapping, and future fusion state mutation.
- C-009 (code-derived): JNI state arrays and Kotlin surface must stay backward-compatible unless a milestone explicitly updates and verifies the contract.

### Known Risks

- ~~**Device caps accelerometer near 100 Hz or 200 Hz.**~~ **RETIRED** — M0 bench measured 400 Hz raw accel on the target. Risk remains for *other* target devices; the in-app rate assertion + low-rate disable path stay in for portability.
- **Fused `LINEAR_ACCELERATION` slower than raw.** Mitigation: DL-007 uses raw `ACCELEROMETER`; the in-app M0 assertion measures the raw path specifically.
- **Harmonic aliasing past the 200 Hz Nyquist** (2× redline ≈ 324 Hz → folds to ~76 Hz). Mitigation: DL-008 — band-limit to ≤160 Hz and ship the harmonic guard as core, plus mic-agreement rejection.
- **Mount coupling is weak or inconsistent.** Mitigation: vibration peak prominence and mic/vibration agreement determine whether `f_vib` participates; weak vibration is ignored.
- **Latency mismatch between mic and vibration.** Mitigation: mic estimate remains responsive and primary; vibration estimate is a slower corrector/confidence boost.
- **Harmonic latch in vibration FFT.** Mitigation: start with prominence and agreement gates; optionally add an autocorrelation/subharmonic check in a later milestone after basic fusion is validated.
- **Realtime regression in audio callback or sensor thread.** Mitigation: sensor thread only computes magnitude and writes ring entries; Oboe callback remains unchanged; DSP worker owns resampling/FFT/fusion.
- **License contamination from phyphox.** Mitigation: document GPLv3 prior art, do not copy code, and reimplement any selected ideas from first principles using existing project primitives.

## Milestones

### Milestone 0: Device accelerometer sample-rate probe and go/no-go gate — **GATE PASSED (bench)**

> **Bench gate already satisfied (2026-06-10):** phyphox reports **400 Hz / 200 Hz Nyquist** on the target — above the 300 Hz threshold. This milestone is now an **in-app rate assertion** that confirms our own `ASensorEventQueue` path (a different code path than phyphox) also reaches ~400 Hz, and records jitter for the resampler. Not a blocking spike.

**Files**: `app/src/main/cpp/native-lib.cpp`, `app/src/main/java/dev/alfieprojects/gearsync/NativeEngine.kt`, optional temporary probe UI/log plumbing

**Flags**: device-required, no-fusion-yet (gate met)

**Requirements** — **STATUS: DONE** (implemented on `feat/accel-probe`; validated on target):

- ✅ Switched the sensor request from `ASENSOR_TYPE_LINEAR_ACCELERATION` @ `SENSOR_US = 10000` to **raw `ASENSOR_TYPE_ACCELEROMETER` at fastest practical rate** (DL-007).
- ✅ Declared `HIGH_SAMPLING_RATE_SENSORS` in the manifest (DL-011) — without it, API 31+ silently capped delivery at 200 Hz (Nyquist 100 Hz → aliases the firing band). Confirmed on target: 200 Hz → 399 Hz after adding it.
- ✅ Measured effective sample rate from event timestamps over windows: average Hz, min/max inter-arrival, jitter, samples per window.
- ✅ Exposed the measured rate via logcat **and a JNI diagnostic** (`nativeAccelProbeStats`), keeping 11↔11 external/export parity (DL-009).
- ✅ Re-asserts the hard gate (`effectiveRateHz >= 300 Hz`) in-app for portability to *other* devices; below it, ADR 004 stays disabled + mic-only.
- ✅ Preserved shift-spike detection on the raw stream (C-007 — gravity removed via slow EMA before the threshold test).

**Acceptance Criteria** — all met:

- ✅ Physical device reports measured raw-accel rate + jitter (not just requested): **399.2 Hz / 2.50 ms** on target.
- ✅ The in-app gate disables fusion + stays mic-only below ~300 Hz (first run read 200 Hz → caught the cap).
- ✅ 11↔11 JNI parity preserved; `CLAUDE.md` JNI table + count updated.
- ✅ `./gradlew assembleDebug` passes (built + installed on device).

**Tests**:

- type:device||normal:probe logs raw-accel effective rate for >=10 s and matches phyphox ~400 Hz||edge:ACCELEROMETER unavailable reports unsupported and leaves mic path unaffected||error:rate below threshold disables future fusion path

### Milestone 1: ADR 004 feature flag, diagnostic state, and implementation workflow scaffold

**Files**: `adr.md`, `app/src/main/assets/vehicle_config.json`, `VehicleConfig.kt`, `NativeEngine.kt`, `native-lib.cpp`, `README.md`

**Flags**: config, docs, opt-in

**Requirements**:

- Add ADR 004 to `adr.md` with the final decision: accelerometer FFT is a second source fused with the acoustic estimate, gated by measured sensor rate, opt-in, and mic-primary.
- Add an opt-in config/runtime flag such as `useVibrationFusion`, defaulting false.
- Add diagnostic state for vibration support: requested rate, measured rate, enabled/disabled reason, latest `f_vib`, vibration prominence, and fused-source mode (`MIC_ONLY`, `FUSED`, `VIB_REJECTED_LOW_RATE`, `VIB_REJECTED_LOW_PROMINENCE`, etc.).
- Keep `getVUMeterState()` behavior backward-compatible for existing UI consumers; expose richer diagnostics through a separate JNI call if needed.
- Document the implementation workflow: branch from `main`, implement on feature branch, open PR, run local build/device checks, request CodeRabbit review, never commit to `main`.

**Acceptance Criteria**:

- Fusion defaults off even on capable devices.
- Low-rate devices report a clear disabled reason and continue mic-only.
- Existing mic-driven classification and VU meter behavior is unchanged with the flag off.
- ADR 004 records the GPLv3 phyphox boundary: ideas only, no copied code.

**Tests**:

- type:unit/manual||normal:config false yields mic-only||edge:config true plus low measured rate still yields mic-only||error:missing config field defaults false

### Milestone 2: Timestamped accelerometer SPSC ring and sensor hot-path integration

**Files**: `app/src/main/cpp/native-lib.cpp`

**Flags**: realtime, lock-free

**Requirements**:

- Add a fixed-size SPSC ring for acceleration samples, with entries `{timestampNs, magnitude}`. Size for the 0.64 s window at ~400 Hz (≈256 samples) plus headroom.
- Sensor thread computes magnitude from raw `ACCELEROMETER` (DL-007), preserves the existing `SHIFT_ACCEL_THRESH` visual flash behavior (now on a delta/high-passed magnitude since gravity is present), and writes timestamped samples into the ring without heap allocation.
- Use atomics and wrap-safe counters following the PCM ring style; avoid locks in the sensor event loop.
- Track ring overruns/dropped samples as diagnostics but do not block the sensor thread.
- Keep all FFT/resampling/fusion out of `sensorThreadFn`.

**Acceptance Criteria**:

- Ring writes are allocation-free and non-blocking.
- Shift flash still fires on acceleration spikes.
- Diagnostics show sample counts increasing at the measured rate on capable devices.
- `./gradlew assembleDebug` passes.

**Tests**:

- type:device/manual||normal:ring receives timestamped samples while service runs||edge:ring overrun increments a counter without crash||error:raw ACCELEROMETER unavailable leaves vibration unsupported and mic path alive

### Milestone 3: Acceleration DSP window, resampling, and vibration FFT estimate

**Files**: `app/src/main/cpp/native-lib.cpp`, optional `app/src/main/cpp/test/*`

**Flags**: DSP, no-new-deps

**Requirements**:

- In `dspWorkerFn` or a closely related DSP-worker helper, copy a stable acceleration window from the SPSC ring into local storage before processing.
- Resample timestamped magnitudes onto a uniform grid at the measured nominal rate using linear interpolation.
- Subtract DC (this also removes the gravity offset from the raw sensor — DL-007), apply the same Hamming-window principle used by `findDominantHz`, and call the existing `fft_inplace`.
- Search an engine vibration band of **~15 Hz to min(160 Hz, Nyquist guard)** — hard-cap at 160 Hz so the band sits below the 200 Hz Nyquist (C-011). Note harmonics above Nyquist alias into this band; the Milestone 5 guard (now required, DL-008) handles the resulting ghosts.
- Return `f_vib` plus a peak-prominence metric, such as peak magnitude divided by median or local noise-floor magnitude.
- Do not introduce kissfft or any other external FFT dependency; ADR 001 is satisfied by the existing owned radix-2 implementation.

**Acceptance Criteria**:

- Synthetic/native host tests recover known vibration frequencies from generated timestamp-jittered samples within tolerance.
- Runtime vibration estimate remains disabled when sample rate gate fails.
- No allocation occurs in sensor/audio hot paths; any DSP buffers are preallocated or worker-local.
- `./gradlew assembleDebug` passes.

**Tests**:

- type:unit||normal:jittered 50 Hz synthetic signal resamples and recovers peak||edge:insufficient samples returns invalid/no estimate||error:Nyquist below requested band clamps band and disables high-frequency estimate

### Milestone 4: Mic-primary fusion policy and graceful degradation

**Files**: `app/src/main/cpp/native-lib.cpp`, `NativeEngine.kt`, optional diagnostics UI/logs

**Flags**: behavior, safety

**Requirements**:

- Preserve the existing `findDominantHz(localSnapshot, FFT_SIZE)` mic estimate as the primary source.
- Add fusion only when opt-in is true, measured rate is adequate, vibration prominence is above threshold, and `f_vib` is plausible.
- When mic and vibration agree within a tolerance band, compute confidence-weighted `f_fused`.
- When they disagree, choose the higher-confidence source only if it is clearly stronger; otherwise fall back to mic.
- Feed `ratio = selectedHz / speed` into the existing `CalibrationEngine::updateWelford`, `classifyGear`, and needle mapping path unchanged.
- Store diagnostics for source choice and rejection reasons.

**Acceptance Criteria**:

- With fusion off, behavior is bit-for-bit or practically equivalent to current mic-only behavior.
- With fusion on and valid agreement, fused frequency is used for ratio/classification.
- With low rate, low prominence, missing accel, or weak mount, source remains mic-only.
- A noisy/implausible vibration estimate cannot override a strong mic estimate.

**Tests**:

- type:unit/manual||normal:agreeing mic/vib estimates average by confidence||edge:vib low prominence rejected||error:large disagreement falls back to mic unless vibration is clearly stronger and mic confidence is weak

### Milestone 5: Harmonic guard for vibration estimate — **REQUIRED on the target (DL-008)**

> Promoted from optional to required: the 200 Hz Nyquist cannot represent 2×/3× firing harmonics at high RPM, so they alias and fold into the 15–160 Hz search band. A bare FFT-peak picker will latch these ghosts, so the guard ships as part of core fusion, not a deferred add-on.

**Files**: `app/src/main/cpp/native-lib.cpp`, `app/src/main/cpp/test/*`

**Flags**: required, phyphox-inspired-ideas-only

**Requirements**:

- Implement a project-owned autocorrelation or subharmonic consistency check; the vibration FFT *will* latch onto aliased 2x/3x harmonics on this device, so this is not contingent on field observation.
- Treat phyphox as prior art only: no copied code or direct translation.
- Prefer the fundamental when autocorrelation and FFT indicate a likely harmonic relationship and the corrected frequency is plausible against mic/GPS/gear context.
- Ship this guard as part of the initial fusion (M3/M4), not deferred: the 200 Hz Nyquist guarantees high-RPM harmonic aliasing on this device (DL-008), so M3/M4 data is not needed to justify it. Tune thresholds against M3/M4 field data, but the guard is present from first fusion.

**Acceptance Criteria**:

- Synthetic harmonic-rich vibration tests choose the fundamental when the FFT peak is at 2x or 3x.
- Pure single-tone tests do not incorrectly halve/third the estimate.
- No regression to mic-only fallback behavior.

**Tests**:

- type:unit||normal:fundamental plus stronger second harmonic resolves to fundamental||edge:ambiguous autocorrelation leaves FFT peak unchanged||error:out-of-band corrected frequency rejected

### Milestone 6: End-to-end validation, PR review, and CodeRabbit workflow

**Files**: `README.md`, `session-notes.md`, PR description/checklist

**Flags**: device-required, review

**Requirements**:

- Validate on physical hardware with fusion off, fusion on with adequate rate, and fusion requested but rejected due to rate/prominence.
- Compare `f_acoustic`, `f_vib`, selected/fused frequency, gear classification, and VU needle behavior across idle, steady cruise, noisy cabin, and low-RPM lugging cases.
- Record device model, measured accel rate, mount type, and whether mount coupling produced usable prominence.
- Open a PR from the implementation branch, include logs/results, and request CodeRabbit review.
- Do not commit to `main`; merge only after local build, device validation, and review feedback are addressed.

**Acceptance Criteria**:

- `./gradlew assembleDebug` passes.
- Physical-device validation confirms mic-only default remains intact.
- Fusion improves or at least does not degrade classification confidence in noisy acoustic cases on an adequate-rate, well-coupled mount.
- CodeRabbit findings are either fixed or explicitly dispositioned in the PR.

**Tests**:

- type:device||normal:quiet cabin and noisy cabin A/B with fusion off/on||edge:loose mount rejects vibration by prominence||error:rate-capped device keeps mic-only and does not crash

## PR Checklist

- Branch is not `main`.
- Milestone 0 measured rate is attached before any fusion code is reviewed.
- No heavy DSP dependencies added.
- No phyphox code copied.
- Mic-only default verified.
- Low-rate and weak-mount graceful degradation verified.
- `./gradlew assembleDebug` passes.
- Physical-device logs included.
- CodeRabbit review requested and addressed.
