# CLAUDE.md — GearSync

Native Android app for manual-transmission drivers. Analyzes engine acoustics + chassis vibration to detect optimal shift points and shows **visual-only** feedback (VU meter). Local-first, no OBD-II, no cloud.

DSP + edge-ML run in native C++ (NDK) to avoid JVM GC jitter. Kotlin handles lifecycle, UI, GPS, persistence, config loading.

> **v2 (visual-only by default):** The VU meter is the canonical channel. The v1 procedural blip was removed because it used a second *Exclusive/low-latency* Oboe output that competed with the mic-capture DSP for the fast-mixer resource. **ADR 006 (opt-in, `useAudioCues`, default off)** reintroduces audio safely: out-of-band chirps (1.5–2.2 kHz, above the 20–250 Hz engine FFT band) on a **Shared/normal-latency** AudioTrack that never claims the mic's exclusive resource. Default builds remain audio-free; the mic path still owns the low-latency audio resource exclusively.

## Build

```bash
./gradlew assembleDebug
```

Requires local machine — cannot build in remote/CI without Android SDK:
- Android Studio Hedgehog (2023.3.1)+
- NDK 26.x+, CMake 3.22.1+
- Physical arm64 device, API 26+ (emulator can't exercise sensor/audio pipeline)

Toolchain: AGP 8.3.0, Kotlin 1.9.23, Gradle 8.14.1, JVM target 17, `compileSdk 34`, `minSdk 26`. ABIs: `arm64-v8a`, `x86_64`. C++17, `ANDROID_STL=c++_shared` (Oboe shares STL). `prefab true` to consume Oboe native AAR.

**Package namespace + applicationId: `dev.alfieprojects.gearsync`** (renamed from `com.app.shiftassistant`). Renaming again requires updating in lockstep: `app/build.gradle` (namespace + applicationId), `package` decls in `app/src/main/java/dev/alfieprojects/gearsync/*.kt`, JNI symbols `Java_dev_alfieprojects_gearsync_NativeEngine_*` in `native-lib.cpp`, the `ASensorManager_getInstanceForPackage("dev.alfieprojects.gearsync")` string, `proguard-rules.pro`, and the custom-view tag in `activity_main.xml`. Manifest uses relative class names (`.MainActivity`) — moves with namespace.

## Layout

```
app/src/main/cpp/          native C++ core
  native-lib.cpp           Oboe INPUT stream, FFT, sensor thread, shift logic, all JNI bindings
  DspPrimitives.h          shared owned radix-2 FFT primitive (ADR 001, no heavy deps)
  AccelVibrationDsp.h      ADR 004 accel resampling + vibration FFT estimate + M5 autocorrelation harmonic guard
  FusionPolicy.h           ADR 004 M4 pure mic-primary fusion policy (host-testable)
  CalibrationEngine.{h,cpp} Welford online algo + seeded 1-D K-Means++ (k=5) + classifyGear tolerance gate
  test/                    host-only C++ tests for RANSAC and accel vibration DSP
  CMakeLists.txt           links liblog, libandroid, oboe::oboe
app/src/main/java/dev/alfieprojects/gearsync/
  NativeEngine.kt          JNI bridge object (System.loadLibrary("native-lib"))
  VehicleConfig.kt         loads assets/vehicle_config.json, computes theoretical k_g seeds
  ShiftAssistantService.kt Foreground Service + GPS + SharedPreferences persistence
  VUMeterView.kt           60 FPS Choreographer-driven Canvas VU meter; drives ADR 006 cues
  CueState.kt              ADR 006 M1 pure zone-transition → cue-intent state machine
  CuePlayer.kt             ADR 006 M2 Shared/normal-latency AudioTrack out-of-band chirp player
  MainActivity.kt          permission gating + service control
app/src/test/java/…/       JVM unit tests for pure Kotlin logic (CueStateTest) — `./gradlew testDebugUnitTest`
app/src/main/assets/vehicle_config.json   per-vehicle profile (ships tuned for Toyota Wigo 1.0 E M/T)
docs (root):               README.md adr.md specs.md prompt.md references.md session-notes.md
```

Class `ShiftAssistantService` keeps its name (not package-related).

## Core mechanics — shift decision logic

No tachometer feed; everything inferred from mic + GPS. See README "Shift Decision Logic" for full detail.

1. **Engine freq from sound:** mono PCM @ 48 kHz → on each 4096 fresh samples, input callback hands windowed snapshot to DSP worker → Hamming window + radix-2 Cooley-Tukey FFT → dominant peak `f` in 20–250 Hz band. `RPM = 120·f / P` (P = cylinders, 3 for Wigo 1KR-FE). Hand-rolled FFT, no lib (small APK, ADR 001).
2. **Ratio observable:** clutch engaged → `f = k_g·v`. If `v ≥ MIN_SPEED_MPS` (1 m/s), compute `r = f/v`. `r` uniquely identifies the gear (constant within a gear, steps after a shift).
3. **Seeded gears:** theoretical `k_g = (ratio_g · finalDrive · firingFactor) / tireCircumference`, `firingFactor = cylinders / (strokeCycle/2)`. Loaded from `vehicle_config.json` (`VehicleConfig.kt`), pushed to C++ via `setVehicleConfig`, **seeds** K-Means centroids → correct from first drive. K-Means refines centroids vs real `(f,v)` over time.
4. **Classify + tolerance gate** (`classifyGear(r)`): nearest centroid, then asymmetric band `k_g·tolLow ≤ r ≤ k_g·tolHigh`. Wigo band **[0.98, 1.025]** (−2% / +2.5%, accommodates tire wear, load, pressure). Outside band for all gears → returns **−1/unknown** (no flicker mid-shift).
5. **Needle** in `[0,1]` within gear band: `clamp((r − k_{g+1})/(k_g − k_{g+1}), 0, 1)`. Gears sorted descending. Zones: **0–33% blue (lugging → downshift)**, **33–66% green (optimal → hold)**, **66–100% red (redline → upshift)**. EMA smoothed α=0.18, resets to lug end when gear unknown.
6. **Quality gating (don't learn on noise):** GPS @ 1 Hz vs acoustic ~12 Hz misalign during accel/brake. Only feed a ratio to Welford/K-Means after GPS stable (Δ ≤ `speedJitterThresholdMps` 0.5 m/s) for `steadyStateWindowSeconds` (4 s) consecutive.

- **Welford** (`CalibrationEngine.cpp`): tracks `n, mean, m2` of ratio. Variance `σ² = m2/n` = lock-confidence threshold. 3 floats persisted, no DB. Enables fragmented-session calibration.
- **Oboe INPUT only:** mic PCM → lock-free SPSC ring → DSP worker → FFT. No output stream.
- **Sensor thread:** raw `ASENSOR_TYPE_ACCELEROMETER` at fastest delivery (min-delay) via ALooper — shift-event detection (gravity-removed spike via EMA), ADR 004 M0 rate probe, and timestamped magnitude writes into the accel SPSC ring. (Was fused `LINEAR_ACCELERATION` @ 100 Hz pre-ADR-004; switched per DL-007.)

## Concurrency (C++)

Audio input callback (real-time prio) + sensor ALooper + DSP worker. DSP is OUT of the audio callback — handoff via lock-free SPSC (`g_dspWriteSeq`/`g_dspReadSeq` atomic, release/acquire). PCM ring write wait-free (single producer). Audio callback wait-free (no locks, no alloc). ADR 004 accel samples use a separate SPSC ring (`g_accelRingWriteSeq`/`g_accelRingReadSeq`) with sensor thread as producer and DSP worker as consumer; M3 resamples a worker-local 256-sample window and computes diagnostic `f_vib`/prominence there. M5 adds an owned autocorrelation harmonic guard inside `estimateAccelVibrationHz`: if the FFT peak is ~2×/3× a strong ACF fundamental, `f_vib` is corrected down to the fundamental before fusion (defends against chassis-resonant harmonic latching, DL-008). M4 runs the mic-primary fusion policy (`FusionPolicy.h::selectFusedHz`) on the same DSP thread: mic stays primary, vibration only refines (prominence-weighted blend on agreement) or, when mic is weak and vibration clearly stronger, replaces. The selected `f` feeds `ratio = f/speed` into the unchanged Welford/classifyGear/needle path. The PCM-frame fusion decision is authoritative for `g_vibrationSourceMode`/`g_vibrationFusionActive`; `updateVibrationFusionDiagnostics` only sets the disabled-reason while the gate is open.

## JNI interface

| Method | Direction | Caller | Freq |
|---|---|---|---|
| `startEngine()` / `stopEngine()` | Kt→C++ | Service create/destroy | once |
| `updateGpsSpeed(float)` | Kt→C++ | Service location cb | 1 Hz |
| `nativeVUMeterState(): FloatArray` | C++→Kt | VUMeterView (via `getVUMeterState` wrapper) | 60 Hz |
| `nativeAccelProbeStats(): FloatArray` | C++→Kt | ADR 004 M0 diagnostic | on demand |
| `nativeVibrationFusionStats(): FloatArray` | C++→Kt | ADR 004 diagnostic | on demand |
| `resumeCalibrationState(FloatArray)` | Kt→C++ | Service.onCreate | once |
| `saveCalibrationState(): FloatArray` | C++→Kt | Service.onDestroy | once |
| `setVehicleConfig(FloatArray kSeeds, …, Boolean useVibrationFusion)` | Kt→C++ | startup, from VehicleConfig | once |

12 externals in `NativeEngine.kt` must match 12 `Java_dev_alfieprojects_gearsync_NativeEngine_*` exports in `native-lib.cpp` (mismatch = UnsatisfiedLinkError), plus the C++→Kt `onGearCalibrated` upcall. (`nativeAccelProbeStats` is the ADR 004 M0 raw-accel rate diagnostic — returns `[effectiveHz, minIntervalMs, maxIntervalMs, jitterMs, cumulativeSamples, supported]`; `nativeVibrationFusionStats` is the ADR 004 gate/ring/estimate/fusion diagnostic — returns `[requestedAccelHz, measuredAccelHz, useVibrationFusion, fusionActive, disabledReasonCode, latestVibrationHz, vibrationProminence, sourceModeCode, accelRingWritten, accelRingRead, accelRingDropped, latestAccelMagnitude]`. Reason codes: 0 none (gate open, fusion live), 1 config disabled, 2 accel unsupported, 3 low rate. Source modes: 0 MIC_ONLY, 1 FUSED, 2 rejected-low-rate, 3 rejected-low-prominence (also invalid/implausible f_vib), 4 rejected-disagreement. `fusionActive`=1 only when vibration actually changed the selected `f` this frame.) State array (13 floats): `[n, mean, m2, ratio0…ratio4, pin0…pin4]` → SharedPreferences (no Room/SQLite); `deserialise` still loads the legacy 8-float blob, defaulting pin flags to unpinned. The native export is `nativeVUMeterState`; Kotlin `getVUMeterState` is a thin wrapper that returns synthetic `DebugSweep` frames in the `sweep` build type (`BuildConfig.VU_SWEEP`) and proxies the native call otherwise. It returns `[needlePos, dominantHz, speedMps, gear(1-based,0=unknown), confidence, shiftDetected]`. JNI externals need `@JvmStatic`; `NativeEngine` kept by `proguard-rules.pro`.

## Conventions

- Conventional Commits (`feat:`, `fix:`, `docs:`…).
- Manual C++ memory management — validate all floats in `deserialise` (isfinite, n≥0, m2≥0) before mutating state.
- UI strings → `strings.xml`, colors → `colors.xml`. No hardcoded literals.
- VUMeterView: pause Choreographer when not visible (`isRunning` flag, `onVisibilityChanged`).
- New vehicle = edit `vehicle_config.json`, no recompile.
- Workflow + session log: branch for implementation, open PR, request CodeRabbit review, never commit directly to `main`, and append to `session-notes.md`. For parallel agent work, prefer worktrees under `/home/finch/repos/gearsync/worktrees/`.

## Open items (see session-notes.md / README Current Limitations)

- Add `mipmap/` launcher icons (manifest references `@mipmap/ic_launcher` — must exist to build).
- Guided per-gear calibration UI (RANSAC, ADR 002).
- `onGearCalibrated(int)` C++→Kt callback to update UI on gear lock.
- Test FFT peak detection on physical arm64 device.
