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
- [DONE] Guided per-gear calibration using RANSAC (ADR 002):
  - `CalibrationEngine`: RANSAC slope fit, capture state machine, pin flags, 8->13 float migration
  - `native-lib.cpp`: DSP worker routing, JVM attach/detach, `JNI_OnLoad` JavaVM cache,
    `startEngine` global jclass/jmethodID cache, `onGearCalibrated` upcall after mutex release
  - `NativeEngine.kt`: 3 new externals (`beginGearCalibration`, `cancelCalibration`,
    `getCalibrationProgress`) + `onGearCalibrated` @JvmStatic + `CalibrationListener` interface
  - `ShiftAssistantService.kt`: `CALIBRATION_STATE_LEN` 8 -> 13; `isRunning` companion flag
  - New `CalibrationActivity` + `ProgressRingView` + `activity_calibration.xml`: gear grid, ring, lock confirm
  - `MainActivity`: "Calibrate" button gated on `ShiftAssistantService.isRunning`
  - Calib colors isolated in `colors_calibration.xml` (codex owns `colors.xml`/VU meter separately)
  - Host unit tests: `cpp/test/test_ransac_host.cpp` (compiled + passing without NDK)
  - Persistence back-compat: old 8-float `SharedPreferences` blobs load correctly (pins default 0)
  - Invariants preserved: m_gearRatios strictly descending, passive K-Means default, m_mutex never held across upcall
- [DONE] `onGearCalibrated(int gear)` JNI callback C++ -> Kotlin
- Consider `AudioEffect` or AGC on the input stream to normalise mic levels across devices
- Test on physical arm64 device; validate FFT peak detection against known engine frequencies
- Validate RANSAC params (N_min=20, eps=3%, inlier_frac=0.6, Delta_v_min=2 m/s) on device

---

## Session 3 (2026-06-08 / 2026-06-09)

**Branch:** `main` · **Machine:** finch@ArP-ThinkPad-T420 (local, has Android SDK)

### Repo sync — v2 work merged into main

- `claude/focused-turing-1xGm4` had 2 commits past the squash-merge that never reached main: the real v2 (visual-only) work. Merged into `main` (merge commit), resolving 7 add/add conflicts by taking branch (= logical superset).
- main now has: audio output removed, `vehicle_config.json` (Toyota Wigo 1.0 E M/T), `VehicleConfig.kt`, shift decision logic, README "Shift Decision Logic" + "Current Limitations".
- Deleted remote branch `claude/focused-turing-1xGm4` after merge.

### Package rename

- `com.app.shiftassistant` → **`dev.alfieprojects.gearsync`** (matches project + GitHub org).
- Updated: 5 java file moves, package decls, 7 JNI symbols (`Java_dev_alfieprojects_gearsync_NativeEngine_*`), `ASensorManager_getInstanceForPackage` string, `build.gradle` namespace + applicationId, `proguard-rules.pro`, layout custom-view tag. Manifest uses relative `.MainActivity` (no edit). Verified 7 Kt externals ↔ 7 JNI exports.
- `ShiftAssistantService` class name kept (not package-related).

### Build infra — now buildable from clean checkout

Was missing everything needed to build. Added:
- Gradle wrapper (`gradlew`, `gradlew.bat`, `gradle-wrapper.jar` 8.14.1) — fetched from gradle/gradle v8.14.1.
- `gradle.properties`: `android.useAndroidX=true` (AndroidX deps failed without it), `nonTransitiveRClass`, jvm args.
- Pinned `ndkVersion '27.1.12297006'` — AGP 8.3 default 25.1 not installed (SDK has 27 & 28).
- `local.properties` → `sdk.dir=/home/finch/Android/Sdk` (gitignored, machine-specific).
- Adaptive launcher icons (`mipmap-anydpi-v26/ic_launcher{,_round}.xml` + `ic_launcher_{background,foreground}.xml`) — gauge/needle motif. Manifest referenced nonexistent `@mipmap/ic_launcher`.
- `.gitignore` (build/, .gradle/, local.properties, .cxx/, IDE files).

**C++ compile bugfix:** `native-lib.cpp` Oboe `AudioStreamBuilder` chain used `.` on a `*` — must be `->`. Hard compile error. Fixed.

**Verified green:** `./gradlew assembleDebug` → `app-debug.apk` (9.4M). NDK 27.1.12297006 + this machine's SDK.

### Guided calibration — design + plan (design-only, no source changes)

- Wrote `guided-calibration-design.md`: optional, additive guided per-gear calibration (RANSAC line-through-origin) layered on passive auto-calibration. Auto stays default.
- Resolved 2 open decisions: **pin guided gears = YES** (state array 8→13 floats, pad-and-default deserialise); **dedicated CalibrationActivity** (not MainActivity mode flag).
- Ran `planner` skill (multi-agent: architect → quality-reviewer → developer → technical-writer, with QR gates). Output: `plans/guided-calibration-implementation-plan.{md,json}`. 4 milestones / 3 waves, 26 code changes, passed design/code/docs QR gates.
- QR caught 7 real defects pre-implementation (most critical: duplicate `startEngine` JNI symbol → would've been a compile failure / UnsatisfiedLinkError; `g_engClass` global ref leak; host-test access to `private ransacFit`).

### Commits this session (on `main`, ahead of origin)

```text
ad8d249 docs: add guided calibration implementation plan
da66bf9 docs: add guided per-gear calibration design note (RANSAC)
eecce93 build: add gradle wrapper, gradle.properties, NDK pin, launcher icons
686cfce fix(dsp): use -> on Oboe AudioStreamBuilder chain
d70b5ca docs: add CLAUDE.md project guide
c5cf166 refactor: rename package com.app.shiftassistant -> dev.alfieprojects.gearsync
b12db0a Merge remote-tracking branch 'origin/claude/focused-turing-1xGm4'
```

`d70b5ca` and earlier pushed; `686cfce`..`ad8d249` (5 commits) **not yet pushed** to `origin/main`.

### Open items updated

- ✅ Launcher icons added (adaptive). ✅ CLAUDE.md added. ✅ Package renamed. ✅ Buildable.
- ⏳ Guided calibration: design + plan done; **implementation not started** (plan in `plans/`).
- ⏳ `onGearCalibrated(int)` callback — specified in plan, not yet coded.
- ⏳ Test FFT peak detection on physical arm64 device.
- Note: pre-existing uncommitted edits to `VUMeterView.kt`, `activity_main.xml`, `colors.xml` + untracked `vu-meter-specs.md` — not mine, left alone.
