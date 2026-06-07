# GearSync — Session Notes
**Date:** 2026-06-07  
**Repo:** `alfieprojectsdev/gearsync`

---

## Session 2 (2026-06-07)

### Changes made

#### Audio cue frequency update
- Changed synthesized blip frequency from 2000 Hz → **14200 Hz**
- Added `g_audioCueFrequency` as `std::atomic<float>` initialized to `14200.0f` in `native-lib.cpp`
- `OutputCallback::onAudioReady` reads frequency once per buffer via `g_audioCueFrequency.load(std::memory_order_relaxed)`

#### JNI + Kotlin API for runtime frequency control
- Added `setAudioCueFrequencyNative(hz: Float)` private external function in `NativeEngine.kt`
- Added public `setAudioCueFrequency(hz: Float)` wrapper that clamps input to 20–22050 Hz via `coerceIn` before passing to native
- Added corresponding JNI binding `Java_com_app_shiftassistant_NativeEngine_setAudioCueFrequencyNative` in `native-lib.cpp`
- Constants `MIN_CUE_FREQ_HZ = 20f` and `MAX_CUE_FREQ_HZ = 22050f` defined in `NativeEngine` companion

#### CodeRabbit review comments addressed (PR #1, 14 threads total)

Round 1 fixes (commit `b4edfb8`):
- `openStreams()` now returns `bool`; `startEngine` aborts if it returns false
- DSP moved out of real-time audio callback via lock-free SPSC handoff (`g_dspWriteSeq`/`g_dspReadSeq` atomic uint32_t with release/acquire ordering)
- `g_pcmRing` write is wait-free (no mutex — single producer)
- `m_lastKMeansSample` tracks last K-Means n; trigger uses `(n - lastRun) % interval` to avoid skipping after deserialise restore
- Bounded `m_ratioSamples` sliding window: evicts `KMEANS_PRUNE_BATCH=512` entries when size exceeds `MAX_KMEANS_SAMPLES + PRUNE_BATCH` (amortised O(1))
- Pre-lock validation of all floats in `CalibrationEngine::deserialise` (isfinite, n≥0, m2≥0) — returns early without touching state on corruption
- `m_rng` (mt19937) seeded from `std::random_device` for non-deterministic K-Means++ centroid selection
- `CALIBRATION_STATE_LEN = 8` constant in `ShiftAssistantService` companion (replaces magic number)
- `isRunning` flag in `VUMeterView`; frame callback only reposts `if (isRunning)`; `onVisibilityChanged` pauses/resumes Choreographer
- Toast strings moved to `strings.xml`; hardcoded colour literals moved to `colors.xml`
- Removed `tools:targetApi="34"` from `AndroidManifest.xml`; removed redundant `android:fillColor` from notification drawable

Round 2 fixes (commit `b610483`):
- `@JvmStatic` retained on `setAudioCueFrequencyNative` (required for JNI parameter consistency — `jclass` not `jobject`)
- Gradle wrapper upgraded from 8.4 → **8.14.1**
- VUMeterView KDoc corrected: arcs sweep from 135° (bottom-left) to 45° (top-right), 270° clockwise

### PR #1 merged
- Branch `claude/focused-turing-1xGm4` squash-merged to `main` (SHA: `46ba169`)
- All 14 CodeRabbit review threads resolved before merge

---

## Session 1 (2026-06-06)

### What was accomplished

Built the entire GearSync Android project from scratch based on `prompt.md`, `specs.md`, `adr.md`, and `README.md`. The repo previously contained only documentation; this session produced all source code, build configuration, and resources.

### Files created (18 files, 1241 lines)

| File | Description |
|---|---|
| `app/src/main/cpp/CMakeLists.txt` | NDK build config, links liblog, libandroid, Oboe via prefab |
| `app/src/main/cpp/CalibrationEngine.h` | CalibrationEngine class declaration, WelfordState struct |
| `app/src/main/cpp/CalibrationEngine.cpp` | Welford Online Algorithm + 1-D K-Means++ (k=5) implementation |
| `app/src/main/cpp/native-lib.cpp` | Oboe dual-stream, FFT, sensor thread, all JNI bindings |
| `app/src/main/java/com/app/shiftassistant/NativeEngine.kt` | JNI bridge object |
| `app/src/main/java/com/app/shiftassistant/ShiftAssistantService.kt` | Foreground Service + GPS + state persistence |
| `app/src/main/java/com/app/shiftassistant/VUMeterView.kt` | 60 FPS hardware-accelerated Canvas VU meter |
| `app/src/main/java/com/app/shiftassistant/MainActivity.kt` | Permission gating + service control |
| `app/src/main/AndroidManifest.xml` | Permissions, foreground service type declaration |
| `app/src/main/res/layout/activity_main.xml` | Dark UI layout with VUMeterView + start/stop buttons |
| `app/src/main/res/values/strings.xml` | String resources |
| `app/src/main/res/values/themes.xml` | Dark theme (#0D0D1A) |
| `app/src/main/res/drawable/ic_notification.xml` | Vector icon for foreground service notification |
| `app/build.gradle` | Prefab enabled, Oboe 1.9.0, Play Services Location, Material |
| `build.gradle` | Root build file, AGP 8.3.0, Kotlin 1.9.23 |
| `settings.gradle` | Single-module project config |
| `gradle/wrapper/gradle-wrapper.properties` | Gradle 8.14.1 |
| `app/proguard-rules.pro` | Keeps NativeEngine JNI class from being stripped |

---

## Key architectural decisions implemented

### 1. Welford Online Algorithm (`CalibrationEngine.cpp`)
Tracks `n`, `mean`, `m2` for the `frequency / speed` ratio. Only three floats are persisted per session — no database needed. State is serialised to `SharedPreferences` on service destroy and restored via `resumeCalibrationState()` JNI call on next launch.

### 2. 1-D K-Means with farthest-first init (`CalibrationEngine.cpp`)
Runs over accumulated ratio samples once ≥ 20 observations exist, every 10 new samples thereafter. Farthest-first seeding avoids degenerate centroid initialisation. Centroids are sorted descending so index 0 = gear 1 (highest ratio = lowest speed gear). Training window capped at 4096 samples with 512-entry batch eviction.

### 3. Radix-2 Cooley-Tukey FFT (`native-lib.cpp`)
4096-sample window at 48 kHz (~85 ms). Hamming window applied before transform. Peak bin searched in the 20–250 Hz engine frequency band. No third-party FFT library — implemented inline to keep the APK small (per ADR 001).

### 4. Oboe dual-stream (`native-lib.cpp`)
- **Input stream:** mic PCM → ring buffer → lock-free SPSC snapshot → DSP worker thread → FFT → ratio feed → Welford/K-Means
- **Output stream:** 50 ms sine blip at 14200 Hz with attack/release envelope, triggered by shift-spike detection on the sensor thread

### 5. ASensorManager thread (`native-lib.cpp`)
Dedicated thread polls `ASENSOR_TYPE_LINEAR_ACCELERATION` at 100 Hz via `ALooper`. Acceleration magnitude > 4 m/s² sets a `g_triggerBlip` atomic flag consumed by the output audio callback.

### 6. VU Meter (`VUMeterView.kt`)
Choreographer-driven at 60 FPS. Three arc zones (blue = lugging, green = optimal, red = redline) sweep 270° from 135° (bottom-left) to 45° (top-right) clockwise. Needle position exponentially smoothed (α = 0.18) to eliminate jitter. Pauses automatically when view is not visible.

### 7. Session stitching
`ShiftAssistantService` calls `saveCalibrationState()` on `onDestroy()` and `resumeCalibrationState()` on `onCreate()`. The 8-float array (`n`, `mean`, `m2`, 5 gear ratios) is stored in `SharedPreferences` — no Room/SQLite dependency.

---

## JNI interface summary

| Method | Direction | Called by | Frequency |
|---|---|---|---|
| `startEngine()` | Kotlin → C++ | ShiftAssistantService.onCreate | Once |
| `stopEngine()` | Kotlin → C++ | ShiftAssistantService.onDestroy | Once |
| `updateGpsSpeed(float)` | Kotlin → C++ | ShiftAssistantService location callback | 1 Hz |
| `getVUMeterState(): FloatArray` | C++ → Kotlin | VUMeterView Choreographer callback | 60 Hz |
| `resumeCalibrationState(FloatArray)` | Kotlin → C++ | ShiftAssistantService.onCreate | Once |
| `saveCalibrationState(): FloatArray` | C++ → Kotlin | ShiftAssistantService.onDestroy | Once |
| `setAudioCueFrequency(float)` | Kotlin → C++ | Settings UI / runtime | On demand |

---

## Build requirements (cannot build in this remote environment)

Requires a local machine with:
- Android Studio Hedgehog or newer
- NDK 26.x+ (installed via SDK Manager)
- CMake 3.22.1+ (installed via SDK Manager)
- Physical Android device recommended (API 26+, arm64) — emulator cannot exercise the full sensor/audio pipeline

```bash
./gradlew assembleDebug
```

---

## Open items / next steps

- Add `mipmap/` launcher icons (currently only `ic_notification` vector exists)
- Wire up a calibration UI flow (guided per-gear calibration using RANSAC as described in ADR 002)
- Add `onGearCalibrated(int gear)` JNI callback from C++ → Kotlin to update UI when a gear locks in
- Consider `AudioEffect` or AGC on the input stream to normalise mic levels across devices
- Test on physical arm64 device; validate FFT peak detection against known engine frequencies
- Wire `setAudioCueFrequency` to a settings UI slider (range 20–22050 Hz)
