# GearSync — Session Notes
**Date:** 2026-06-06  
**Branch:** `claude/focused-turing-1xGm4`  
**Repo:** `alfieprojectsdev/gearsync`

---

## What was accomplished

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
| `gradle/wrapper/gradle-wrapper.properties` | Gradle 8.4 |
| `app/proguard-rules.pro` | Keeps NativeEngine JNI class from being stripped |

---

## Key architectural decisions implemented

### 1. Welford Online Algorithm (`CalibrationEngine.cpp`)
Tracks `n`, `mean`, `m2` for the `frequency / speed` ratio. Only three floats are persisted per session — no database needed. State is serialised to `SharedPreferences` on service destroy and restored via `resumeCalibrationState()` JNI call on next launch.

### 2. 1-D K-Means++ (`CalibrationEngine.cpp`)
Runs over accumulated ratio samples once ≥ 20 observations exist, every 10 new samples thereafter. K-Means++ seeding (farthest-first) avoids degenerate centroid initialisation. Centroids are sorted descending so index 0 = gear 1 (highest ratio = lowest speed gear).

### 3. Radix-2 Cooley-Tukey FFT (`native-lib.cpp`)
4096-sample window at 48 kHz (~85 ms). Hamming window applied before transform. Peak bin searched in the 20–250 Hz engine frequency band. No third-party FFT library — implemented inline to keep the APK small (per ADR 001).

### 4. Oboe dual-stream (`native-lib.cpp`)
- **Input stream:** mic PCM → ring buffer → FFT → ratio feed → Welford/K-Means
- **Output stream:** 50 ms sine blip at 2 kHz with attack/release envelope, triggered by shift-spike detection on the sensor thread

### 5. ASensorManager thread (`native-lib.cpp`)
Dedicated thread polls `ASENSOR_TYPE_LINEAR_ACCELERATION` at 100 Hz via `ALooper`. Acceleration magnitude > 4 m/s² sets a `g_triggerBlip` atomic flag consumed by the output audio callback.

### 6. VU Meter (`VUMeterView.kt`)
Choreographer-driven at 60 FPS. Three arc zones (blue = lugging, green = optimal, red = redline) sweep 270°. Needle position exponentially smoothed (α = 0.18) to eliminate jitter from per-frame native state reads. Gear number and dominant Hz displayed as text overlay.

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

---

## What was NOT used

- `references.md` — listed Oboe sample projects and MicUp as reference implementations. All decisions were already fully specified in `prompt.md` / `adr.md` so these were not consulted during implementation.

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
