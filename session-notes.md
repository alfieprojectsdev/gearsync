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

---

## Session 2026-06-09 — guided calibration implemented + VU redesign landed + PR workflow

Adopted a **branch + PR + CodeRabbit** flow (never commit to `main`). Helper scripts live in `scripts/` (untracked; `gh pr create`/merge are sandbox-blocked, so they're run via `!`).

### Shipped
- **Guided calibration implemented** from `plans/guided-calibration-implementation-plan.md` (all 4 milestones): RANSAC slope fit + capture state machine + pin flags + 8→13-float persistence migration (legacy back-compat); `onGearCalibrated` DSP-worker JVM upcall (JNI_OnLoad JavaVM cache + startEngine global refs, fired outside `m_mutex`); `CalibrationActivity` + top-level `ProgressRingView`; `MainActivity` Calibrate button. Host tests `cpp/test/test_ransac_host.cpp` — **8/8 pass** (no NDK).
- **VU meter redesign** (codex's `vu-meter-specs.md`) was already implemented in the working tree; landed it. Calibration deliberately avoided codex-owned `colors.xml`/`VUMeterView.kt` — calib colors isolated in `colors_calibration.xml`; only shared file is `activity_main.xml` (+Calibrate button).

### PR board (repo: `alfieprojectsdev/gearsync`, CodeRabbit on)
- **#2** VU redesign → `main` — **MERGED** (merge commit, to keep stacked diff clean).
- **#4** `chore` gradlew.bat CRLF + `.gitattributes` → `main` — **MERGED** (squash).
- **#3** guided calibration → `main` (retargeted from the VU branch via REST after `gh pr edit` hit a gh projects-classic bug) — **OPEN, mergeable**, diff is calibration-only. All **7 CodeRabbit findings addressed** in `e5d765f` (Majors: unbounded capture/throttle; pin-by-index not sorted-slot; cache `calibratingGear()` for the upcall; re-enable gear buttons after a lock). CR won't re-review a stacked PR's already-seen commits — retarget-to-main was the fix.
- **#5** docs ADR 004 (accel-FFT sensor fusion note) → `main` — **OPEN**.

### Key decisions / gotchas (for resume)
- `ProgressRingView` is **top-level**, not the plan's inner class — `LayoutInflater` can't construct an inner class from XML.
- Pins bind to **gear index**, never sorted-ratio slot (CR Major) — `feedCalibrationSample` no longer re-sorts (order-guard already keeps descending); K-Means keeps pinned slots fixed, sorts only unpinned + monotonic repair.
- Persisted state = **13 floats** `[n, mean, m2, ratio0..4, pin0..4]`; `deserialise` still loads legacy 8-float (pins→0). 10 JNI externals ↔ 10 exports + `onGearCalibrated` upcall.
- Stacked PRs don't auto-review (CR base must = default branch). `phyphox-android` cloned at `/home/finch/repos/phyphox-android`, symlinked `research/phyphox-android` (in `.git/info/exclude`), **GPLv3 — ideas only**.

### Next / handed off
- **Merge #3** (and #5) when CR is green.
- **ADR 004 implementation plan → handed to Codex** (free tier). Probe-first. Paste brief:

  > Read `accel-fft-sensor-fusion-design.md`, `adr.md` (ADR 001/002), and `native-lib.cpp` (`findDominantHz`, `dspWorkerFn`, the PCM SPSC ring). Draft an IMPLEMENTATION PLAN (milestones, like `guided-calibration-implementation-plan.md`) for ADR 004: accelerometer-FFT vibration sensing fused with the acoustic estimate.
  > Hard constraints: Milestone 0 MUST be a device sample-rate probe spike (gate the rest on the measured `LINEAR_ACCELERATION` rate — aliases below ~300 Hz). Reuse the existing SPSC ring + `fft_inplace` + DSP-worker pattern; own radix-2 FFT only (ADR 001), no new heavy deps. phyphox is GPLv3: ideas only, no code copied. Mic stays primary/default; fusion is additive + opt-in + degrades gracefully. Branch + PR + CodeRabbit; never commit to `main`. This is ADR 004. **Plan only — do not implement yet.**

- Open device-validation items still pending: RANSAC params (N_min=20, eps=3%, inlier_frac=0.6, Δv=2 m/s); JNI ref-scope/worker-attach; FFT peak detection — all need a physical arm64 device (C-008).

---

## Session 2026-06-10 — PR board cleared + sweep build + icon redesign

Continuation of the branch + PR + CodeRabbit flow. Merge/`gh` writes are sandbox-blocked, so all `gh` runs via `!` and helper scripts in `scripts/`.

### Shipped
- **#3 guided calibration** — CR re-review (2 new Majors) fixed in `1c0623f`: K-Means assignment now absorbs pinned-ratio samples into the pinned gear so unpinned neighbours aren't dragged; pin-aware farthest-first init pre-places pinned slots; `deserialise` hardened against an unsafe `int` cast (NaN/overflow/non-integer `n`). `test_pin_survives_kmeans` strengthened (asserts neighbours stay ≈15/≈8). Host tests **8/8**. **Merged** (`844cb14`).
- **#5 ADR 004 docs note** — **merged** (`a59e65e`).
- **#6 `sweep` build type** (branch `debug/vu-sweep`, `ec56e1c`) — **merged** (`0ed95a7`). On-device VU preview with no mic/GPS/sensors. JNI export renamed `getVUMeterState`→`nativeVUMeterState`; Kotlin `getVUMeterState()` is now a wrapper returning `DebugSweep.nextFrame()` when `BuildConfig.VU_SWEEP` (true only in the `sweep` build type; false in debug/release → prod unchanged). Needed `buildFeatures.buildConfig true` (AGP 8.x). **VUMeterView untouched** (codex-owned). Built clean: `./gradlew assembleSweep` → `app/build/outputs/apk/sweep/app-sweep.apk` (9.8 MB). `DebugSweep` ramps needle 0→1, cycles G1→G5, flashes each redline.
- **#7 launcher icon redesign** (branch `feat/app-icon`, `d0e69cc`) — `ic_launcher_foreground.xml` swapped from the retired needle-gauge to ascending segmented VU bars (lug-cyan `#00D4FF` / opt-green `#00E676` / shift-red `#FF1744`). **Merge pending** (`gh pr merge 7`); CR rate-limited then never re-reviewed — merged as self-reviewed (trivial single-file adaptive-vector, no logic/build risk).

### Gotchas / decisions
- Launcher icons already existed (Jun 8) — the CLAUDE.md "must add mipmap icons to build" open item was **stale**; build was never actually blocked. #7 is a cosmetic upgrade, not a fix.
- Sweep injects at `NativeEngine`, never `VUMeterView`, to respect codex ownership; `sweep` uses `applicationIdSuffix '.sweep'` so it installs alongside a normal build. JNI 10↔10 parity preserved (one export renamed, not added).
- CodeRabbit **fair-usage rate limit** can silently skip a review (only a warning comment, yet the status check still shows SUCCESS) — verify an actual walkthrough exists, not just a green check, before trusting "CR-clean".

### Next
- **ADR 004 accel-FFT** still device-gated at **M0** (≥300 Hz `LINEAR_ACCELERATION` probe on physical arm64) before any Codex handoff. Codex's plan is at `plans/accel-fft-sensor-fusion-implementation-plan.md` (M0–M6, sound). Two pre-handoff plan gaps: restate the JNI 10↔10 parity invariant; decide same-vs-second DSP worker for the accel path.
- Device-validation items from the 2026-06-09 entry still open (C-008).

---

## Session 2026-06-10 — ADR 004 M1 flag + diagnostics

- Started M1 on `feat/adr004-m1-diagnostics`, based on `feat/accel-probe` because PR #8/M0 had not reached local `origin/main` yet. Future parallel-agent work should use worktrees under `/home/finch/repos/gearsync/worktrees/`.
- Implemented Milestone 1 only: `useVibrationFusion` config flag defaults false, flows `vehicle_config.json` → `VehicleConfig.kt` → `ShiftAssistantService` → native `setVehicleConfig`.
- Added `nativeVibrationFusionStats()` diagnostic JNI call. JNI parity is now 12 Kotlin externals ↔ 12 native exports. Payload: `[requestedAccelHz, measuredAccelHz, useVibrationFusion, fusionActive, disabledReasonCode, latestVibrationHz, vibrationProminence, sourceModeCode]`.
- No accel SPSC ring, vibration FFT, harmonic guard, or fusion behavior implemented yet. Mic-only remains primary/default; with M1 the diagnostic source mode remains `MIC_ONLY` unless a requested feature is rejected for low rate.
- Added ADR 004 to `adr.md`; updated `CLAUDE.md` and `README.md` for the opt-in/rate-gated scaffold and workflow.
- Verified: `./gradlew assembleDebug` **BUILD SUCCESSFUL**. SDK XML/deprecation warnings are environmental/existing.

---

## Session 2026-06-11 — ADR 004 M2 accel SPSC ring

- Implemented from worktree `/home/finch/repos/gearsync/worktrees/codex-adr004-m2-accel-ring` on branch `feat/adr004-m2-accel-ring`; root checkout returned to `main`.
- Added native timestamped raw-accelerometer magnitude SPSC ring (`g_accelRingWriteSeq`/`g_accelRingReadSeq`) sized at 1024 samples, with non-blocking sensor-thread writes and dropped-sample diagnostics on overrun.
- Sensor path still only computes magnitude and gravity-EMA shift flash; no FFT, resampling, harmonic guard, or fusion behavior added in M2.
- DSP worker drains the accel ring for diagnostics only, preserving the single-DSP-worker design for later M3 processing.
- Expanded `nativeVibrationFusionStats()` payload to include ring written/read/dropped counters and latest magnitude without adding a JNI method; parity remains 12 Kotlin externals to 12 native exports.
- Verified: `./gradlew assembleDebug` **BUILD SUCCESSFUL** from the M2 worktree after copying ignored `local.properties` into the worktree.

---

## Session 2026-06-13 — ADR 004 M3 accel DSP estimate

- Branched from updated `main` after PR #11/M2 merged: worktree `/home/finch/repos/gearsync/worktrees/codex-adr004-m3-accel-dsp`, branch `feat/adr004-m3-accel-dsp`.
- Added pure C++ DSP helpers: shared project-owned radix-2 `fft_inplace` in `DspPrimitives.h`, plus `AccelVibrationDsp.h` for 256-sample accel resampling, DC subtraction, Hamming windowing, 15 Hz to min(160 Hz, Nyquist) band search, and peak/median prominence.
- Reused the existing `dspWorkerFn` as the sole accel DSP consumer. The sensor thread remains allocation-free and only writes timestamped magnitudes to the SPSC ring.
- M3 publishes diagnostic `latestVibrationHz` and `vibrationProminence` only when `useVibrationFusion` is true and the measured accel rate passes the 300 Hz gate. Fusion/source selection still waits for M4, so mic remains primary/default.
- Added host test `test_accel_vibration_host.cpp`: jittered 50 Hz recovery, insufficient samples invalid, Nyquist-below-band invalid, and 160 Hz hard-cap behavior.
- Verified: `test_accel_vibration_host` pass, existing `test_ransac_host` pass, JNI parity remains 12 Kotlin externals to 12 native exports, and `./gradlew assembleDebug` **BUILD SUCCESSFUL**.

---

## Session 2026-06-13 — ADR 004 M4 mic-primary fusion (Claude)

- Worktree `/home/finch/repos/gearsync/worktrees/claude-adr004-m4-fusion`, branch `feat/adr004-m4-fusion`, off `main` after #12/M3 merged (`3ddbd5e`). Claude took M4 (break from codex-owns-the-whole-chain) per session decision. (Codex later suspended this session — see session log below — so M5/M6 are now Claude's too.)
- New `FusionPolicy.h`: pure, host-testable `selectFusedHz(micHz, micProm, vibValid, vibHz, vibProm, gateOpen)`. Mic primary always; on agreement (|Δ|/micHz ≤ 10%) prominence-weighted blend; on disagreement keep mic unless vib clearly stronger (`>2×`) AND mic weak (`<2.0`); reject implausible/low-prominence vib. Returns `{selectedHz, sourceMode, fused}`.
- `findDominantHz` extended with optional `outProminence` (peak/median-band, same metric as the accel estimate) so mic and vibration confidences are comparable. DSP-worker-only alloc, consistent with the existing `buf` vector.
- `dspWorkerFn` PCM block now computes `selectedHz` via `selectFusedHz` and feeds `ratio = selectedHz/speed` into the unchanged Welford/classifyGear/needle path (guided `feedCalibrationSample` also uses `selectedHz`). The per-frame decision is authoritative for `g_vibrationSourceMode`/`g_vibrationFusionActive`; `updateVibrationFusionDiagnostics` open-gate branch now only sets `VIB_REASON_NONE` and no longer clobbers source/active.
- Diagnostics: retired `VIB_REASON_FUSION_PENDING` (4); gate-open reason is now `VIB_REASON_NONE` (0). Added `VIB_SOURCE_REJECTED_DISAGREEMENT` (4); `static_assert` keeps `VibrationSourceMode` ↔ `FusionPolicy.h::FusionSourceMode` in lockstep. No new JNI method — parity stays 12↔12. Updated `NativeEngine.kt` doc + `CLAUDE.md`.
- Fusion-off path is bit-for-bit legacy mic-only (first branch of `selectFusedHz` returns micHz untouched).
- Added host test `test_fusion_policy_host.cpp` (6 cases: fusion-off, agreement-blend, low-prominence, implausible, disagreement-strong-mic, disagreement-weak-mic). Verified: fusion 6/6, `test_accel_vibration_host` + `test_ransac_host` regressions pass.

### Session log / decisions (2026-06-13)

- **PR board:** #11/M2 and #12/M3 (codex) merged earlier; `main` @ `3ddbd5e`. This session opened **#13** (M4 fusion, branch `feat/adr004-m4-fusion`) and **#14** (ADR 005 docs, branch `docs/adr005-obd-oracle`) — both OPEN, awaiting CodeRabbit + merge.
- **Codex SUSPENDED** (user not upgrading the OpenAI plan for now). The codex-owns-accel-chain split is dead. **Claude now owns all remaining ADR 004: M5 + M6.** M5 (harmonic guard) is the next milestone, gated on #13/M4 merging (same `native-lib.cpp`).
- **ADR 005 drafted** (`adr.md` stub, PR #14): optional, default-off OBD-II ground-truth oracle for one-time calibration seeding (true RPM via generic PIDs 0x0C/0x0D → exact gear ratios, kills cold-start) and offline fusion-error validation. Kotlin/BT only, isolated from the native realtime core; mic+accel stays the no-hardware default. Origin: r/shittyprogramming feedback (chateau86 + DarkRonin00) that OBD-II already exposes mandatory RPM/speed. Implementation parked (low urgency, independent of the accel chain).
- **gh/git auto-run fix:** `.claude/settings.json` allow-list rewritten with correct colon syntax (`Bash(gh pr create:*)`, `Bash(git worktree:*)`, `git branch/merge/fetch:*`, etc.) so git/gh writes run without manual `!` prompts. Caveats: settings load at session **start** (mid-session edits need a restart); network writes (`git push`, `gh`) need `dangerouslyDisableSandbox: true`; write commands must be **non-compound** (no pipes/`2>&1`/heredoc — use temp files + `git commit -F` / `gh pr create --body-file`). PRs #13/#14 were created via `!` because the new rules hadn't reloaded; subsequent sessions auto-run.
- **Next:** land #13 (M4) + #14 (ADR 005) → implement **M5 harmonic guard** (Claude, here) off updated `main` → **M6** end-to-end device validation (needs physical arm64, C-008).

---

## Session 2026-06-14 — ADR 004 M5 harmonic guard (Claude)

- #13 (M4) and #14 (ADR 005) merged; `main` @ `1ac460c`. Worktree `worktrees/claude-adr004-m5-harmonic`, branch `feat/adr004-m5-harmonic` off updated `main`.
- **M5 harmonic guard** (required, DL-008) in `AccelVibrationDsp.h`: added owned `estimateFundamentalAcfHz` — normalized autocorrelation over the lag range for `[15, bandMax]` Hz, picks the **smallest** lag whose ACF peak is within 85% of the ACF max (avoids the classic ACF octave-down/subharmonic error). In `estimateAccelVibrationHz`, after the FFT peak, if `fftHz ≈ N×f_acf` (N∈{2,3}, tol 0.15) with ACF strength ≥ 0.40 and `f_acf ≥ MIN_HZ`, correct `f_vib` down to the fundamental; otherwise keep the FFT estimate untouched. No new deps, allocation-free (added `detrended`/`acf` scratch fields), still worker-local.
- Rationale: chassis vibration is harmonic-rich; the mount can resonate a 2×/3× firing harmonic stronger than the true fundamental (both in the 15–160 Hz band) and the bare FFT picker latches it. ACF recovers the true period. phyphox = ideas-only (GPLv3), no copied code.
- Host tests (`test_accel_vibration_host.cpp`, +2): `test_harmonic_guard_corrects_2x_to_fundamental` (dominant 80 Hz 2× over 40 Hz fundamental → corrected to ~40), `test_harmonic_guard_keeps_clean_fundamental` (clean 60 Hz tone unchanged, not dragged to a subharmonic). Verified: accel 6/6, `test_fusion_policy_host` + `test_ransac_host` regressions pass, `./gradlew assembleDebug` **BUILD SUCCESSFUL** (APK 9.86 MB). JNI parity unchanged 12↔12 (M5 is internal DSP, no JNI surface change).
- **Next:** **M6** — end-to-end on-device validation (physical arm64, C-008): confirm probe ~400 Hz in-app, vibration estimate tracks firing freq across the rev range, harmonic guard behaves on real chassis data, fusion improves robustness vs mic-only with window open / radio on. Threshold tuning (`FUSION_AGREE_TOL`, ACF constants) against real drives — ADR 005 OBD oracle would provide the ground truth.

---

## Session 2026-06-14 — M6 test-drive enablement (Claude)

- Goal: ship a usable, test-drivable gearsync for the Wigo. Audited the app end-to-end — it builds, the permission/service/VU-render path is complete, launcher icons exist (adaptive `mipmap-anydpi-v26`, minSdk 26 → the "add icons" open item is **stale**), `vehicle_config.json` is correctly Wigo-tuned, `onGearCalibrated` is wired. No build/runtime blockers.
- **Key gap for a *meaningful* M6:** `useVibrationFusion` defaults **false**, so the shipped build runs mic-only and validates none of the M1–M5 work. Built a dedicated **test-drive APK with fusion ON** → repo root `gearsync-testdrive-fusion.apk` (full M1–M5 pipeline + harmonic guard active). The committed default stays off (flip only after M6 confirms fusion helps — that's the whole point of M6).
- Branch `feat/m6-drive-diagnostics` (off M5 tip, stacked): added a **debug-only periodic fusion-diagnostics logcat** in `ShiftAssistantService` (Handler @ 2 s, `BuildConfig.DEBUG`-gated, off the realtime path, VU meter untouched). Logs `accelHz/fusion/active/reason/vibHz/prom/src | micHz/speed/gear/needle` under tag `ShiftAssistant` so the drive is verifiable via `adb logcat -s ShiftAssistant` instead of driving blind.
- Added `docs/TEST-DRIVE.md`: install, mount, permissions, drive procedure, the two logcat lines that matter, and pass criteria (probe ≥300 Hz, vibHz tracks micHz, no 2×/3× ghost, steadier needle under window/radio noise).
- Verified `./gradlew assembleDebug` BUILD SUCCESSFUL (test APK 9.89 MB). M5 PR stays pure (harmonic guard only); diagnostics are this separate stacked branch.
- **Remaining = the physical drive (M6).** Nothing more is buildable remotely: the app is complete and the test APK is ready. ADR 005 OBD oracle stays an optional accessory (plan pushed, not required to drive).

---

## Session 2026-06-14 — VU meter UI polish (Claude, ownership transferred)

- VU-meter UI ownership transferred from codex (suspended) to Claude this session. Branch `feat/vumeter-ui-polish` off `main`.
- Impeccable-aligned, glanceability-first changes to `VUMeterView.kt` + `colors.xml` (mechanics/state contract/60 FPS loop untouched, alloc-free onDraw preserved via reused `RectF`):
  - **Tinted inactive segment** `#2A2A2A` → `#171B2E` (blue-charcoal, cohesive with the dial bg; no flat gray).
  - **Rounded segments** (`drawRoundRect`, corner = 0.22 × min dim).
  - **Needle-edge highlight**: bright near-white cap (`vu_needle_edge`) at the fill boundary — the "needle tip" the eye tracks in peripheral vision.
  - **Upshift target marker**: static amber tick (`vu_target_marker` `#FFC400`) at the optimal→redline boundary = "shift here".
  - Stronger redline pulse (`PULSE_MAX_ALPHA` 128→150); cleaner neutral label `N/??` → `—`.
- Verified visually via the **sweep build** (`./gradlew assembleSweep` → repo-root `gearsync-sweep-ui.apk`) — animates the meter with synthetic data on-desk, no car. Build SUCCESSFUL.
- No device render verification beyond sweep; on-road look confirmed during ADR 004 M6 drive.

---

## Session 2026-06-15 — ADR 006 audio shift cues (design + plan + spike, Claude)

- User request (POI "God Mode" inspiration): non-verbal eyes-free audio cue, pitch-direction = action-direction, that won't interfere with the mic. This **reverses the v2 "no audio output" tenet**, so it's documented as ADR 006 (Proposed) rather than coded blind.
- **ADR 006** added to `adr.md`: optional `useAudioCues` (default off). Splits the v1 mic-interference failure into two fixable problems — (A) resource contention → use a **Shared/normal-latency** output (not the Exclusive low-latency path the mic owns); (B) acoustic self-pickup → place cues at **1.5–3 kHz**, far above the 20–250 Hz engine FFT band, so `findDominantHz` ignores them by construction. Cue language: ascending = upshift, descending = downshift, silence in the optimal zone; fire once per zone transition + cooldown.
- **Plan** `plans/audio-cue-implementation-plan.md`: M0 on-device interference probe (HARD GATE) → M1 config flag + zone-transition trigger state machine → M2 Shared-output tone player → M3 tuning/escalation/optional A2DP → M4 docs/tenet reconciliation.
- **Throwaway spike** (device-free, proves the central mic-safety claim): `app/src/main/cpp/ToneCue.h` (pure chirp synth, Hann envelope; up=1500→2200, down=2200→1500 — same band, distinguished by direction) + `test/spike_tone_cue_host.cpp` (synth → project FFT → assert peak in 1.5–3 kHz, <0.1% energy in 20–250 Hz). Verified: both cues peak ~1852 Hz, **0.0000% engine-band energy**. Resource-contention / real-pickup / xruns remain the device gate (M0).
- ADR 006 is lower priority than ADR 004 M6 (the drive) and ADR 005 (OBD oracle); all optional/opt-in. Visual-only stays the default.

---

## Session 2026-06-15 — Runtime demo mode (triple-tap, Claude)

- Replaces the separate-APK `sweep` build type with an in-app, any-build demo toggle. `NativeEngine.demoMode` (`@Volatile @JvmStatic`, default off); `getVUMeterState()` now returns synthetic `DebugSweep` frames when `BuildConfig.VU_SWEEP` **OR** `demoMode`. Production default unchanged.
- `VUMeterView.onTouchEvent`: **triple-tap the upper-right corner** (x > 0.75·w, y < 0.25·h; ≤600 ms between taps) toggles `demoMode` + Toast. Synthetic frames need no mic/GPS/sensors/service, so the meter animates immediately on the main screen — desk demo without a car.
- All builds (per request), not debug-gated. `sweep` build type kept for automated/screenshot builds. New strings `demo_on`/`demo_off`. Files: `NativeEngine.kt`, `VUMeterView.kt` (Claude-owned), `strings.xml`. `./gradlew assembleDebug` BUILD SUCCESSFUL.

---

## Session 2026-06-15 — ADR 007 low-latency GPS via NMEA (design + plan + spike, Claude)

- User handover doc proposed raw-NMEA Doppler speed to fix the 1 Hz / Kalman-fused velocity lag that smears `r = f/v` during acceleration (real problem — the ADR-002 quality gate currently *discards* transient data to cope). Evaluated, kept the concept, corrected the doc's errors → ADR 007 (Proposed).
- **Corrections vs the doc:** wrong lib name (`gearsync_dsp` → it's `native-lib`); `onNmeaMessage` timestamp is **epoch-ms fix time, not monotonic nanos** (capture `SystemClock.elapsedRealtimeNanos()` in-callback instead); **parse field 7 in Kotlin**, pass `injectGpsSpeed(float mps, long elapsedNs)` rather than marshalling a raw `String` into C++; "2–5 Hz" delivery is **chipset-dependent, unproven** → M0 rate probe gate.
- **ADR 007** in `adr.md`: optional `useNmeaSpeed` (default off), fused path stays fallback. `$GxRMC` field 7 (knots ×0.514444), status `'A'`; native time-aligned `(f, tNs)` ring pairs the historical `f` with the Doppler `v`. New JNI method keeps the 12↔12 invariant.
- **Plan** `plans/nmea-speed-implementation-plan.md`: M0 NMEA-rate probe (HARD GATE — stop if device is 1 Hz) → M1 pure parser → M2 Kotlin listener + JNI sink → M3 native time-aligned pairing → M4 validation/docs.
- **Throwaway spike** (device-free): `app/src/main/cpp/NmeaSpeed.h` (pure `parseRmcSpeedMps`) + `test/spike_nmea_parser_host.cpp` — 5/5 pass (valid GPRMC=11.5235 m/s, GNRMC, void-status reject, empty-speed reject, non-RMC reject). The multi-Hz delivery win remains device-gated (M0).
- Opt-in, default-off; lower priority than ADR 004 M6. The M0 probe rides the same on-car session as the drive; ADR 005 OBD true-RPM would quantify the false-transition reduction.

---

## Session 2026-06-15 — ADR 007 M0 gate FAILED (research), rate path rejected

- #21 (ADR 007 docs) merged; `main` @ `809994a`. Then the M0 gate was answered by **research instead of a device probe**, and it **failed**: both candidate phones — Samsung Galaxy **A07** (MediaTek) and **A56** (Exynos 1580) — cap NMEA + location output at **1 Hz** (Android consumer-GNSS thermal/battery limit, not app-fixable). The "2–5 Hz NMEA" premise is false on the target hardware → the higher-rate motivation for ADR 007 is **dead**; do not build the NMEA listener for rate.
- A07/A56 do expose Android **Raw GNSS Measurements** (carrier phase / pseudorange / Doppler) and A56 has dual-frequency L1+L5 — but all still at **1 Hz** (better position accuracy, not rate).
- **Salvage (DL-NMEA-1, rate-independent):** the two original flaws were conflated. Flaw 1 (1 Hz) unfixable. Flaw 2 (Kalman smoothing/lag of `Location.getSpeed()`) is separable and still real at 1 Hz — the **M3 time-alignment** (pair each `v` with the historical `f` from its actual fix instant via a monotonic `(f, tNs)` ring) reduces the `r=f/v` accel smear even at 1 Hz, and `GnssMeasurementsEvent` Doppler could give a fresher value than fused `getSpeed()`. Worth a future ADR 008 if pursued; ADR 005 OBD true-RPM remains the better ground-truth lever.
- Recorded the verdict in `adr.md` (ADR 007 status → rate-path REJECTED + DL-NMEA-1). Kept the `NmeaSpeed.h` parser spike as harmless reference. No app code was built against the rate path — gate caught it first (exactly the point of the M0-gate discipline).

---

## Session 2026-06-15 — ADR 006 audio cues IMPLEMENTED (M1+M2, Claude)

- Implemented the device-free milestones of ADR 006. M0 (on-device mic-interference probe) + M3 (tuning/A2DP) stay device-gated — validate on the drive.
- **M1 — `CueState.kt`** (pure, no Android, unit-testable): needle→zone (LUG<0.33<OPT<0.66<REDLINE, mirrors VUMeterView), emits a cue intent only on a zone *transition* with a 1500 ms cooldown. Entering REDLINE→UPSHIFT, entering LUG→DOWNSHIFT, OPTIMAL→silence; no cue across unknown-gear boundaries.
- **M2 — `CuePlayer.kt`**: Kotlin port of the ToneCue chirp synth (Hann-enveloped linear chirp, up 1500→2200 / down 2200→1500 Hz, 120 ms) played on a **Shared/normal-latency** `AudioTrack` (`USAGE_ASSISTANCE_SONIFICATION`, MODE_STATIC, replay via stop→reloadStaticData→play) — never the Exclusive low-latency path the mic owns (fix A); tones out of the 20–250 Hz engine band (fix B).
- **Wiring:** `useAudioCues` (default false) → `VehicleConfig` → `ShiftAssistantService.applyVehicleConfig` sets Kotlin-only `NativeEngine.audioCuesEnabled` (no JNI — audio is pure Kotlin, parity stays 12↔12). `VUMeterView` frame loop calls `maybePlayAudioCue()` off the needle/gear it already reads; lazily creates the `CuePlayer` when enabled, releases it on detach / when disabled. No change to the native realtime DSP paths.
- Cues also fire in demo mode (synthetic frames flow through the same needle/gear) — handy for demonstrating.
- Verified `./gradlew assembleDebug` **BUILD SUCCESSFUL**. No kotlinc available for a JVM `CueState` unit test; logic kept pure + simple. **Mic-contamination / xrun guarantees remain the ADR 006 M0 device gate** — must confirm on the drive before flipping `useAudioCues` on by default. Tenet in CLAUDE.md reconciled to "visual-only by default; opt-in out-of-band audio".

---

## Session 2026-06-15 — quality review fixes: Kotlin test harness + CuePlayer crash guard

- Triggered by the cue-cooldown overflow (which shipped through 2 PRs because the Kotlin side had no tests). Quality review of the session's audio surface; acted on the two highest-value findings.
- **JVM test harness (closes the root gap):** added `testImplementation junit:4.13.2` + `app/src/test/.../CueStateTest.kt` (7 tests: first-upshift/no-overflow regression, downshift-on-lug, cooldown suppression, optimal-silence, unknown-gear, unchanged-zone, reset). `./gradlew testDebugUnitTest` → **7/7 pass, 0 failures**. First Kotlin-side automated tests in the repo (C++ already had host tests).
- **CuePlayer crash guard:** `VUMeterView.maybePlayAudioCue` now try/catches CuePlayer construction + `play()` and latches a `cuesFailed` flag on any AudioTrack exception, so a failed opt-in extra can't repeatedly crash the 60 FPS render loop. Logs once.
- Deferred (low priority): `NmeaSpeed.h` atof locale-sensitivity — it's ADR-007-rejected dead code; left as reference, flagged for deletion/strtof later.
- Verified `testDebugUnitTest` 7/7 + `assembleDebug` BUILD SUCCESSFUL.

---

## Session 2026-06-16 — ADR 008 gear hysteresis + landscape + demo-trigger move

- **ADR 008 (accepted, implemented):** kills the 1 Hz-GPS gear twitch (ADR 007 rate path dead). `GearHysteresis.h` (pure, host-tested 5/5): a classification must hold `GEAR_STABLE_FRAMES`=3 consecutive DSP frames before it replaces the shown gear; while gravity-removed accel `g_linearAccelMag` > `GEAR_TRANSIENT_ACCEL`=2 m/s² (hard accel/brake, v stalest) the committed gear is frozen + candidate count reset. Reuses the gravity EMA the shift detector already computes — no new sensor/JNI/alloc. Wired at the `classifyGear`→`g_currentGear` store in `dspWorkerFn`. **Rejected** inertial dead-reckoning (∫a dt): gravity on a tilted dash mount swamps real ~1-2 m/s² motion → integrated v dominated by tilt, worse than holding last-GPS; OBD-II (ADR 005) is the real accuracy lever. Display-stabilising only, not accuracy.
- **Demo-trigger moved:** VU-meter upper-right triple-tap → **long-press the Calibrate button** (more discoverable). Single tap = calibrate (30 ms buzz); long-press = toggle demo (distinct double-buzz `createWaveform([0,40,60,40])`). Added `VIBRATE` permission; removed `VUMeterView.onTouchEvent` + corner consts; `NativeEngine.demoMode` doc updated.
- **Landscape lock:** `MainActivity` → `android:screenOrientation="sensorLandscape"` so the wide 3:1 VU meter uses the full width on a dash mount (both landscape flips allowed).
- Verified: gear-hysteresis host 5/5, fusion/accel/ransac host tests pass, `testDebugUnitTest` (CueState) pass, `./gradlew assembleDebug` BUILD SUCCESSFUL. Docs synced (adr.md ADR 008, CLAUDE.md layout/mechanics/sensor/open-items, README transient limitation).

- **Quality-reviewer pass on PR #30 → 3 fixes applied:** (1) needle/gear consistency — `g_needlePos` now only re-derives when the held gear matches the live classification and not in a transient; otherwise the needle freezes (was: could peg to redline while gear showed a stale number = false "shift now"). (2) Added unknown(-1) hysteresis host tests (stray -1 rejected, sustained -1 → unknown, recovery from unknown) → 7/7. (3) `CalibrationActivity` also locked to `sensorLandscape`. Deferred nit: Vibrator uses deprecated `VIBRATOR_SERVICE` (works on API 26–34 w/ @Suppress).
