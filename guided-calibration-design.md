# Design Note — Optional Guided Per-Gear Calibration (RANSAC)

**Status:** Proposed (design only — not yet implemented)
**Date:** 2026-06-08
**Relates to:** ADR 002 (Local-First Edge ML), README "Shift Decision Logic", open items in `session-notes.md`

---

## 1. Motivation

Today calibration is **fully automatic and passive** (see `ShiftAssistantService.onCreate` → `applyVehicleConfig` → `seedCentroids`, then `runKMeans` over incidental steady-state samples). This is good default UX: the app works from the first drive using theoretical `k_g` seeds from `vehicle_config.json`, and silently refines.

Passive auto-calibration has three structural weaknesses:

1. **No gear targeting.** K-Means clusters whatever ratios arrive; it cannot be told "these samples are 3rd gear." Mislabeled or sparse gears converge slowly or not at all.
2. **Slow convergence.** Refinement depends on the driver *happening* to cruise at steady state in each gear across many drives.
3. **No supervised recovery.** After a tire change or pressure shift, the driver cannot deliberately re-fit one gear without disturbing the rest.

**Guided calibration** is an *optional, additive* mode that fixes all three. The driver selects a gear, holds it across a range of steady speeds for ~30–60 s, and the engine fits that gear's slope directly. **Auto-calibration remains the default and is never removed.**

---

## 2. Why RANSAC line-through-origin (not Welford-on-ratio)

Within one gear, `f = k_g · v` is a line through the origin. Guided capture *encourages varied steady speeds* in the chosen gear, producing `(v, f)` points spanning a speed range. The right estimator is a robust slope fit, not a mean of instantaneous ratios:

- **RANSAC** repeatedly samples a minimal subset (here a single `(v, f)` point defines a candidate slope `k = f/v`), counts inliers within a residual band `|f − k·v| ≤ ε`, and keeps the slope with the most inliers. A final least-squares refit over inliers gives the locked `k_g`.
- This **rejects outliers** that steady-state gating alone misses: clutch slip (f drops, v steady), a pothole jolt, a momentary FFT mis-peak, a gear-change caught mid-window.
- Welford-on-ratio (the alternative) only works if speed is held *constant*; it has no notion of inliers and is biased by any slip sample inside the gate. RANSAC degrades gracefully when the driver's speed wanders.

This matches ADR 002's stated intent ("RANSAC linear regression to reject structural noise").

### RANSAC parameters (initial, tune on device)

| Param | Symbol | Initial | Notes |
|---|---|---|---|
| Min inliers to accept a fit | `N_min` | 20 | gated `(v,f)` pairs |
| Residual threshold | `ε` | `0.03 · f` | 3% of frequency, relative band |
| Iterations | `K` | 100 | cheap; each iter is O(n) inlier count |
| Min speed spread | `Δv_min` | 2.0 m/s | reject "held one speed" — needs a range for a stable slope |
| Inlier fraction to lock | — | ≥ 0.6 | guards against a noisy capture locking garbage |

---

## 3. State machine (C++ `CalibrationEngine`)

New mode, orthogonal to the existing passive path:

```
            beginGearCalibration(g)
   IDLE ───────────────────────────────► CAPTURING(g)
    ▲                                      │  feeds gated (v,f) into m_capture[g]
    │  onGearCalibrated(g)  (success)      │
    │  ◄───────────────────────────────────┤  when N_min inliers & Δv spread met:
    │                                       │     RANSAC fit → m_gearRatios[g] = k_g
    │  cancelCalibration()  (abort)         │     re-sort centroids desc, persist
    └───────────────────────────────────────┘
```

- While `CAPTURING(g)`, the DSP worker routes each **gated** `(speed, hz)` pair (same stability gate as today, `native-lib.cpp:159`) into a per-gear capture buffer instead of / in addition to `updateWelford`.
- Passive K-Means is **paused** during capture to avoid double-moving the centroid.
- On lock: overwrite `m_gearRatios[g]`, keep array sorted descending (re-sort + guard: a guided `k_g` that would break monotonic ordering vs neighbors is rejected, callback reports failure).
- `m_calibrated` stays true throughout (seeds already valid); guided lock just sharpens one centroid.

### New `CalibrationEngine` surface

```cpp
// Mode control
void beginGearCalibration(int gearIndex);   // 0-based; clears m_capture[g], enters CAPTURING
void cancelCalibration();                   // back to IDLE, discard capture
bool isCalibrating() const;
int  calibratingGear() const;               // -1 when IDLE

// Capture feed (called from DSP worker while CAPTURING)
// Returns true when this sample completed a successful lock.
bool feedCalibrationSample(float speed, float hz);

// Progress for UI ring [0,1] = min(inlierCount / N_min, spreadProgress)
float calibrationProgress() const;

private:
struct GearCapture { std::vector<float> v, f; };
GearCapture m_capture;                 // only the gear under capture
int         m_calibGear = -1;          // -1 = IDLE
// RANSAC over m_capture → slope; sets m_gearRatios[g] on success
bool ransacFit(const GearCapture&, float& kOut) const;
```

Serialisation unchanged — guided mode is transient; only the resulting `m_gearRatios` persist via the existing 8-float `serialise`.

---

## 4. JNI surface (3 new bindings)

Extends the table in `CLAUDE.md`. Current: 7 externals. New total: **10** Kt→C++/C++→Kt.

| Method | Direction | Caller | Freq |
|---|---|---|---|
| `beginGearCalibration(int gear)` | Kt→C++ | Calibration UI | once per capture |
| `cancelCalibration()` | Kt→C++ | UI back / abort | once |
| `getCalibrationProgress(): Float` | C++→Kt | Calibration UI poll | ~10 Hz |
| **`onGearCalibrated(int gear)`** | **C++→Kt** | DSP worker on lock | once per lock |

### The callback is the only hard part

`onGearCalibrated` fires from the **DSP worker thread**, which is *not* JVM-attached. Required plumbing (standard JNI, but easy to get wrong):

1. Cache `JavaVM*` in `JNI_OnLoad` (`env->GetJavaVM(&g_jvm)`).
2. In `startEngine`, cache a **global** ref to the `NativeEngine` jclass and the `onGearCalibrated` `jmethodID` (static method). `NewGlobalRef` — local refs don't survive the callback.
3. In the DSP worker at lock time: `g_jvm->AttachCurrentThread(&env, nullptr)` → `CallStaticVoidMethod(cls, mid, gear)` → `DetachCurrentThread()`. Or attach the worker once at thread start and detach at exit (cheaper; preferred).
4. Kotlin: add `@JvmStatic fun onGearCalibrated(gear: Int)` to `NativeEngine` — must be kept by ProGuard (already covered by the `-keep class …NativeEngine { *; }` rule). Posts to the UI via a listener/handler set by the calibration Activity.

**Thread-safety:** lock acquisition in the new methods reuses `m_mutex`. `feedCalibrationSample` runs on the DSP worker; `begin/cancel/progress` run on UI/binder threads — all must take `m_mutex`. The callback itself must fire *outside* the mutex (release before `CallStaticVoidMethod`) to avoid holding the lock across a JVM upcall.

---

## 5. Kotlin / UI flow

New `CalibrationActivity` (or a mode in `MainActivity`), launched from a button. Reuses `VUMeterView`'s Canvas/Choreographer patterns for the progress ring.

```
[ Start Calibration ]
        │
        ▼
 Pick gear:  1  2  3  4  5      ← grid; greyed if currently captured
        │  beginGearCalibration(g)
        ▼
 "Hold gear N. Vary speed 30–70 km/h, steady throttle."
   ◐ progress ring fills  ← getCalibrationProgress() @ 10 Hz via Choreographer
   [ Cancel ] → cancelCalibration()
        │
        ▼  onGearCalibrated(g)
 "Gear N locked ✓"  → return to gear grid (that gear now marked done)
```

- Ring fill = `getCalibrationProgress()`; reuse zone colors from `colors.xml`.
- Lock event = visual confirm (no audio — v2 is visual-only). Pulse the ring green, optional haptic (`VibrationEffect`).
- All strings → `strings.xml`, colors → `colors.xml` (project convention).
- Activity must hold `RECORD_AUDIO` + location already granted (gate before entry, same as `MainActivity`).

---

## 6. Persistence & interaction with auto-calibration

- Guided lock writes `m_gearRatios[g]`, persisted by the existing `saveCalibrationState` path — no schema change.
- **After** a guided lock, passive K-Means resumes and may keep nudging that centroid. Option (decide at impl): mark guided-locked gears as "pinned" (skip them in `runKMeans` reassignment) so a deliberate calibration isn't washed out by later incidental noise. Pinning needs 5 extra persisted flags → would bump state array from 8 → 13 floats (back-compat: pad-and-default on deserialise).
- `reset()` clears guided locks too.

---

## 7. Scope / risk

| Layer | Effort | Risk |
|---|---|---|
| C++ state machine + RANSAC | ~80–110 LOC | Low — pure, unit-testable on host |
| JNI begin/cancel/progress | ~30 LOC | Low |
| `onGearCalibrated` callback (JavaVM/global ref/attach) | ~25 LOC | **Medium** — wrong ref scope or missing detach = crash; test on device |
| Calibration UI | ~150 LOC + layout | Low–Med — new Activity, permission gating |
| Pinning (optional) | state array 8→13, deserialise back-compat | Med — persistence migration |

**Recommended build order:** (1) C++ RANSAC + mode, host-test the fit; (2) begin/cancel/progress JNI + a temporary log-only "lock" to validate capture; (3) `onGearCalibrated` callback wiring; (4) UI; (5) optional pinning.

---

## 8. Open questions

- **Pin guided gears** against later K-Means drift? (state migration cost vs. fidelity)
- **Speed-range coaching:** enforce `Δv_min` spread, or just fill slower if the driver holds one speed?
- **Partial captures:** persist an in-progress capture across an app kill, or require one continuous session? (Lean: in-session only — matches "deliberate" framing.)
- Reuse `MainActivity` with a mode flag vs. a dedicated `CalibrationActivity`? (Dedicated = cleaner lifecycle/permission story.)
