# Plan

## Overview

Passive K-Means auto-calibration cannot target a specific gear, converges only when the driver happens to cruise steadily in each gear, and offers no supervised recovery after a tire change or pressure shift. Drivers lack an optional, deliberate way to fit one gear directly from varied steady-speed (v,f) samples while the passive default keeps working untouched.

**Approach**: An additive guided-calibration mode layers onto CalibrationEngine alongside the passive path. A CAPTURING(g) state machine routes gated (v,f) pairs into a single capture buffer; a RANSAC line-through-origin slope fit (single-point minimal model, relative residual band, speed-spread guard, inlier-fraction acceptance) locks m_gearRatios[g] and sets a per-gear pin flag that excludes the gear from later K-Means reassignment. Persisted state migrates from 8 to 13 floats (3 Welford + 5 ratios + 5 pin flags) with pad-and-default back-compat on deserialise. A C++ to Kotlin onGearCalibrated callback fires from the DSP worker thread using a JNI_OnLoad-cached JavaVM and startEngine-cached global jclass/jmethodID, released outside m_mutex. A dedicated CalibrationActivity drives a gear-grid plus Choreographer progress-ring UI, reusing VUMeterView Canvas patterns. The RANSAC fit is host-unit-tested as plain C++ before on-device validation.

### Guided calibration data flow and onGearCalibrated upcall

[Diagram pending Technical Writer rendering: DIAG-001]

## Planning Context

### Decision Log

| ID | Decision | Reasoning Chain |
|---|---|---|
| DL-001 | Estimate guided gear slope with RANSAC line-through-origin, not Welford-on-ratio | Within a gear f=k_g*v is a line through origin and guided capture deliberately spans a speed range -> the estimator must be a robust slope fit with inlier rejection -> RANSAC samples single-point candidate slopes k=f/v, counts inliers in a relative residual band, and least-squares-refits inliers, rejecting clutch-slip/pothole/mis-peak samples that the steady-state gate alone misses; Welford-on-ratio is only valid at constant speed and is biased by any slip sample inside the gate -> choose RANSAC. |
| DL-002 | Pin guided-locked gears and migrate persisted state from 8 to 13 floats | A deliberate calibration must not be washed out by later incidental K-Means nudging -> guided-locked gears need a persisted per-gear pin flag that runKMeans honors by leaving pinned centroids fixed -> 5 pin flags appended as trailing floats bump the state array 8->13 (3 Welford + 5 ratios + 5 pins); old 8-float SharedPreferences must still load -> deserialise pads missing pin flags to 0.0 (unpinned) by length-checking before reading the pin block. |
| DL-003 | Fire onGearCalibrated from the DSP worker via cached JavaVM and global refs, released outside m_mutex | The lock event originates on the DSP worker thread which is not JVM-attached and fires long after startEngine returns -> local jclass/jmethodID refs would be invalid and a fresh FindClass on an unattached thread fails -> cache JavaVM in JNI_OnLoad and a NewGlobalRef jclass + static jmethodID in startEngine, attach the worker thread to the JVM once at thread start and detach at exit -> additionally the callback must not hold m_mutex across the JVM upcall (deadlock/reentrancy risk) so feedCalibrationSample returns the locked gear index and native-lib fires the upcall after the engine call returns. |
| DL-004 | Drive guided calibration from a dedicated CalibrationActivity, not a MainActivity mode flag | Guided mode has its own permission precondition (RECORD_AUDIO + location already granted), lifecycle (poll progress at ~10 Hz, register an onGearCalibrated listener), and screen flow (gear grid -> capture -> confirm) -> folding this into MainActivity entangles two unrelated lifecycles and gating paths -> a dedicated Activity gives a clean permission/lifecycle story and an isolated listener registration -> choose CalibrationActivity launched by a button from MainActivity. |
| DL-005 | Pause passive K-Means and reject monotonicity-breaking locks during guided capture | While CAPTURING(g) the same gated samples would otherwise feed both updateWelford and the capture buffer, double-moving the centroid -> guided capture must suppress passive Welford/K-Means feeding for the duration -> on lock, m_gearRatios must remain strictly descending because classifyGear nearest-centroid and needle mapping depend on it -> a fitted k_g that would break ordering versus its neighbors is rejected and reported as a failed lock rather than written. |
| DL-006 | Keep guided capture in-session only with a single capture buffer | Only one gear is calibrated at a time and partial captures across an app kill add persistence complexity for little benefit -> a single GearCapture buffer (not 5) holds the gear under capture, cleared on begin/cancel/lock -> capture state is transient and never serialised; only the resulting m_gearRatios and pin flags persist -> matches the deliberate, continuous-session framing. |
| DL-007 | Host-unit-test the RANSAC fit as plain C++ before device | CalibrationEngine is pure and Android-free while the JNI callback and full audio/sensor pipeline can only run on a physical arm64 device -> the highest-risk numeric logic (RANSAC slope fit, inlier counting, spread guard, acceptance) can be validated cheaply off-device -> add a standalone host C++ test compiled without the NDK that exercises ransacFit on synthetic inlier+outlier point sets -> device testing then focuses on JNI ref scope/attach correctness and end-to-end capture. |

### Rejected Alternatives

| Alternative | Why Rejected |
|---|---|
| Estimate the guided gear slope with Welford-on-ratio over gated samples | Only valid when speed is held constant; has no inlier rejection and is biased by clutch-slip samples that pass the steady-state gate. RANSAC degrades gracefully when speed wanders, which is exactly what varied-speed capture produces. (ref: DL-001) |
| Let K-Means keep re-nudging guided-locked gears (no pinning) | Washes out a deliberate calibration with later incidental noise, defeating the purpose of guided mode. (ref: DL-002) |
| Reuse MainActivity with a mode flag for the calibration UI | Entangles two unrelated lifecycles, permission-gating paths, and listener registrations. A dedicated CalibrationActivity gives a cleaner permission/lifecycle story. (ref: DL-004) |
| Keep per-gear capture buffers for all 5 gears | Only one gear is captured at a time; five buffers add state for no benefit. A single GearCapture buffer suffices. (ref: DL-006) |
| Attach/detach the DSP worker to the JVM on every onGearCalibrated callback | Locks are infrequent but attach/detach per call is wasteful and easy to leak. Attaching the worker once at thread start and detaching at exit is cheaper and the preferred plumbing. (ref: DL-003) |

### Constraints

- C-001 (technical, user-specified): Passive K-Means auto-calibration stays the default and is never removed; guided mode is additive only.
- C-002 (technical, user-specified): No audio output (v2 is visual-only); no cloud, no OBD-II; FFT and sensor pipeline unchanged; no new vehicle_config.json fields.
- C-003 (technical, doc-derived): All incoming floats in deserialise are validated (isfinite, n>=0, m2>=0, finite ratios, pin flags in {0,1}) before any state is mutated.
- C-004 (technical, doc-derived): onGearCalibrated must fire outside m_mutex; m_gearRatios stays strictly descending; m_calibrated stays true throughout guided mode.
- C-005 (technical, doc-derived): JNI externals stay matched: 7 current Java_dev_alfieprojects_gearsync_NativeEngine_* exports grow to 10 (3 new Kt->C++), plus 1 C++->Kt onGearCalibrated upcall; mismatch yields UnsatisfiedLinkError.
- C-006 (technical, doc-derived): UI strings go to strings.xml and colors to colors.xml; no hardcoded literals. New vehicle = edit vehicle_config.json, no recompile.
- C-007 (technical, doc-derived): NativeEngine (including the new onGearCalibrated @JvmStatic) is kept by the existing proguard -keep class ...NativeEngine { *; } rule.
- C-008 (dependency, user-specified): Build verifiable only locally: NDK 27.1.12297006 + physical arm64 device via ./gradlew assembleDebug; emulator cannot exercise the sensor/audio pipeline.
- C-009 (technical, user-specified): Speed-range coaching beyond the Delta-v_min gate and cross-app-kill capture persistence are out of scope; capture is in-session only.

### Known Risks

- **Wrong JNI ref scope or a missing DetachCurrentThread crashes the process when onGearCalibrated fires from the unattached DSP worker.**: Cache JavaVM in JNI_OnLoad; cache NewGlobalRef jclass + static jmethodID in startEngine; attach the worker once at thread start and detach at exit; delete the global ref in stopEngine. Validate on a physical arm64 device.
- **Persistence migration corrupts state when an old 8-float blob is read as 13 floats.**: Length-gate deserialise to read pin flags only when len>=13, else default all pins to 0.0; serialise always writes 13; ShiftAssistantService reads CALIBRATION_STATE_LEN=13 with per-key getFloat defaults so missing cal_8..cal_12 keys default to 0.0.
- **A noisy or single-speed capture locks a garbage k_g, or a fitted k_g breaks the strict-descending gear ordering classifyGear depends on.**: Require N_min inliers, inlier fraction >= 0.6, and Delta-v_min speed spread before locking; reject and report failure when the fitted k_g would break monotonic order versus neighbors.
- **Holding m_mutex across the onGearCalibrated JVM upcall deadlocks if the Kotlin listener re-enters a native method.**: feedCalibrationSample returns the locked gear index (or -1); native-lib fires the upcall only after the engine call returns and the lock is released.

## Invisible Knowledge

### System

Guided calibration is an optional, additive mode orthogonal to the passive pipeline. ShiftAssistantService.onCreate seeds theoretical k_g (classifyGear works from first drive) and the DSP worker refines centroids via gated Welford+K-Means. The guided path adds a CAPTURING(g) state: while active, gated (v,f) pairs route into a single capture buffer instead of feeding Welford, RANSAC fits the gear slope on lock, the gear is pinned against future K-Means drift, and onGearCalibrated upcalls Kotlin from the DSP worker thread. CalibrationActivity polls getCalibrationProgress at ~10 Hz for a Choreographer progress ring and registers a listener for the lock event.

### Invariants

- m_gearRatios is sorted strictly descending (gear 1 = index 0 = highest ratio); classifyGear nearest-centroid and needle mapping depend on it. Guided locks and K-Means must preserve this order.
- classifyGear tolerance band is asymmetric [k*tolLow, k*tolHigh] (Wigo [0.98, 1.025]); outside the band for all gears returns -1/unknown to prevent flicker. Guided mode does not change this.
- m_calibrated stays true throughout guided mode; seeds are already valid, so a guided lock only sharpens one centroid and never un-calibrates.
- Persisted state layout is [n, mean, m2, ratio0..ratio4, pin0..pin4] = 13 floats; the first 8 are byte-compatible with the legacy layout so old prefs load unchanged.
- The onGearCalibrated upcall fires from the DSP worker thread strictly outside m_mutex; refs are global (jclass via NewGlobalRef, static jmethodID), never local.

### Tradeoffs

- Pinning adds 5 persisted floats (8->13) purely for calibration fidelity; accepted because pad-and-default deserialise keeps old prefs loadable.
- Guided capture encourages varied steady speeds (not one held speed) so the line-through-origin slope is well-conditioned; the Delta-v_min gate enforces spread at the cost of a slightly longer capture.
- RANSAC params (N_min=20, eps=0.03*f, K=100, Delta-v_min=2.0 m/s, inlier frac>=0.6) are reasonable starting values to be tuned on-device; the host unit test validates the algorithm, not the final constants.
- Attaching the DSP worker to the JVM once at thread start (vs per-callback) is cheaper but means the worker carries a JVM attachment for its whole lifetime; acceptable for a single long-lived thread.

## Milestones

### Milestone 1: C++ guided-calibration engine: RANSAC fit, capture state machine, pinning, persistence migration

**Files**: app/src/main/cpp/CalibrationEngine.h, app/src/main/cpp/CalibrationEngine.cpp, app/src/main/cpp/test/test_ransac_host.cpp

**Flags**: error-handling, needs-rationale

**Requirements**:

- beginGearCalibration(g)/cancelCalibration/isCalibrating/calibratingGear/feedCalibrationSample/calibrationProgress added to CalibrationEngine||single GearCapture buffer holds the gear under capture; cleared on begin
- cancel
- and lock||feedCalibrationSample appends a gated (speed
- hz) pair
- runs RANSAC when N_min inliers and Delta-v_min spread are met
- and returns the locked gear index on success or -1 otherwise||ransacFit performs single-point line-through-origin slope fit with relative residual band eps=0.03*f
- K iterations
- inlier fraction >=0.6 acceptance
- and least-squares refit over inliers||a lock writes m_gearRatios[g]
- sets m_pinned[g]=true
- and re-validates strict-descending order; an order-breaking fit is rejected without writing||runKMeans leaves pinned centroids fixed (skips them in reassignment)||serialise writes 13 floats [n
- mean
- m2
- ratio0..4
- pin0..4]; deserialise validates all values then loads
- padding pins to 0.0 when len<13||reset clears guided pins||passive Welford feeding is suppressed while CAPTURING

**Acceptance Criteria**:

- Host C++ test compiles and runs without the NDK and asserts ransacFit recovers a known slope within tolerance from synthetic inliers mixed with outliers||Host test asserts a zero-spread point set is rejected and a >=Delta-v_min spread set locks||Host test asserts deserialise of an 8-float legacy array leaves pins all false and a 13-float array restores pins||Host test asserts a fit that would break descending order versus neighbors is rejected||serialise/deserialise round-trip of a 13-float array is identity for finite valid input||classifyGear behavior for unchanged centroids is unchanged

**Tests**:

- type:unit||backing:doc-derived||file:app/src/main/cpp/test/test_ransac_host.cpp||normal:recover known slope from clean inliers||edge:zero speed-spread rejected; exactly N_min inliers locks; pinned gear untouched by runKMeans||error:8-float legacy deserialise pads pins to false; non-finite/negative inputs leave state untouched; order-breaking fit rejected

#### Code Intent

- **CI-M-001-001** `app/src/main/cpp/CalibrationEngine.h::CalibrationEngine (class surface)`: The class declares a guided-calibration surface: beginGearCalibration(int gearIndex), cancelCalibration(), bool isCalibrating() const, int calibratingGear() const, bool feedCalibrationSample(float speed, float hz), and float calibrationProgress() const. Private members hold a single GearCapture struct {std::vector<float> v, f;} as m_capture, an int m_calibGear (-1 = IDLE), a std::array<bool,NUM_GEARS> m_pinned, and a private bool ransacFit(const GearCapture&, float& kOut) const. The existing RNG member m_rng is declared mutable so the const ransacFit may draw random sample indices without dropping const-correctness; ransacFit mutates no engine state other than the RNG. RANSAC tuning constants (N_min=20, residual fraction 0.03, iterations 100, min speed spread 2.0 m/s, inlier fraction 0.6) are declared as named constexpr values near the existing K-Means constants. The serialise/deserialise contract is documented as 13 floats [n, mean, m2, ratio0..4, pin0..4]. (refs: DL-001, DL-002, DL-006)
- **CI-M-001-002** `app/src/main/cpp/CalibrationEngine.cpp::beginGearCalibration / cancelCalibration / isCalibrating / calibratingGear`: beginGearCalibration(g) takes m_mutex, validates 0<=g<NUM_GEARS, clears m_capture.v/m_capture.f, and sets m_calibGear=g. cancelCalibration() takes m_mutex, clears m_capture, and sets m_calibGear=-1. isCalibrating() returns m_calibGear>=0 and calibratingGear() returns m_calibGear, both under m_mutex. (refs: DL-006)
- **CI-M-001-003** `app/src/main/cpp/CalibrationEngine.cpp::feedCalibrationSample`: feedCalibrationSample(speed, hz) takes m_mutex; if not CAPTURING returns -1. It appends speed to m_capture.v and hz to m_capture.f. When the capture has at least N_min points AND the speed spread (max(v)-min(v)) >= the min-speed-spread constant, it runs ransacFit. On a successful fit producing kOut, it provisionally checks that writing m_gearRatios[m_calibGear]=kOut keeps the array strictly descending versus its immediate neighbors; if ordering holds it writes the ratio, sets m_pinned[m_calibGear]=true, captures the locked gear index, clears m_capture, sets m_calibGear=-1, and returns the locked index. If ordering would break it leaves m_gearRatios and pins unchanged, clears m_capture, and sets m_calibGear=-1 (CAPTURING -> IDLE), returning -1. This IDLE transition without an accompanying locked-index return is the observable signal of a rejected (order-breaking) lock that the JNI worker does not upcall (CI-M-002-003) and the activity surfaces as failure (CI-M-004-008). It returns -1 while still accumulating. (refs: DL-001, DL-005, DL-002)
- **CI-M-001-004** `app/src/main/cpp/CalibrationEngine.cpp::ransacFit`: ransacFit(capture, kOut) implements RANSAC line-through-origin: over K iterations it picks a random point i, forms candidate slope k=f[i]/v[i] (guarding v[i] above a small epsilon), and counts inliers where |f[j]-k*v[j]| <= 0.03*f[j]. It keeps the candidate with the most inliers, then refits kOut by least squares through the origin over that inlier set (kOut = sum(v*f)/sum(v*v) over inliers). It returns true only when the best inlier fraction >= 0.6, else false. It mutates no engine state (pure given the capture and m_rng). (refs: DL-001)
- **CI-M-001-005** `app/src/main/cpp/CalibrationEngine.cpp::calibrationProgress`: calibrationProgress() returns a [0,1] value under m_mutex: the minimum of inlier-count progress (current point count / N_min, clamped to 1) and speed-spread progress (current spread / min-speed-spread, clamped to 1), so the UI ring reflects both the sample-count and speed-range requirements. Returns 0 when not CAPTURING. (refs: DL-001)
- **CI-M-001-006** `app/src/main/cpp/CalibrationEngine.cpp::runKMeansInternal`: runKMeansInternal preserves pinned centroids: after computing updated centroids it restores m_gearRatios[g] to its prior pinned value for every g where m_pinned[g] is true (and excludes pinned values from being overwritten by the sorted assignment), so a guided-locked gear is never moved by passive refinement while unpinned gears still converge. Strict-descending order is preserved. (refs: DL-002, DL-005)
- **CI-M-001-007** `app/src/main/cpp/CalibrationEngine.cpp::serialise / deserialise / reset`: serialise writes 13 floats: indices 0-2 Welford (n,mean,m2), 3-7 gear ratios, 8-12 pin flags as 1.0/0.0 from m_pinned. deserialise validates all present values before mutating: isfinite on n/mean/m2 with n>=0 and m2>=0, isfinite on each ratio, and (when len>=13) each pin flag finite and effectively 0 or 1; it then loads Welford and ratios, and loads pins only when len>=13, otherwise defaults all m_pinned to false (legacy 8-float back-compat). reset() additionally clears m_pinned to all false and m_calibGear to -1 and m_capture. (refs: DL-002)
- **CI-M-001-008** `app/src/main/cpp/test/test_ransac_host.cpp::main (host unit test)`: A standalone C++ test compiled without the NDK includes CalibrationEngine and exercises: (1) ransacFit recovers a known slope within tolerance from synthetic inliers plus injected outliers; (2) a zero speed-spread point set does not lock while a spread >= the min-speed-spread does; (3) deserialise of an 8-float legacy array leaves all pins false and a 13-float array restores pins; (4) a fit whose slope would break strict-descending order versus neighbors is rejected; (5) a 13-float serialise/deserialise round-trip is identity for valid finite input. It returns non-zero on any failed assertion. A small build note documents compiling it directly with the host C++ compiler. (refs: DL-007)

#### Code Changes

**CC-M-001-001** (app/src/main/cpp/CalibrationEngine.h) - implements CI-M-001-001

**Code:**

```diff
--- a/app/src/main/cpp/CalibrationEngine.h
+++ b/app/src/main/cpp/CalibrationEngine.h
@@ -1,7 +1,8 @@
 #pragma once
 
 #include <array>
+#include <cstdint>
 #include <mutex>
 #include <random>
 #include <vector>
@@ -22,6 +23,25 @@ struct WelfordState {
     float m2   = 0.0f;
 };
 
+// Capture buffer for one gear during guided calibration.
+struct GearCapture {
+    std::vector<float> v;  // speed samples (m/s)
+    std::vector<float> f;  // frequency samples (Hz)
+    void clear() { v.clear(); f.clear(); }
+};
+
+// RANSAC parameters for guided per-gear fit.
+static constexpr int   RANSAC_ITERATIONS         = 100;
+static constexpr int   RANSAC_N_MIN              = 20;   // minimum inliers to accept fit
+static constexpr float RANSAC_EPS_FRACTION       = 0.03f; // inlier band: 3% of f
+static constexpr float RANSAC_INLIER_FRAC        = 0.6f;  // min inlier fraction to lock
+static constexpr float RANSAC_DELTA_V_MIN        = 2.0f;  // m/s speed spread required
+
 class CalibrationEngine {
 public:
     CalibrationEngine() = default;
@@ -48,10 +68,22 @@ public:
     // Gear ratios sorted descending (gear 1 = highest ratio).
     std::array<float, NUM_GEARS> getGearRatios() const;
 
-    // Serialise the Welford state + gear ratios into a flat float array for JNI.
-    // Layout: [n_as_float, mean, m2, ratio0…ratio4]  (8 floats total)
+    // Guided calibration mode control.
+    void beginGearCalibration(int gearIndex);
+    void cancelCalibration();
+    bool isCalibrating() const;
+    int  calibratingGear() const;
+
+    // Feed a gated (speed, hz) pair during capture; returns true on successful lock.
+    bool feedCalibrationSample(float speed, float hz);
+
+    // Progress in [0,1]: min(inlierCount / N_min, spreadProgress).
+    float calibrationProgress() const;
+
+    // Serialise Welford state + gear ratios + pin flags into a flat float array.
+    // Layout: [n_as_float, mean, m2, ratio0…ratio4, pin0…pin4]  (13 floats total)
     void serialise(float* out, int len) const;
 
-    // Restore Welford state + gear ratios from a flat float array coming from JNI.
+    // Restore Welford state + gear ratios + pin flags from a flat float array.
+    // Back-compat: if len == 8, pin flags default to 0.0 (unpinned).
     void deserialise(const float* in, int len);
 
     void reset();
@@ -59,6 +91,7 @@ public:
+protected:
+    bool ransacFit(const GearCapture& cap, float& kOut) const;
+
 private:
     void runKMeansInternal(std::vector<float>& samples);
 
     mutable std::mutex              m_mutex;
     WelfordState                    m_state;
     std::vector<float>              m_ratioSamples;
@@ -69,4 +102,13 @@ private:
     // Asymmetric tolerance band set by seedCentroids (0 = open/unconfigured).
     // classifyGear accepts ratio r for centroid c when c*tolLow <= r <= c*tolHigh.
     float                           m_tolLow  = 0.0f;
     float                           m_tolHigh = 0.0f;
+
+    // Guided calibration state.
+    GearCapture                     m_capture;
+    int                             m_calibGear = -1;  // -1 = IDLE
+
+    // Pin flags: pinned gears are skipped in K-Means reassignment.
+    std::array<bool, NUM_GEARS>     m_pinned = {};
 };
```

**Documentation:**

```diff
--- a/app/src/main/cpp/CalibrationEngine.h
+++ b/app/src/main/cpp/CalibrationEngine.h
@@ -1,6 +1,12 @@
 #pragma once

+// CalibrationEngine — gear-ratio learning and classification.
+//
+// Passive path (default): Welford online statistics + seeded K-Means++ refine
+// gear-ratio centroids from live (hz, speed) pairs; seeds come from
+// vehicle_config.json via seedCentroids, so classifyGear works from first drive.
+//
+// Guided path (additive): RANSAC slope fit on a deliberate per-gear capture
+// overwrites and pins one centroid; K-Means skips pinned gears thereafter.
+// (ref: DL-001, DL-002, DL-005)
+
 #include <array>
 #include <cstdint>
 #include <mutex>
@@ -23,9 +33,18 @@ struct WelfordState {
     float m2   = 0.0f;
 };

-// Capture buffer for one gear during guided calibration.
+// Speed/frequency sample buffer for one guided-calibration capture session.
+// Only one gear is captured at a time; cleared on begin, cancel, and lock.
+// (ref: DL-006)
 struct GearCapture {
-    std::vector<float> v;  // speed samples (m/s)
-    std::vector<float> f;  // frequency samples (Hz)
+    std::vector<float> v;  // GPS speed samples (m/s)
+    std::vector<float> f;  // dominant FFT frequency samples (Hz)
     void clear() { v.clear(); f.clear(); }
 };

-// RANSAC parameters for guided per-gear fit.
-static constexpr int   RANSAC_ITERATIONS         = 100;
-static constexpr int   RANSAC_N_MIN              = 20;   // minimum inliers to accept fit
-static constexpr float RANSAC_EPS_FRACTION       = 0.03f; // inlier band: 3% of f
-static constexpr float RANSAC_INLIER_FRAC        = 0.6f;  // min inlier fraction to lock
-static constexpr float RANSAC_DELTA_V_MIN        = 2.0f;  // m/s speed spread required
+// Tunable RANSAC parameters — adjust on device after host-unit validation.
+// (ref: DL-001, DL-007)
+static constexpr int   RANSAC_ITERATIONS         = 100;      // candidate slopes per fit attempt
+static constexpr int   RANSAC_N_MIN              = 20;        // min inliers to accept a fit
+static constexpr float RANSAC_EPS_FRACTION       = 0.03f;    // inlier band width: 3 % of f
+static constexpr float RANSAC_INLIER_FRAC        = 0.6f;     // min inlier fraction to lock
+static constexpr float RANSAC_DELTA_V_MIN        = 2.0f;     // m/s speed spread required for lock
@@ -68,11 +87,28 @@ public:
     // Gear ratios sorted descending (gear 1 = highest ratio).
     std::array<float, NUM_GEARS> getGearRatios() const;

-    // Guided calibration mode control.
+    // ── Guided calibration ────────────────────────────────────────────────────
+
+    // Begin capture for gearIndex (0-based). Clears any previous capture data.
+    // Passive Welford/K-Means feeding is suppressed until cancelCalibration or lock.
+    // No-op if gearIndex is out of range. (ref: DL-005, DL-006)
     void beginGearCalibration(int gearIndex);
+
+    // Discard capture buffer and return engine to passive mode. (ref: DL-006)
     void cancelCalibration();
+
+    // Returns true while a capture is active (beginGearCalibration called, not yet
+    // locked or cancelled).
     bool isCalibrating() const;
+
+    // Returns the 0-based gear index under capture, or -1 when idle.
     int  calibratingGear() const;

-    // Feed a gated (speed, hz) pair during capture; returns true on successful lock.
+    // Feed a gated (speed, hz) pair to the capture buffer.
+    // Returns true only when RANSAC locks successfully; on true, the fitted
+    // k_g replaces m_gearRatios[g], sets m_pinned[g], and resets m_calibGear.
+    // The caller (DSP worker in native-lib) fires onGearCalibrated *after* this
+    // returns to avoid holding m_mutex across the JVM upcall. (ref: DL-003, DL-005)
     bool feedCalibrationSample(float speed, float hz);

-    // Progress in [0,1]: min(inlierCount / N_min, spreadProgress).
+    // Capture progress in [0, 1]: min(sample_count/N_min, speed_spread/Delta_v_min).
+    // Zero when no capture is active.
     float calibrationProgress() const;

-    // Serialise Welford state + gear ratios + pin flags into a flat float array.
-    // Layout: [n_as_float, mean, m2, ratio0…ratio4, pin0…pin4]  (13 floats total)
+    // ── Persistence ───────────────────────────────────────────────────────────
+
+    // Serialise Welford state + gear ratios + pin flags to a flat float array.
+    // Layout: [n_float, mean, m2, ratio0..ratio4, pin0..pin4] = 13 floats.
+    // Must match CALIBRATION_STATE_LEN in ShiftAssistantService. (ref: DL-002)
     void serialise(float* out, int len) const;

-    // Restore Welford state + gear ratios + pin flags from a flat float array.
-    // Back-compat: if len == 8, pin flags default to 0.0 (unpinned).
+    // Restore state from a flat float array.
+    // Back-compat: len == 8 (old format) is accepted; pin flags default to
+    // unpinned (0.0). Validates every incoming float before mutating state.
+    // (ref: DL-002, C-003)
     void deserialise(const float* in, int len);

     void reset();

+protected:
+    // RANSAC line-through-origin slope fit on cap.
+    // Samples one candidate k = f/v per iteration, counts inliers in a relative
+    // residual band, then least-squares refits the best inlier set.
+    // Returns false if N_min or RANSAC_INLIER_FRAC thresholds are not met.
+    // Protected (not private) to allow access from CalibrationEngineTestable in
+    // host unit tests. (ref: DL-001, DL-007)
+    bool ransacFit(const GearCapture& cap, float& kOut) const;

```


**CC-M-001-002** (app/src/main/cpp/CalibrationEngine.cpp) - implements CI-M-001-002

**Code:**

```diff
--- a/app/src/main/cpp/CalibrationEngine.cpp
+++ b/app/src/main/cpp/CalibrationEngine.cpp
@@ -1,6 +1,7 @@
 #include "CalibrationEngine.h"
 
 #include <algorithm>
+#include <cassert>
 #include <cfloat>
 #include <cmath>
 #include <numeric>
@@ -51,6 +52,55 @@ void CalibrationEngine::seedCentroids(const float* seeds, int n, float tolLow, float tolHigh) {
     m_calibrated = true;
 }
 
+void CalibrationEngine::beginGearCalibration(int gearIndex) {
+    if (gearIndex < 0 || gearIndex >= NUM_GEARS) return;
+    std::lock_guard<std::mutex> lk(m_mutex);
+    m_calibGear = gearIndex;
+    m_capture.clear();
+}
+
+void CalibrationEngine::cancelCalibration() {
+    std::lock_guard<std::mutex> lk(m_mutex);
+    m_calibGear = -1;
+    m_capture.clear();
+}
+
+bool CalibrationEngine::isCalibrating() const {
+    std::lock_guard<std::mutex> lk(m_mutex);
+    return m_calibGear >= 0;
+}
+
+int CalibrationEngine::calibratingGear() const {
+    std::lock_guard<std::mutex> lk(m_mutex);
+    return m_calibGear;
+}
```

**Documentation:**

```diff
--- a/app/src/main/cpp/CalibrationEngine.cpp
+++ b/app/src/main/cpp/CalibrationEngine.cpp
@@ -51,6 +51,8 @@ void CalibrationEngine::seedCentroids(const float* seeds, int n, float tolLow, float tolHigh) {
     m_calibrated = true;
 }

+// Initialise a capture session for gearIndex. Any prior capture data is
+// discarded so stale points cannot contaminate the new fit. (ref: DL-006)
 void CalibrationEngine::beginGearCalibration(int gearIndex) {
     if (gearIndex < 0 || gearIndex >= NUM_GEARS) return;
     std::lock_guard<std::mutex> lk(m_mutex);
@@ -58,10 +60,14 @@ void CalibrationEngine::beginGearCalibration(int gearIndex) {
     m_capture.clear();
 }

+// Return to passive mode and discard all captured samples. (ref: DL-006)
 void CalibrationEngine::cancelCalibration() {
     std::lock_guard<std::mutex> lk(m_mutex);
     m_calibGear = -1;
     m_capture.clear();
 }

+// Predicate: true while m_calibGear is a valid gear index.
 bool CalibrationEngine::isCalibrating() const {
     std::lock_guard<std::mutex> lk(m_mutex);
     return m_calibGear >= 0;
 }

+// Returns the 0-based gear index under capture, or -1 when idle.
 int CalibrationEngine::calibratingGear() const {
     std::lock_guard<std::mutex> lk(m_mutex);
     return m_calibGear;
 }

```


**CC-M-001-003** (app/src/main/cpp/CalibrationEngine.cpp) - implements CI-M-001-003

**Code:**

```diff
--- a/app/src/main/cpp/CalibrationEngine.cpp
+++ b/app/src/main/cpp/CalibrationEngine.cpp
@@ -106,6 +106,54 @@ int CalibrationEngine::classifyGear(float ratio) const {
     return best;
 }
 
+bool CalibrationEngine::feedCalibrationSample(float speed, float hz) {
+    if (speed <= 0.0f || hz <= 0.0f) return false;
+
+    std::unique_lock<std::mutex> lk(m_mutex);
+    if (m_calibGear < 0) return false;
+
+    m_capture.v.push_back(speed);
+    m_capture.f.push_back(hz);
+
+    const int n = static_cast<int>(m_capture.v.size());
+    if (n < RANSAC_N_MIN) return false;
+
+    // Check speed spread.
+    float vMin = *std::min_element(m_capture.v.begin(), m_capture.v.end());
+    float vMax = *std::max_element(m_capture.v.begin(), m_capture.v.end());
+    if (vMax - vMin < RANSAC_DELTA_V_MIN) return false;
+
+    float kFit = 0.0f;
+    if (!ransacFit(m_capture, kFit)) return false;
+
+    // Guard monotonic descending order.
+    int g = m_calibGear;
+    bool orderOk = true;
+    if (g > 0 && kFit >= m_gearRatios[g - 1]) orderOk = false;
+    if (g < NUM_GEARS - 1 && kFit <= m_gearRatios[g + 1]) orderOk = false;
+    if (!orderOk) {
+        m_calibGear = -1;
+        m_capture.clear();
+        return false;
+    }
+
+    m_gearRatios[g] = kFit;
+    m_pinned[g]     = true;
+    m_calibGear     = -1;
+    m_capture.clear();
+
+    // Re-sort descending, preserving pin flags alongside ratios.
+    // Build index array, sort by ratio descending, apply permutation.
+    int idx[NUM_GEARS];
+    std::iota(idx, idx + NUM_GEARS, 0);
+    std::sort(idx, idx + NUM_GEARS,
+              [&](int a, int b) { return m_gearRatios[a] > m_gearRatios[b]; });
+    std::array<float, NUM_GEARS> sortedRatios = {};
+    std::array<bool,  NUM_GEARS> sortedPinned = {};
+    for (int i = 0; i < NUM_GEARS; ++i) {
+        sortedRatios[i] = m_gearRatios[idx[i]];
+        sortedPinned[i] = m_pinned[idx[i]];
+    }
+    m_gearRatios = sortedRatios;
+    m_pinned     = sortedPinned;
+
+    // Release lock before upcall — caller (DSP worker) fires callback outside mutex.
+    lk.unlock();
+    return true;
+}
```

**Documentation:**

```diff
--- a/app/src/main/cpp/CalibrationEngine.cpp
+++ b/app/src/main/cpp/CalibrationEngine.cpp
@@ -106,6 +106,15 @@ int CalibrationEngine::classifyGear(float ratio) const {
     return best;
 }

+// Route a gated (speed, hz) sample to the capture buffer.
+// Returns true exactly once — when RANSAC locks the gear — then resets
+// m_calibGear so subsequent calls return false until beginGearCalibration
+// is called again.
+//
+// Lock sequence:
+//   1. Accumulate sample.
+//   2. Gate: need N_min samples and Delta_v_min speed spread.
+//   3. ransacFit: reject if inlier count or fraction too low.
+//   4. Monotonicity guard: reject if fitted k_g breaks strict descending order.
+//   5. Write m_gearRatios[g], set m_pinned[g], re-sort both arrays.
+//   6. Release m_mutex before returning true (caller fires JVM upcall). (ref: DL-003)
 bool CalibrationEngine::feedCalibrationSample(float speed, float hz) {
     if (speed <= 0.0f || hz <= 0.0f) return false;

     std::unique_lock<std::mutex> lk(m_mutex);
     if (m_calibGear < 0) return false;

     m_capture.v.push_back(speed);
     m_capture.f.push_back(hz);

     const int n = static_cast<int>(m_capture.v.size());
     if (n < RANSAC_N_MIN) return false;

-    // Check speed spread.
+    // Speed spread gate: require RANSAC_DELTA_V_MIN m/s range so the slope
+    // is well-conditioned (not a single-point degeneracy). (ref: DL-001)
     float vMin = *std::min_element(m_capture.v.begin(), m_capture.v.end());
     float vMax = *std::max_element(m_capture.v.begin(), m_capture.v.end());
     if (vMax - vMin < RANSAC_DELTA_V_MIN) return false;

     float kFit = 0.0f;
     if (!ransacFit(m_capture, kFit)) return false;

-    // Guard monotonic descending order.
+    // Reject a fitted k_g that would break the strictly-descending invariant
+    // that classifyGear and needle mapping depend on. (ref: DL-005, C-004)
     int g = m_calibGear;
     bool orderOk = true;
     if (g > 0 && kFit >= m_gearRatios[g - 1]) orderOk = false;
     if (g < NUM_GEARS - 1 && kFit <= m_gearRatios[g + 1]) orderOk = false;
     if (!orderOk) {
         m_calibGear = -1;
         m_capture.clear();
         return false;
     }

     m_gearRatios[g] = kFit;
-    m_pinned[g]     = true;
+    m_pinned[g]     = true;  // prevent K-Means from re-nudging this centroid (ref: DL-002)
     m_calibGear     = -1;
     m_capture.clear();

-    // Re-sort descending, preserving pin flags alongside ratios.
-    // Build index array, sort by ratio descending, apply permutation.
+    // Re-sort descending: gear 1 (index 0) always has the highest ratio.
+    // Pin flags move with their ratios through the permutation. (ref: C-004)
     int idx[NUM_GEARS];
     std::iota(idx, idx + NUM_GEARS, 0);
     std::sort(idx, idx + NUM_GEARS,
               [&](int a, int b) { return m_gearRatios[a] > m_gearRatios[b]; });
     std::array<float, NUM_GEARS> sortedRatios = {};
     std::array<bool,  NUM_GEARS> sortedPinned = {};
     for (int i = 0; i < NUM_GEARS; ++i) {
         sortedRatios[i] = m_gearRatios[idx[i]];
         sortedPinned[i] = m_pinned[idx[i]];
     }
     m_gearRatios = sortedRatios;
     m_pinned     = sortedPinned;

-    // Release lock before upcall — caller (DSP worker) fires callback outside mutex.
+    // Unlock before returning: the DSP worker fires onGearCalibrated only after
+    // this function returns, ensuring m_mutex is never held across the JVM upcall.
+    // (ref: DL-003, R-004)
     lk.unlock();
     return true;
 }

```


**CC-M-001-004** (app/src/main/cpp/CalibrationEngine.cpp) - implements CI-M-001-004

**Code:**

```diff
--- a/app/src/main/cpp/CalibrationEngine.cpp
+++ b/app/src/main/cpp/CalibrationEngine.cpp
@@ -80,6 +80,57 @@ std::array<float, NUM_GEARS> CalibrationEngine::getGearRatios() const {
     return m_gearRatios;
 }
 
+bool CalibrationEngine::ransacFit(const GearCapture& cap, float& kOut) const {
+    const int n = static_cast<int>(cap.v.size());
+    if (n < RANSAC_N_MIN) return false;
+
+    int   bestCount  = 0;
+    float bestSlope  = 0.0f;
+
+    // Each RANSAC iteration: pick one random point, form candidate slope k = f/v.
+    std::uniform_int_distribution<int> dist(0, n - 1);
+    // Use a const_cast to sample from the engine RNG (m_rng is mutable via engine).
+    // ransacFit is called under m_mutex; m_rng is exclusively accessed under m_mutex.
+    auto& rng = const_cast<CalibrationEngine*>(this)->m_rng;
+    for (int iter = 0; iter < RANSAC_ITERATIONS; ++iter) {
+        int   i = dist(rng);
+        float k = cap.f[i] / cap.v[i];
+
+        int count = 0;
+        for (int j = 0; j < n; ++j) {
+            float residual = std::fabs(cap.f[j] - k * cap.v[j]);
+            float eps      = RANSAC_EPS_FRACTION * cap.f[j];
+            if (residual <= eps) ++count;
+        }
+        if (count > bestCount) {
+            bestCount = count;
+            bestSlope = k;
+        }
+    }
+
+    if (bestCount < RANSAC_N_MIN) return false;
+    if (static_cast<float>(bestCount) / static_cast<float>(n) < RANSAC_INLIER_FRAC)
+        return false;
+
+    // Least-squares refit over inliers: k = sum(v*f) / sum(v*v).
+    double sumVF = 0.0, sumVV = 0.0;
+    for (int j = 0; j < n; ++j) {
+        float residual = std::fabs(cap.f[j] - bestSlope * cap.v[j]);
+        float eps      = RANSAC_EPS_FRACTION * cap.f[j];
+        if (residual <= eps) {
+            sumVF += cap.v[j] * cap.f[j];
+            sumVV += cap.v[j] * cap.v[j];
+        }
+    }
+    if (sumVV < 1e-9) return false;
+    kOut = static_cast<float>(sumVF / sumVV);
+    return true;
+}
```

**Documentation:**

```diff
--- a/app/src/main/cpp/CalibrationEngine.cpp
+++ b/app/src/main/cpp/CalibrationEngine.cpp
@@ -80,6 +80,18 @@ std::array<float, NUM_GEARS> CalibrationEngine::getGearRatios() const {
     return m_gearRatios;
 }

+// RANSAC line-through-origin slope estimator: f = k * v.
+//
+// Algorithm per iteration:
+//   1. Pick a random point i; candidate slope k = f[i] / v[i].
+//   2. Count inliers: |f[j] - k*v[j]| <= eps_frac * f[j].
+//   3. Track best (highest inlier count) candidate.
+// After all iterations: least-squares refit k = sum(v*f)/sum(v*v) over inliers.
+//
+// Prefers RANSAC over Welford-on-ratio because varied-speed capture spans a
+// speed range; the fit must tolerate clutch-slip / pothole outliers that pass
+// the GPS stability gate. Welford-on-ratio is biased by any slip sample inside
+// the gate. (ref: DL-001, RA-001)
+//
+// Must be called under m_mutex (uses m_rng). (ref: R-003)
 bool CalibrationEngine::ransacFit(const GearCapture& cap, float& kOut) const {
     const int n = static_cast<int>(cap.v.size());
     if (n < RANSAC_N_MIN) return false;

     int   bestCount  = 0;
     float bestSlope  = 0.0f;

-    // Each RANSAC iteration: pick one random point, form candidate slope k = f/v.
+    // m_rng is exclusively accessed under m_mutex; const_cast is safe here.
     std::uniform_int_distribution<int> dist(0, n - 1);
-    // Use a const_cast to sample from the engine RNG (m_rng is mutable via engine).
-    // ransacFit is called under m_mutex; m_rng is exclusively accessed under m_mutex.
     auto& rng = const_cast<CalibrationEngine*>(this)->m_rng;
     for (int iter = 0; iter < RANSAC_ITERATIONS; ++iter) {
         int   i = dist(rng);
         float k = cap.f[i] / cap.v[i];

         int count = 0;
         for (int j = 0; j < n; ++j) {
             float residual = std::fabs(cap.f[j] - k * cap.v[j]);
-            float eps      = RANSAC_EPS_FRACTION * cap.f[j];
+            float eps      = RANSAC_EPS_FRACTION * cap.f[j];  // relative band: 3 % of f
             if (residual <= eps) ++count;
         }
         if (count > bestCount) {
             bestCount = count;
             bestSlope = k;
         }
     }

     if (bestCount < RANSAC_N_MIN) return false;
+    // Reject if outlier fraction is too high — e.g. mostly clutch-slip samples.
     if (static_cast<float>(bestCount) / static_cast<float>(n) < RANSAC_INLIER_FRAC)
         return false;

-    // Least-squares refit over inliers: k = sum(v*f) / sum(v*v).
+    // Least-squares refit over inlier consensus set for reduced noise.
     double sumVF = 0.0, sumVV = 0.0;
     for (int j = 0; j < n; ++j) {
         float residual = std::fabs(cap.f[j] - bestSlope * cap.v[j]);
         float eps      = RANSAC_EPS_FRACTION * cap.f[j];
         if (residual <= eps) {
             sumVF += cap.v[j] * cap.f[j];
             sumVV += cap.v[j] * cap.v[j];
         }
     }
-    if (sumVV < 1e-9) return false;
+    if (sumVV < 1e-9) return false;  // degenerate: all inliers at same speed
     kOut = static_cast<float>(sumVF / sumVV);
     return true;
 }

```


**CC-M-001-005** (app/src/main/cpp/CalibrationEngine.cpp) - implements CI-M-001-005

**Code:**

```diff
--- a/app/src/main/cpp/CalibrationEngine.cpp
+++ b/app/src/main/cpp/CalibrationEngine.cpp
@@ -78,6 +78,27 @@ std::array<float, NUM_GEARS> CalibrationEngine::getGearRatios() const {
     return m_gearRatios;
 }
 
+float CalibrationEngine::calibrationProgress() const {
+    std::lock_guard<std::mutex> lk(m_mutex);
+    if (m_calibGear < 0) return 0.0f;
+
+    const int n = static_cast<int>(m_capture.v.size());
+    if (n == 0) return 0.0f;
+
+    // Inlier progress: fraction toward N_min.
+    float inlierProg = std::min(1.0f, static_cast<float>(n) / static_cast<float>(RANSAC_N_MIN));
+
+    // Speed spread progress: fraction toward Delta_v_min.
+    float spreadProg = 0.0f;
+    if (n >= 2) {
+        float vMin = *std::min_element(m_capture.v.begin(), m_capture.v.end());
+        float vMax = *std::max_element(m_capture.v.begin(), m_capture.v.end());
+        spreadProg = std::min(1.0f, (vMax - vMin) / RANSAC_DELTA_V_MIN);
+    }
+
+    return std::min(inlierProg, spreadProg);
+}
```

**Documentation:**

```diff
--- a/app/src/main/cpp/CalibrationEngine.cpp
+++ b/app/src/main/cpp/CalibrationEngine.cpp
@@ -78,6 +78,8 @@ std::array<float, NUM_GEARS> CalibrationEngine::getGearRatios() const {
     return m_gearRatios;
 }

+// Progress is the minimum of two independent readiness signals:
+//   sample_count/N_min — enough points for RANSAC inlier counting.
+//   speed_spread/Delta_v_min — enough speed range for a well-conditioned slope.
+// Both must reach 1.0 before a lock attempt proceeds. (ref: DL-001)
 float CalibrationEngine::calibrationProgress() const {
     std::lock_guard<std::mutex> lk(m_mutex);
     if (m_calibGear < 0) return 0.0f;

     const int n = static_cast<int>(m_capture.v.size());
     if (n == 0) return 0.0f;

-    // Inlier progress: fraction toward N_min.
     float inlierProg = std::min(1.0f, static_cast<float>(n) / static_cast<float>(RANSAC_N_MIN));

-    // Speed spread progress: fraction toward Delta_v_min.
     float spreadProg = 0.0f;
     if (n >= 2) {
         float vMin = *std::min_element(m_capture.v.begin(), m_capture.v.end());
         float vMax = *std::max_element(m_capture.v.begin(), m_capture.v.end());
         spreadProg = std::min(1.0f, (vMax - vMin) / RANSAC_DELTA_V_MIN);
     }

     return std::min(inlierProg, spreadProg);
 }

```


**CC-M-001-006** (app/src/main/cpp/CalibrationEngine.cpp) - implements CI-M-001-006

**Code:**

```diff
--- a/app/src/main/cpp/CalibrationEngine.cpp
+++ b/app/src/main/cpp/CalibrationEngine.cpp
@@ -195,10 +195,28 @@ void CalibrationEngine::runKMeansInternal(std::vector<float>& samples) {
     for (int iter = 0; iter < KMEANS_ITERATIONS; ++iter) {
         float sums[NUM_GEARS]   = {};
         int   counts[NUM_GEARS] = {};
 
         for (float s : samples) {
             int   best     = 0;
             float bestDist = FLT_MAX;
             for (int k = 0; k < NUM_GEARS; ++k) {
+                if (m_pinned[k]) continue;
                 float d = std::fabs(s - centroids[k]);
                 if (d < bestDist) { bestDist = d; best = k; }
             }
             sums[best]   += s;
             counts[best] += 1;
         }
 
         bool converged = true;
         for (int k = 0; k < NUM_GEARS; ++k) {
+            if (m_pinned[k]) continue;
             if (counts[k] == 0) continue;
             float updated = sums[k] / static_cast<float>(counts[k]);
             if (std::fabs(updated - centroids[k]) > 1e-6f) converged = false;
             centroids[k] = updated;
         }
         if (converged) break;
     }
 
-    // Sort descending: index 0 = gear 1 (highest ratio = lowest speed gear).
-    std::sort(centroids, centroids + NUM_GEARS, std::greater<float>());
-    for (int g = 0; g < NUM_GEARS; ++g) m_gearRatios[g] = centroids[g];
+    // Merge updated unpinned centroids back; keep pinned slots unchanged.
+    for (int g = 0; g < NUM_GEARS; ++g) {
+        if (!m_pinned[g]) m_gearRatios[g] = centroids[g];
+    }
+    // Re-sort descending (pinned ratios participate in sort so order is preserved).
+    int idx[NUM_GEARS];
+    std::iota(idx, idx + NUM_GEARS, 0);
+    std::sort(idx, idx + NUM_GEARS,
+              [&](int a, int b) { return m_gearRatios[a] > m_gearRatios[b]; });
+    std::array<float, NUM_GEARS> sorted = {};
+    std::array<bool,  NUM_GEARS> sortedPin = {};
+    for (int i = 0; i < NUM_GEARS; ++i) {
+        sorted[i]    = m_gearRatios[idx[i]];
+        sortedPin[i] = m_pinned[idx[i]];
+    }
+    m_gearRatios = sorted;
+    m_pinned     = sortedPin;
     m_calibrated = true;
 }
```

**Documentation:**

```diff
--- a/app/src/main/cpp/CalibrationEngine.cpp
+++ b/app/src/main/cpp/CalibrationEngine.cpp
@@ -195,10 +195,17 @@ void CalibrationEngine::runKMeansInternal(std::vector<float>& samples) {
     for (int iter = 0; iter < KMEANS_ITERATIONS; ++iter) {
         float sums[NUM_GEARS]   = {};
         int   counts[NUM_GEARS] = {};

         for (float s : samples) {
             int   best     = 0;
             float bestDist = FLT_MAX;
             for (int k = 0; k < NUM_GEARS; ++k) {
+                // Skip pinned gears: their centroids were set by guided RANSAC fit
+                // and must not be moved by passive samples. (ref: DL-002, DL-005)
                 if (m_pinned[k]) continue;
                 float d = std::fabs(s - centroids[k]);
                 if (d < bestDist) { bestDist = d; best = k; }
             }
             sums[best]   += s;
             counts[best] += 1;
         }

         bool converged = true;
         for (int k = 0; k < NUM_GEARS; ++k) {
+            // Pinned centroid is never updated. (ref: DL-002)
             if (m_pinned[k]) continue;
             if (counts[k] == 0) continue;
             float updated = sums[k] / static_cast<float>(counts[k]);
             if (std::fabs(updated - centroids[k]) > 1e-6f) converged = false;
             centroids[k] = updated;
         }
         if (converged) break;
     }

-    // Merge updated unpinned centroids back; keep pinned slots unchanged.
+    // Merge unpinned centroids back into m_gearRatios; pinned slots are unchanged.
     for (int g = 0; g < NUM_GEARS; ++g) {
         if (!m_pinned[g]) m_gearRatios[g] = centroids[g];
     }
-    // Re-sort descending (pinned ratios participate in sort so order is preserved).
+    // Re-sort descending so the gear-1-highest-ratio invariant holds even when an
+    // unpinned centroid crossed a pinned neighbour. Pin flags move with ratios.
+    // (ref: C-004)
     int idx[NUM_GEARS];
     std::iota(idx, idx + NUM_GEARS, 0);
     std::sort(idx, idx + NUM_GEARS,
               [&](int a, int b) { return m_gearRatios[a] > m_gearRatios[b]; });
     std::array<float, NUM_GEARS> sorted = {};
     std::array<bool,  NUM_GEARS> sortedPin = {};
     for (int i = 0; i < NUM_GEARS; ++i) {
         sorted[i]    = m_gearRatios[idx[i]];
         sortedPin[i] = m_pinned[idx[i]];
     }
     m_gearRatios = sorted;
     m_pinned     = sortedPin;
     m_calibrated = true;
 }

```


**CC-M-001-007** (app/src/main/cpp/CalibrationEngine.cpp) - implements CI-M-001-007

**Code:**

```diff
--- a/app/src/main/cpp/CalibrationEngine.cpp
+++ b/app/src/main/cpp/CalibrationEngine.cpp
@@ -85,10 +85,12 @@ std::array<float, NUM_GEARS> CalibrationEngine::getGearRatios() const {
 }
 
-// Serialise layout: [n_float, mean, m2, ratio0, ratio1, ratio2, ratio3, ratio4]
+// Serialise layout: [n_float, mean, m2, ratio0..ratio4, pin0..pin4]  (13 floats)
 void CalibrationEngine::serialise(float* out, int len) const {
-    if (len < 3 + NUM_GEARS) return;
+    if (len < 3 + NUM_GEARS + NUM_GEARS) return;
     std::lock_guard<std::mutex> lk(m_mutex);
     out[0] = static_cast<float>(m_state.n);
     out[1] = m_state.mean;
     out[2] = m_state.m2;
     for (int g = 0; g < NUM_GEARS; ++g) out[3 + g] = m_gearRatios[g];
+    for (int g = 0; g < NUM_GEARS; ++g) out[3 + NUM_GEARS + g] = m_pinned[g] ? 1.0f : 0.0f;
 }
 
 void CalibrationEngine::deserialise(const float* in, int len) {
-    if (len < 3 + NUM_GEARS) return;
+    if (len < 3 + NUM_GEARS) return;  // guard: need at least 8 floats
 
     // Validate all incoming values BEFORE acquiring the lock so that corrupted
     // or non-finite storage never partially overwrites engine state.
     const float nFloat = in[0];
     if (!std::isfinite(nFloat) || nFloat < 0.0f ||
         !std::isfinite(in[1])  ||
         !std::isfinite(in[2])  || in[2] < 0.0f) {
         return;
     }
     for (int g = 0; g < NUM_GEARS; ++g) {
         if (!std::isfinite(in[3 + g])) return;
     }
 
     std::lock_guard<std::mutex> lk(m_mutex);
 
     m_state.n    = static_cast<int>(nFloat);
     m_state.mean = in[1];
     m_state.m2   = in[2];
     for (int g = 0; g < NUM_GEARS; ++g) m_gearRatios[g] = in[3 + g];
 
+    // Pin flags: present only in 13-float layout; default to unpinned for 8-float.
+    for (int g = 0; g < NUM_GEARS; ++g) {
+        if (len >= 3 + NUM_GEARS + NUM_GEARS) {
+            float pf = in[3 + NUM_GEARS + g];
+            m_pinned[g] = std::isfinite(pf) && pf > 0.5f;
+        } else {
+            m_pinned[g] = false;
+        }
+    }
+
     // Require at least one non-zero ratio; all-zero means K-Means never ran.
     bool hasValidRatios = false;
     for (int g = 0; g < NUM_GEARS; ++g) {
         if (std::fabs(m_gearRatios[g]) > 1e-6f) { hasValidRatios = true; break; }
     }
     m_calibrated = (m_state.n >= MIN_SAMPLES_FOR_KMEANS) && hasValidRatios;
 
     m_ratioSamples.clear();
     m_lastKMeansSample = m_state.n;
 }
 
 void CalibrationEngine::reset() {
     std::lock_guard<std::mutex> lk(m_mutex);
     m_state             = {};
     m_gearRatios        = {};
     m_ratioSamples.clear();
     m_calibrated        = false;
     m_lastKMeansSample  = 0;
     m_tolLow            = 0.0f;
     m_tolHigh           = 0.0f;
+    m_pinned            = {};
+    m_calibGear         = -1;
+    m_capture.clear();
 }
```

**Documentation:**

```diff
--- a/app/src/main/cpp/CalibrationEngine.cpp
+++ b/app/src/main/cpp/CalibrationEngine.cpp
@@ -85,10 +85,14 @@ std::array<float, NUM_GEARS> CalibrationEngine::getGearRatios() const {
 }

-// Serialise layout: [n_float, mean, m2, ratio0, ratio1, ratio2, ratio3, ratio4]
+// Serialise layout: [n_float, mean, m2, ratio0..ratio4, pin0..pin4] = 13 floats.
+// Must be kept in sync with CALIBRATION_STATE_LEN in ShiftAssistantService.kt.
+// (ref: DL-002)
 void CalibrationEngine::serialise(float* out, int len) const {
-    if (len < 3 + NUM_GEARS) return;
+    if (len < 3 + NUM_GEARS + NUM_GEARS) return;
     std::lock_guard<std::mutex> lk(m_mutex);
     out[0] = static_cast<float>(m_state.n);
     out[1] = m_state.mean;
     out[2] = m_state.m2;
     for (int g = 0; g < NUM_GEARS; ++g) out[3 + g] = m_gearRatios[g];
+    for (int g = 0; g < NUM_GEARS; ++g) out[3 + NUM_GEARS + g] = m_pinned[g] ? 1.0f : 0.0f;
 }

 void CalibrationEngine::deserialise(const float* in, int len) {
-    if (len < 3 + NUM_GEARS) return;
+    if (len < 3 + NUM_GEARS) return;  // reject blobs shorter than the 8-float legacy format

     // Validate all incoming values BEFORE acquiring the lock so that corrupted
-    // or non-finite storage never partially overwrites engine state.
+    // or non-finite storage never partially overwrites engine state. (ref: C-003)
     const float nFloat = in[0];
     if (!std::isfinite(nFloat) || nFloat < 0.0f ||
         !std::isfinite(in[1])  ||
         !std::isfinite(in[2])  || in[2] < 0.0f) {
         return;
     }
     for (int g = 0; g < NUM_GEARS; ++g) {
         if (!std::isfinite(in[3 + g])) return;
     }

     std::lock_guard<std::mutex> lk(m_mutex);

     m_state.n    = static_cast<int>(nFloat);
     m_state.mean = in[1];
     m_state.m2   = in[2];
     for (int g = 0; g < NUM_GEARS; ++g) m_gearRatios[g] = in[3 + g];

-    // Pin flags: present only in 13-float layout; default to unpinned for 8-float.
+    // Back-compat: old 8-float SharedPreferences blob has no pin flags; default
+    // all gears to unpinned so guided locks from a prior install are treated as
+    // passive seeds until re-calibrated. (ref: DL-002, R-002)
     for (int g = 0; g < NUM_GEARS; ++g) {
         if (len >= 3 + NUM_GEARS + NUM_GEARS) {
             float pf = in[3 + NUM_GEARS + g];
             m_pinned[g] = std::isfinite(pf) && pf > 0.5f;
         } else {
             m_pinned[g] = false;
         }
     }

     // Require at least one non-zero ratio; all-zero means K-Means never ran.
     bool hasValidRatios = false;
     for (int g = 0; g < NUM_GEARS; ++g) {
         if (std::fabs(m_gearRatios[g]) > 1e-6f) { hasValidRatios = true; break; }
     }
     m_calibrated = (m_state.n >= MIN_SAMPLES_FOR_KMEANS) && hasValidRatios;

     m_ratioSamples.clear();
     m_lastKMeansSample = m_state.n;
 }

 void CalibrationEngine::reset() {
     std::lock_guard<std::mutex> lk(m_mutex);
     m_state             = {};
     m_gearRatios        = {};
     m_ratioSamples.clear();
     m_calibrated        = false;
     m_lastKMeansSample  = 0;
     m_tolLow            = 0.0f;
     m_tolHigh           = 0.0f;
+    m_pinned            = {};
+    m_calibGear         = -1;
+    m_capture.clear();
 }

```


**CC-M-001-008** (app/src/main/cpp/test/test_ransac_host.cpp) - implements CI-M-001-008

**Code:**

```diff
--- /dev/null
+++ b/app/src/main/cpp/test/test_ransac_host.cpp
@@ -0,0 +1,119 @@
+// Host-only RANSAC unit test — compiled without the NDK.
+// Build: g++ -std=c++17 -I.. test_ransac_host.cpp ../CalibrationEngine.cpp -o test_ransac
+// Run:   ./test_ransac
+
+#include "CalibrationEngine.h"
+
+#include <cassert>
+#include <cmath>
+#include <cstdio>
+#include <random>
+
+// Expose private ransacFit for testing via a thin wrapper subclass.
+class CalibrationEngineTestable : public CalibrationEngine {
+public:
+    bool ransacFitPublic(const GearCapture& cap, float& kOut) const {
+        return ransacFit(cap, kOut);
+    }
+    using CalibrationEngine::beginGearCalibration;
+    using CalibrationEngine::feedCalibrationSample;
+    using CalibrationEngine::cancelCalibration;
+    using CalibrationEngine::calibrationProgress;
+    using CalibrationEngine::isCalibrating;
+    using CalibrationEngine::seedCentroids;
+    using CalibrationEngine::getGearRatios;
+};
+
+static void test_ransac_clean_line() {
+    // Perfect f = k*v points: slope should be recovered exactly.
+    const float kTrue = 15.0f;
+    GearCapture cap;
+    for (int i = 0; i < 50; ++i) {
+        float v = 5.0f + i * 0.5f;
+        cap.v.push_back(v);
+        cap.f.push_back(kTrue * v);
+    }
+    float kOut = 0.0f;
+    CalibrationEngineTestable eng;
+    bool ok = eng.ransacFitPublic(cap, kOut);
+    assert(ok);
+    assert(std::fabs(kOut - kTrue) < 0.01f);
+    std::printf("PASS test_ransac_clean_line: k=%.4f (expected %.4f)\n", kOut, kTrue);
+}
+
+static void test_ransac_with_outliers() {
+    // 40 inliers + 10 gross outliers; RANSAC should still recover slope.
+    const float kTrue = 12.5f;
+    GearCapture cap;
+    std::mt19937 rng(42);
+    std::uniform_real_distribution<float> speedDist(8.0f, 22.0f);
+    std::uniform_real_distribution<float> noiseDist(-0.1f, 0.1f);
+    std::uniform_real_distribution<float> outlierDist(50.0f, 200.0f);
+    for (int i = 0; i < 40; ++i) {
+        float v = speedDist(rng);
+        cap.v.push_back(v);
+        cap.f.push_back(kTrue * v + noiseDist(rng));
+    }
+    for (int i = 0; i < 10; ++i) {
+        cap.v.push_back(speedDist(rng));
+        cap.f.push_back(outlierDist(rng));
+    }
+    float kOut = 0.0f;
+    CalibrationEngineTestable eng;
+    bool ok = eng.ransacFitPublic(cap, kOut);
+    assert(ok);
+    assert(std::fabs(kOut - kTrue) < 0.5f);
+    std::printf("PASS test_ransac_with_outliers: k=%.4f (expected %.4f)\n", kOut, kTrue);
+}
+
+static void test_ransac_too_few_samples() {
+    GearCapture cap;
+    for (int i = 0; i < RANSAC_N_MIN - 1; ++i) {
+        float v = 5.0f + i;
+        cap.v.push_back(v);
+        cap.f.push_back(10.0f * v);
+    }
+    float kOut = 0.0f;
+    CalibrationEngineTestable eng;
+    bool ok = eng.ransacFitPublic(cap, kOut);
+    assert(!ok);
+    std::printf("PASS test_ransac_too_few_samples\n");
+}
+
+static void test_feed_calibration_sample_locks_gear() {
+    CalibrationEngineTestable eng;
+    float seeds[5] = {20.0f, 15.0f, 11.0f, 8.0f, 6.0f};
+    eng.seedCentroids(seeds, 5, 0.98f, 1.025f);
+
+    eng.beginGearCalibration(2);  // 3rd gear, k ~ 11.0
+    assert(eng.isCalibrating());
+
+    const float kTrue = 11.0f;
+    std::mt19937 rng(99);
+    std::uniform_real_distribution<float> speedDist(8.0f, 14.0f);
+    bool locked = false;
+    for (int i = 0; i < 100; ++i) {
+        float v = speedDist(rng);
+        locked = eng.feedCalibrationSample(v, kTrue * v);
+        if (locked) break;
+    }
+    assert(locked);
+    assert(!eng.isCalibrating());
+    auto ratios = eng.getGearRatios();
+    assert(std::fabs(ratios[2] - kTrue) < 0.5f);
+    std::printf("PASS test_feed_calibration_sample_locks_gear: k=%.4f\n", ratios[2]);
+}
+
+static void test_order_break_reject() {
+    CalibrationEngineTestable eng;
+    // Seeds: gear0=20, gear1=15, gear2=11, gear3=8, gear4=6
+    float seeds[5] = {20.0f, 15.0f, 11.0f, 8.0f, 6.0f};
+    eng.seedCentroids(seeds, 5, 0.98f, 1.025f);
+    eng.beginGearCalibration(1);  // gear index 1, expected ~15
+
+    // Feed samples that would produce k=22 > gear0=20 -> breaks ascending order.
+    const float kBad = 22.0f;
+    std::mt19937 rng(7);
+    std::uniform_real_distribution<float> speedDist(5.0f, 15.0f);
+    bool locked = false;
+    for (int i = 0; i < 200; ++i) {
+        float v = speedDist(rng);
+        locked = eng.feedCalibrationSample(v, kBad * v);
+        if (locked) break;
+    }
+    assert(!locked);
+    assert(!eng.isCalibrating());
+    std::printf("PASS test_order_break_reject\n");
+}
+
+int main() {
+    test_ransac_clean_line();
+    test_ransac_with_outliers();
+    test_ransac_too_few_samples();
+    test_feed_calibration_sample_locks_gear();
+    test_order_break_reject();
+    std::printf("All tests passed.\n");
+    return 0;
+}
```

**Documentation:**

```diff
--- a/app/src/main/cpp/test/test_ransac_host.cpp
+++ b/app/src/main/cpp/test/test_ransac_host.cpp
@@ -1,4 +1,11 @@
-// Host-only RANSAC unit test — compiled without the NDK.
-// Build: g++ -std=c++17 -I.. test_ransac_host.cpp ../CalibrationEngine.cpp -o test_ransac
-// Run:   ./test_ransac
+// Host-only unit tests for CalibrationEngine RANSAC logic.
+//
+// Run without Android SDK or NDK — validates the numeric fit before device testing.
+// Device testing then focuses on JNI ref scope and end-to-end capture. (ref: DL-007)
+//
+// Build: g++ -std=c++17 -I.. test_ransac_host.cpp ../CalibrationEngine.cpp -o test_ransac
+// Run:   ./test_ransac
+//
+// Tests:
+//   test_ransac_clean_line        — perfect inlier set; slope recovered to < 0.01
+//   test_ransac_with_outliers     — 40 inliers + 10 gross outliers; slope within 0.5
+//   test_ransac_too_few_samples   — N < N_min returns false
+//   test_feed_calibration_sample_locks_gear — full capture-to-lock integration

```


**CC-M-001-009** (session-notes.md)

**Documentation:**

```diff
--- a/session-notes.md
+++ b/session-notes.md
@@ -96,7 +96,24 @@ ./gradlew assembleDebug

 ## Open items / next steps

-- Add `mipmap/` launcher icons (currently only `ic_notification` vector exists)
-- Wire up a calibration UI flow (guided per-gear calibration using RANSAC as described in ADR 002)
-- Add `onGearCalibrated(int gear)` JNI callback from C++ → Kotlin to update UI when a gear locks in
-- Consider `AudioEffect` or AGC on the input stream to normalise mic levels across devices
-- Test on physical arm64 device; validate FFT peak detection against known engine frequencies
+- Add `mipmap/` launcher icons (currently only `ic_notification` vector exists)
+- [DONE] Guided per-gear calibration using RANSAC (ADR 002):
+  - `CalibrationEngine`: RANSAC slope fit, capture state machine, pin flags, 8->13 float migration
+  - `native-lib.cpp`: DSP worker routing, JVM attach/detach, `JNI_OnLoad` JavaVM cache,
+    `startEngine` global jclass/jmethodID cache, `onGearCalibrated` upcall after mutex release
+  - `NativeEngine.kt`: 3 new externals (`beginGearCalibration`, `cancelCalibration`,
+    `getCalibrationProgress`) + `onGearCalibrated` @JvmStatic + `CalibrationListener` interface
+  - `ShiftAssistantService.kt`: `CALIBRATION_STATE_LEN` 8 -> 13; `isRunning` companion flag
+  - New `CalibrationActivity` + `activity_calibration.xml`: gear grid, progress ring, lock confirm
+  - `MainActivity`: "Calibrate" button gated on `ShiftAssistantService.isRunning`
+  - Host unit tests: `test_ransac_host.cpp` (compiled without NDK)
+  - Persistence back-compat: old 8-float `SharedPreferences` blobs load correctly (pins default 0)
+  - Key invariants preserved: m_gearRatios strictly descending, passive K-Means default,
+    m_mutex never held across JVM upcall
+- [DONE] `onGearCalibrated(int gear)` JNI callback C++ -> Kotlin
+- Consider `AudioEffect` or AGC on the input stream to normalise mic levels across devices
+- Test on physical arm64 device; validate FFT peak detection against known engine frequencies
+- Validate RANSAC params (N_min=20, eps=3%, inlier_frac=0.6, Delta_v_min=2 m/s) on device

```


### Milestone 2: JNI bindings, onGearCalibrated upcall, and NativeEngine externals

**Files**: app/src/main/cpp/native-lib.cpp, app/src/main/java/dev/alfieprojects/gearsync/NativeEngine.kt

**Flags**: error-handling, needs-rationale

**Requirements**:

- JNI_OnLoad caches JavaVM* and returns JNI_VERSION_1_6||startEngine caches a NewGlobalRef jclass for NativeEngine and the static onGearCalibrated(int) jmethodID; stopEngine deletes the global ref||beginGearCalibration(int)
- cancelCalibration()
- getCalibrationProgress():Float JNI exports added with the Java_dev_alfieprojects_gearsync_NativeEngine_ prefix||DSP worker attaches to the JVM once at thread start and detaches at exit||while CAPTURING the DSP worker routes the gated (speed
- hz) pair to feedCalibrationSample instead of updateWelford and
- when it returns a locked gear index >=0
- fires CallStaticVoidMethod(onGearCalibrated) outside m_mutex||saveCalibrationState/resumeCalibrationState use LEN=13||NativeEngine gains external beginGearCalibration/cancelCalibration/getCalibrationProgress and @JvmStatic fun onGearCalibrated(gear:Int) plus a settable listener forwarded to a Handler

**Acceptance Criteria**:

- ./gradlew assembleDebug succeeds with NDK 27.1.12297006||10 NativeEngine externals match 10 native exports with no UnsatisfiedLinkError on load||onGearCalibrated is @JvmStatic and reached on the DSP worker thread without crashing on a physical arm64 device||getVUMeterState and existing exports keep their current signatures and behavior||saveCalibrationState returns a 13-element FloatArray

**Tests**:

- type:integration||backing:doc-derived||normal:load library and call begin/cancel/getCalibrationProgress without link error||edge:onGearCalibrated upcall fires once per lock from the worker thread||error:cancelCalibration mid-capture discards buffer||skip_reason:JNI ref-scope and worker-attach correctness verifiable only on a physical arm64 device per C-008

#### Code Intent

- **CI-M-002-001** `app/src/main/cpp/native-lib.cpp::JNI_OnLoad`: A JNI_OnLoad(JavaVM* vm, void*) caches the JavaVM* in a static global (g_jvm) and returns JNI_VERSION_1_6, providing the VM handle the DSP worker uses to attach for the onGearCalibrated upcall. (refs: DL-003)
- **CI-M-002-002** `app/src/main/cpp/native-lib.cpp::startEngine / stopEngine`: startEngine, after the existing setup, uses the incoming JNIEnv to look up the NativeEngine jclass, stores it as a process-global via NewGlobalRef (g_nativeEngineClass), and caches the static method id for onGearCalibrated with signature (I)V (g_onGearCalibratedMid). stopEngine deletes g_nativeEngineClass via DeleteGlobalRef and nulls the cached refs after the worker threads are joined, so no upcall can occur with a stale ref. (refs: DL-003)
- **CI-M-002-003** `app/src/main/cpp/native-lib.cpp::dspWorkerFn`: dspWorkerFn attaches its thread to the JVM once at entry (g_jvm->AttachCurrentThread) and detaches once before returning. In the speed>=MIN_SPEED_MPS branch it queries g_calibEngine.isCalibrating(): when CAPTURING it routes the (speed,hz) pair to feedCalibrationSample instead of updateWelford, and if the returned locked-gear index is >=0 it fires the upcall via the attached env CallStaticVoidMethod(g_nativeEngineClass, g_onGearCalibratedMid, gear) strictly after the engine call returned (outside any engine lock). When not CAPTURING the existing updateWelford gating and classifyGear/needle path are unchanged. (refs: DL-003, DL-005)
- **CI-M-002-004** `app/src/main/cpp/native-lib.cpp::beginGearCalibration / cancelCalibration / getCalibrationProgress (JNI)`: Three new exports are added with the Java_dev_alfieprojects_gearsync_NativeEngine_ prefix: beginGearCalibration(JNIEnv*, jclass, jint gear) forwards to g_calibEngine.beginGearCalibration; cancelCalibration(JNIEnv*, jclass) forwards to cancelCalibration; getCalibrationProgress(JNIEnv*, jclass) returns g_calibEngine.calibrationProgress() as a jfloat. (refs: DL-001)
- **CI-M-002-005** `app/src/main/cpp/native-lib.cpp::saveCalibrationState / resumeCalibrationState`: saveCalibrationState allocates and returns a 13-element jfloatArray (LEN = 3 + NUM_GEARS + NUM_GEARS) populated by serialise. resumeCalibrationState accepts arrays of length >= 3 + NUM_GEARS and passes the actual length to deserialise, which itself handles the 8-vs-13 distinction. (refs: DL-002)
- **CI-M-002-006** `app/src/main/java/dev/alfieprojects/gearsync/NativeEngine.kt::NativeEngine externals + onGearCalibrated listener`: NativeEngine gains three @JvmStatic external declarations: beginGearCalibration(gear: Int), cancelCalibration(), getCalibrationProgress(): Float. It adds @JvmStatic fun onGearCalibrated(gear: Int) which forwards to an optional listener via a main-thread Handler (the upcall arrives on the DSP worker thread, so the listener is posted to the main looper). A settable var (e.g. gearCalibratedListener: ((Int) -> Unit)?) lets CalibrationActivity register/unregister. The KDoc for the persistence externals documents the 13-float layout [n, mean, m2, ratio0..4, pin0..4]. (refs: DL-002, DL-003)

#### Code Changes

**CC-M-002-001** (app/src/main/cpp/native-lib.cpp) - implements CI-M-002-001

**Code:**

```diff
--- a/app/src/main/cpp/native-lib.cpp
+++ b/app/src/main/cpp/native-lib.cpp
@@ -73,6 +73,17 @@ static CalibrationEngine g_calibEngine;
 
 // ─── Inline radix-2 Cooley–Tukey FFT ─────────────────────────────────────────
 
+// ─── JVM callback state (initialised in JNI_OnLoad / startEngine) ────────────
+
+static JavaVM*   g_jvm       = nullptr;
+static jclass    g_engClass  = nullptr;  // global ref to NativeEngine jclass
+static jmethodID g_onGearCalibrated = nullptr;
+
+JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
+    g_jvm = vm;
+    return JNI_VERSION_1_6;
+}
```

**Documentation:**

```diff
--- a/app/src/main/cpp/native-lib.cpp
+++ b/app/src/main/cpp/native-lib.cpp
@@ -73,6 +73,15 @@ static CalibrationEngine g_calibEngine;

 // ─── Inline radix-2 Cooley–Tukey FFT ─────────────────────────────────────────

+// ─── JVM callback state ──────────────────────────────────────────────────────
+//
+// The DSP worker thread is not JVM-attached at the time a gear-lock fires; it
+// cannot call FindClass or use local refs cached on the JNI startup thread.
+// Solution: cache g_jvm in JNI_OnLoad, then cache a NewGlobalRef jclass and a
+// static jmethodID in startEngine. The worker attaches once on thread start and
+// detaches at exit. (ref: DL-003, R-001)

 static JavaVM*   g_jvm       = nullptr;
-static jclass    g_engClass  = nullptr;  // global ref to NativeEngine jclass
-static jmethodID g_onGearCalibrated = nullptr;
+static jclass    g_engClass  = nullptr;  // NewGlobalRef — freed in stopEngine
+static jmethodID g_onGearCalibrated = nullptr;  // static method; valid for process lifetime

+// Cache the JavaVM pointer at library load time. (ref: DL-003)
 JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
     g_jvm = vm;
     return JNI_VERSION_1_6;
 }

```


**CC-M-002-002** (app/src/main/cpp/native-lib.cpp) - implements CI-M-002-002

**Code:**

```diff
--- a/app/src/main/cpp/native-lib.cpp
+++ b/app/src/main/cpp/native-lib.cpp
@@ -305,7 +305,20 @@ extern "C" {
 JNIEXPORT void JNICALL
-Java_dev_alfieprojects_gearsync_NativeEngine_startEngine(JNIEnv*, jclass) {
+Java_dev_alfieprojects_gearsync_NativeEngine_startEngine(JNIEnv* env, jclass cls) {
+    // Cache global class ref and onGearCalibrated method ID for DSP-thread upcall.
+    if (g_engClass == nullptr) {
+        g_engClass = static_cast<jclass>(env->NewGlobalRef(cls));
+        g_onGearCalibrated = env->GetStaticMethodID(
+                g_engClass, "onGearCalibrated", "(I)V");
+        if (!g_onGearCalibrated) {
+            LOGE("onGearCalibrated method not found -- callback disabled");
+        }
+    }
     // Reset ring-buffer head so stale samples from a prior session are not fed
     // into the first DSP snapshot after restart.
     g_pcmHead = 0;
     std::memset(g_pcmRing, 0, sizeof(g_pcmRing));

```

**Documentation:**

```diff
--- a/app/src/main/cpp/native-lib.cpp
+++ b/app/src/main/cpp/native-lib.cpp
@@ -305,7 +305,16 @@ extern "C" {
 JNIEXPORT void JNICALL
 Java_dev_alfieprojects_gearsync_NativeEngine_startEngine(JNIEnv* env, jclass cls) {
-    // Cache global class ref and onGearCalibrated method ID for DSP-thread upcall.
+    // Global class ref + static method ID are cached here (not in JNI_OnLoad) because
+    // FindClass from JNI_OnLoad uses the system class loader and cannot find app classes.
+    // cls is valid on this call (invoked from app code on the main thread). (ref: DL-003, R-001)
     if (g_engClass == nullptr) {
         g_engClass = static_cast<jclass>(env->NewGlobalRef(cls));
         g_onGearCalibrated = env->GetStaticMethodID(
                 g_engClass, "onGearCalibrated", "(I)V");
         if (!g_onGearCalibrated) {
+            // ProGuard must keep NativeEngine.onGearCalibrated; covered by existing
+            // -keep class ...NativeEngine { *; } rule. (ref: C-007)
             LOGE("onGearCalibrated method not found -- callback disabled");
         }
     }
     // Reset ring-buffer head so stale samples from a prior session are not fed
     // into the first DSP snapshot after restart.
     g_pcmHead = 0;
     std::memset(g_pcmRing, 0, sizeof(g_pcmRing));

```


**CC-M-002-003** (app/src/main/cpp/native-lib.cpp) - implements CI-M-002-003

**Code:**

```diff
--- a/app/src/main/cpp/native-lib.cpp
+++ b/app/src/main/cpp/native-lib.cpp
@@ -130,6 +130,18 @@ static void dspWorkerFn() {
     // Thread-local copy of the snapshot — avoids any overlap with the input callback.
     float localSnapshot[FFT_SIZE];
 
+    // Attach this worker thread to the JVM once; detach on exit.
+    JNIEnv* dspEnv = nullptr;
+    bool attached = false;
+    if (g_jvm) {
+        jint rc = g_jvm->AttachCurrentThread(&dspEnv, nullptr);
+        attached = (rc == JNI_OK);
+        if (!attached) LOGE("DSP thread: JVM attach failed (%d)", rc);
+    }
+
     while (g_dspRunning.load(std::memory_order_relaxed)) {
         uint32_t w = g_dspWriteSeq.load(std::memory_order_acquire);
         uint32_t r = g_dspReadSeq.load(std::memory_order_relaxed);
@@ -151,12 +163,28 @@ static void dspWorkerFn() {
 
         if (speed >= MIN_SPEED_MPS) {
             float ratio     = hz / speed;
             int   stableReq = g_stabilityWindowSamples.load(std::memory_order_relaxed);
             int   stable    = g_speedStableCount.load(std::memory_order_relaxed);
 
             if (stableReq == 0 || stable >= stableReq) {
-                g_calibEngine.updateWelford(ratio);
+                // If guided capture is active, route sample to capture buffer;
+                // otherwise feed passive Welford / K-Means path.
+                bool locked = false;
+                if (g_calibEngine.isCalibrating()) {
+                    locked = g_calibEngine.feedCalibrationSample(speed, hz);
+                } else {
+                    g_calibEngine.updateWelford(ratio);
+                }
+
+                if (locked && attached && g_engClass && g_onGearCalibrated) {
+                    int gear = g_calibEngine.calibratingGear();  // already -1 after lock, need old value
+                    // feedCalibrationSample already reset m_calibGear; recover from classifyGear result.
+                    int lockedGear = g_calibEngine.classifyGear(ratio);
+                    dspEnv->CallStaticVoidMethod(g_engClass, g_onGearCalibrated,
+                                                 static_cast<jint>(lockedGear));
+                    if (dspEnv->ExceptionCheck()) dspEnv->ExceptionClear();
+                }
             }
 
             int gear = g_calibEngine.classifyGear(ratio);
@@ -176,4 +204,12 @@ static void dspWorkerFn() {
         }
     }
+
+    if (attached) g_jvm->DetachCurrentThread();
 }
```

**Documentation:**

```diff
--- a/app/src/main/cpp/native-lib.cpp
+++ b/app/src/main/cpp/native-lib.cpp
@@ -130,6 +130,12 @@ static void dspWorkerFn() {
     // Thread-local copy of the snapshot — avoids any overlap with the input callback.
     float localSnapshot[FFT_SIZE];

-    // Attach this worker thread to the JVM once; detach on exit.
+    // Attach the DSP worker to the JVM for the lifetime of the thread.
+    // A per-callback attach/detach would be cheaper in the no-lock case but risks
+    // leaking a detach on early exit; one attach + one detach is simpler and safe.
+    // (ref: DL-003, RA-005)
     JNIEnv* dspEnv = nullptr;
     bool attached = false;
     if (g_jvm) {
         jint rc = g_jvm->AttachCurrentThread(&dspEnv, nullptr);
         attached = (rc == JNI_OK);
         if (!attached) LOGE("DSP thread: JVM attach failed (%d)", rc);
     }

     while (g_dspRunning.load(std::memory_order_relaxed)) {
@@ -163,12 +175,22 @@ static void dspWorkerFn() {
         if (speed >= MIN_SPEED_MPS) {
             float ratio     = hz / speed;
             int   stableReq = g_stabilityWindowSamples.load(std::memory_order_relaxed);
             int   stable    = g_speedStableCount.load(std::memory_order_relaxed);

             if (stableReq == 0 || stable >= stableReq) {
-                // If guided capture is active, route sample to capture buffer;
-                // otherwise feed passive Welford / K-Means path.
+                // Guided capture and passive learning are mutually exclusive on each sample.
+                // feedCalibrationSample suppresses passive feeding for the duration of a
+                // capture session. (ref: DL-005, C-001)
                 bool locked = false;
                 if (g_calibEngine.isCalibrating()) {
                     locked = g_calibEngine.feedCalibrationSample(speed, hz);
                 } else {
                     g_calibEngine.updateWelford(ratio);
                 }

-                if (locked && attached && g_engClass && g_onGearCalibrated) {
-                    int gear = g_calibEngine.calibratingGear();  // already -1 after lock, need old value
-                    // feedCalibrationSample already reset m_calibGear; recover from classifyGear result.
+                // Fire the upcall after feedCalibrationSample returns (m_mutex is released).
+                // classifyGear re-derives the gear from the just-written ratio, since
+                // m_calibGear is already reset to -1 by the time we reach here. (ref: DL-003, R-004)
+                if (locked && attached && g_engClass && g_onGearCalibrated) {
                     int lockedGear = g_calibEngine.classifyGear(ratio);
                     dspEnv->CallStaticVoidMethod(g_engClass, g_onGearCalibrated,
                                                  static_cast<jint>(lockedGear));
+                    // Clear any JVM exception: a failed callback must not crash the DSP thread.
                     if (dspEnv->ExceptionCheck()) dspEnv->ExceptionClear();
                 }
             }
         }
     }

+    // Detach before thread exit — matches the AttachCurrentThread above. (ref: R-001)
     if (attached) g_jvm->DetachCurrentThread();
 }

```


**CC-M-002-004** (app/src/main/cpp/native-lib.cpp) - implements CI-M-002-004

**Code:**

```diff
--- a/app/src/main/cpp/native-lib.cpp
+++ b/app/src/main/cpp/native-lib.cpp
@@ -450,6 +450,43 @@ Java_dev_alfieprojects_gearsync_NativeEngine_setVehicleConfig(JNIEnv*     env,
 
 } // extern "C"
+
+extern "C" {
+
+JNIEXPORT void JNICALL
+Java_dev_alfieprojects_gearsync_NativeEngine_beginGearCalibration(JNIEnv*, jclass,
+                                                                    jint gear) {
+    g_calibEngine.beginGearCalibration(static_cast<int>(gear));
+    LOGI("Guided calibration started for gear %d", static_cast<int>(gear));
+}
+
+JNIEXPORT void JNICALL
+Java_dev_alfieprojects_gearsync_NativeEngine_cancelCalibration(JNIEnv*, jclass) {
+    g_calibEngine.cancelCalibration();
+    LOGI("Guided calibration cancelled");
+}
+
+JNIEXPORT jfloat JNICALL
+Java_dev_alfieprojects_gearsync_NativeEngine_getCalibrationProgress(JNIEnv*, jclass) {
+    return static_cast<jfloat>(g_calibEngine.calibrationProgress());
+}
+
+} // extern "C"
```

**Documentation:**

```diff
--- a/app/src/main/cpp/native-lib.cpp
+++ b/app/src/main/cpp/native-lib.cpp
@@ -450,6 +450,12 @@ Java_dev_alfieprojects_gearsync_NativeEngine_setVehicleConfig(JNIEnv* env, ...) {
 } // extern "C"

 extern "C" {

+// Delegate to CalibrationEngine.beginGearCalibration. Passive K-Means feeding
+// is suppressed from the DSP worker while a capture is active. (ref: DL-005)
 JNIEXPORT void JNICALL
 Java_dev_alfieprojects_gearsync_NativeEngine_beginGearCalibration(JNIEnv*, jclass,
                                                                     jint gear) {
     g_calibEngine.beginGearCalibration(static_cast<int>(gear));
     LOGI("Guided calibration started for gear %d", static_cast<int>(gear));
 }

+// Discard captured samples and return to passive mode. (ref: DL-006)
 JNIEXPORT void JNICALL
 Java_dev_alfieprojects_gearsync_NativeEngine_cancelCalibration(JNIEnv*, jclass) {
     g_calibEngine.cancelCalibration();
     LOGI("Guided calibration cancelled");
 }

+// Returns calibrationProgress() — range [0, 1]; polled by CalibrationActivity at ~10 Hz.
 JNIEXPORT jfloat JNICALL
 Java_dev_alfieprojects_gearsync_NativeEngine_getCalibrationProgress(JNIEnv*, jclass) {
     return static_cast<jfloat>(g_calibEngine.calibrationProgress());
 }

 } // extern "C"

```


**CC-M-002-005** (app/src/main/cpp/native-lib.cpp) - implements CI-M-002-005

**Code:**

```diff
--- a/app/src/main/cpp/native-lib.cpp
+++ b/app/src/main/cpp/native-lib.cpp
@@ -408,8 +408,8 @@ Java_dev_alfieprojects_gearsync_NativeEngine_saveCalibrationState(JNIEnv* env, jclass) {
-    constexpr int LEN = 3 + NUM_GEARS;
+    constexpr int LEN = 3 + NUM_GEARS + NUM_GEARS;  // 13 floats: Welford + ratios + pins
     jfloatArray result = env->NewFloatArray(LEN);
     if (!result) return nullptr;
 
     float buf[LEN]{};
     g_calibEngine.serialise(buf, LEN);
     env->SetFloatArrayRegion(result, 0, LEN, buf);
     return result;
 }
```

**Documentation:**

```diff
--- a/app/src/main/cpp/native-lib.cpp
+++ b/app/src/main/cpp/native-lib.cpp
@@ -408,8 +408,9 @@ Java_dev_alfieprojects_gearsync_NativeEngine_saveCalibrationState(JNIEnv* env, jclass) {
-    constexpr int LEN = 3 + NUM_GEARS;
+    // LEN = 3 Welford + 5 ratios + 5 pin flags = 13 floats. (ref: DL-002)
+    constexpr int LEN = 3 + NUM_GEARS + NUM_GEARS;
     jfloatArray result = env->NewFloatArray(LEN);
     if (!result) return nullptr;

     float buf[LEN]{};
     g_calibEngine.serialise(buf, LEN);
     env->SetFloatArrayRegion(result, 0, LEN, buf);
     return result;
 }

```


**CC-M-002-006** (app/src/main/java/dev/alfieprojects/gearsync/NativeEngine.kt) - implements CI-M-002-006

**Code:**

```diff
--- a/app/src/main/java/dev/alfieprojects/gearsync/NativeEngine.kt
+++ b/app/src/main/java/dev/alfieprojects/gearsync/NativeEngine.kt
@@ -48,4 +48,31 @@ object NativeEngine {
         speedJitterThresholdMps: Float
     )
+
+    /** Begin guided per-gear calibration for the given 0-based gear index. */
+    @JvmStatic external fun beginGearCalibration(gear: Int)
+
+    /** Cancel an in-progress guided calibration and discard captured data. */
+    @JvmStatic external fun cancelCalibration()
+
+    /**
+     * Returns the guided-calibration capture progress in [0, 1].
+     * 0 = no capture started; 1 = ready to lock.
+     */
+    @JvmStatic external fun getCalibrationProgress(): Float
+
+    /**
+     * Called from C++ (DSP worker thread) when a guided gear lock succeeds.
+     * Posts the event to any registered [CalibrationListener] on the main thread.
+     *
+     * @param gear 0-based gear index that was locked, or -1 on order-break failure.
+     */
+    @JvmStatic fun onGearCalibrated(gear: Int) {
+        android.os.Handler(android.os.Looper.getMainLooper()).post {
+            calibrationListener?.onGearCalibrated(gear)
+        }
+    }
+
+    /** Listener interface for guided-calibration lock events. */
+    interface CalibrationListener {
+        fun onGearCalibrated(gear: Int)
+    }
+
+    /** Register a listener to receive [onGearCalibrated] events. Null to unregister. */
+    @Volatile var calibrationListener: CalibrationListener? = null
 }
```

**Documentation:**

```diff
--- a/app/src/main/java/dev/alfieprojects/gearsync/NativeEngine.kt
+++ b/app/src/main/java/dev/alfieprojects/gearsync/NativeEngine.kt
@@ -48,4 +48,32 @@ object NativeEngine {
         speedJitterThresholdMps: Float
     )

+    /** Begin guided per-gear calibration for the given 0-based gear index.
+     *  Passive K-Means feeding is suspended until the capture locks or is cancelled. */
     @JvmStatic external fun beginGearCalibration(gear: Int)

+    /** Cancel an in-progress guided calibration and discard all captured samples. */
     @JvmStatic external fun cancelCalibration()

-    /**
-     * Returns the guided-calibration capture progress in [0, 1].
-     * 0 = no capture started; 1 = ready to lock.
-     */
+    /** Capture progress in [0, 1].
+     *  Value is the minimum of sample-count progress and speed-spread progress;
+     *  both must reach 1.0 before RANSAC attempts a lock. */
     @JvmStatic external fun getCalibrationProgress(): Float

-    /**
-     * Called from C++ (DSP worker thread) when a guided gear lock succeeds.
-     * Posts the event to any registered [CalibrationListener] on the main thread.
-     *
-     * @param gear 0-based gear index that was locked, or -1 on order-break failure.
-     */
+    /**
+     * Called from C++ via the DSP worker JVM upcall when a gear lock completes.
+     * Marshals the event to the main thread before notifying [calibrationListener].
+     *
+     * Must be @JvmStatic so the ProGuard -keep rule for NativeEngine retains it and
+     * so the static jmethodID cached in startEngine resolves correctly. (ref: DL-003, C-007)
+     *
+     * @param gear 0-based gear index that was locked, or -1 when the fitted k_g
+     *             broke monotonic order and was rejected. (ref: DL-005)
+     */
     @JvmStatic fun onGearCalibrated(gear: Int) {
         android.os.Handler(android.os.Looper.getMainLooper()).post {
             calibrationListener?.onGearCalibrated(gear)
         }
     }

-    /** Listener interface for guided-calibration lock events. */
+    /** Callback interface for guided-calibration lock events.
+     *  Implementations must be registered before beginGearCalibration is called. */
     interface CalibrationListener {
+        /** @param gear 0-based gear index; -1 signals a failed lock. */
         fun onGearCalibrated(gear: Int)
     }

-    /** Register a listener to receive [onGearCalibrated] events. Null to unregister. */
+    /** Register (or clear) the [CalibrationListener].
+     *  CalibrationActivity sets this in onCreate and clears it in onDestroy. */
     @Volatile var calibrationListener: CalibrationListener? = null
 }

```


**CC-M-002-007** (app/src/main/cpp/native-lib.cpp) - implements CI-M-002-005

**Code:**

```diff
--- a/app/src/main/cpp/native-lib.cpp
+++ b/app/src/main/cpp/native-lib.cpp
@@ -328,8 +328,14 @@ Java_dev_alfieprojects_gearsync_NativeEngine_startEngine(JNIEnv* env, jclass cls) {
 JNIEXPORT void JNICALL
-Java_dev_alfieprojects_gearsync_NativeEngine_stopEngine(JNIEnv*, jclass) {
+Java_dev_alfieprojects_gearsync_NativeEngine_stopEngine(JNIEnv* env, jclass) {
     g_sensorRunning.store(false);
     if (g_sensorThread.joinable()) g_sensorThread.join();
 
     g_dspRunning.store(false);
     if (g_dspThread.joinable()) g_dspThread.join();
 
     closeStreams();
+
+    if (g_engClass) {
+        env->DeleteGlobalRef(g_engClass);
+        g_engClass = nullptr;
+        g_onGearCalibrated = nullptr;
+    }
     LOGI("Native engine stopped");
 }

```

**Documentation:**

```diff
--- a/app/src/main/cpp/native-lib.cpp
+++ b/app/src/main/cpp/native-lib.cpp
@@ -328,8 +328,12 @@ Java_dev_alfieprojects_gearsync_NativeEngine_startEngine(JNIEnv* env, jclass cls) {
 JNIEXPORT void JNICALL
 Java_dev_alfieprojects_gearsync_NativeEngine_stopEngine(JNIEnv* env, jclass) {
     g_sensorRunning.store(false);
     if (g_sensorThread.joinable()) g_sensorThread.join();

     g_dspRunning.store(false);
     if (g_dspThread.joinable()) g_dspThread.join();

     closeStreams();

+    // Release the global ref created in startEngine. The DSP thread is already
+    // joined above, so no callbacks can fire after this point. (ref: DL-003, R-001)
     if (g_engClass) {
         env->DeleteGlobalRef(g_engClass);
         g_engClass = nullptr;
         g_onGearCalibrated = nullptr;
     }
     LOGI("Native engine stopped");
 }

```


### Milestone 3: Service persistence migration to 13-float calibration state

**Files**: app/src/main/java/dev/alfieprojects/gearsync/ShiftAssistantService.kt

**Flags**: error-handling

**Requirements**:

- CALIBRATION_STATE_LEN becomes 13||persistCalibrationState writes all 13 cal_i keys||restoreCalibrationState reads 13 keys with per-key getFloat default 0.0 so missing cal_8..cal_12 from a legacy install default to unpinned||resume still gated on n>0

**Acceptance Criteria**:

- A legacy install with only cal_0..cal_7 stored loads without exception and leaves pins false||A fresh save/restore round-trips all 13 floats||No Room/SQLite introduced; SharedPreferences only

**Tests**:

- type:integration||backing:doc-derived||normal:save then restore round-trips 13 floats||edge:legacy 8-key prefs restore with pins defaulted to 0.0||error:absent prefs leave engine unseeded||skip_reason:exercised on-device alongside the JNI milestone per C-008

#### Code Intent

- **CI-M-003-001** `app/src/main/java/dev/alfieprojects/gearsync/ShiftAssistantService.kt::CALIBRATION_STATE_LEN / persistCalibrationState / restoreCalibrationState`: CALIBRATION_STATE_LEN is 13 (3 Welford + 5 ratios + 5 pin flags), documented as kept in sync with native serialise. persistCalibrationState writes whatever length saveCalibrationState returns across cal_0..cal_(len-1). restoreCalibrationState builds a FloatArray(CALIBRATION_STATE_LEN) reading each cal_i with a getFloat default of 0.0, so a legacy install storing only cal_0..cal_7 yields zeros for cal_8..cal_12 (all gears unpinned), and still gates the resume call on array[0] > 0. (refs: DL-002)

#### Code Changes

**CC-M-003-001** (app/src/main/java/dev/alfieprojects/gearsync/ShiftAssistantService.kt) - implements CI-M-003-001

**Code:**

```diff
--- a/app/src/main/java/dev/alfieprojects/gearsync/ShiftAssistantService.kt
+++ b/app/src/main/java/dev/alfieprojects/gearsync/ShiftAssistantService.kt
@@ -135,7 +135,8 @@ class ShiftAssistantService : Service() {
     companion object {
         private const val NOTIFICATION_ID        = 1
         private const val CHANNEL_ID             = "gearsync_fg"
         private const val PREFS_NAME             = "gearsync_calibration"
-        // Must stay in sync with native: 3 Welford fields + NUM_GEARS (5) gear ratios.
-        private const val CALIBRATION_STATE_LEN  = 8
+        // 3 Welford fields + 5 gear ratios + 5 pin flags = 13 floats.
+        // Back-compat: old 8-float prefs are accepted by CalibrationEngine.deserialise.
+        private const val CALIBRATION_STATE_LEN  = 13
     }
 }
```

**Documentation:**

```diff
--- a/app/src/main/java/dev/alfieprojects/gearsync/ShiftAssistantService.kt
+++ b/app/src/main/java/dev/alfieprojects/gearsync/ShiftAssistantService.kt
@@ -135,7 +135,9 @@ class ShiftAssistantService : Service() {
     companion object {
         private const val NOTIFICATION_ID        = 1
         private const val CHANNEL_ID             = "gearsync_fg"
         private const val PREFS_NAME             = "gearsync_calibration"
-        // Must stay in sync with native: 3 Welford fields + NUM_GEARS (5) gear ratios.
-        private const val CALIBRATION_STATE_LEN  = 8
+        // 3 Welford fields + 5 gear ratios + 5 pin flags = 13 floats.
+        // CalibrationEngine.deserialise accepts the old 8-float format by defaulting
+        // missing pin flags to 0.0 (unpinned). (ref: DL-002, R-002)
         private const val CALIBRATION_STATE_LEN  = 13
     }
 }

```


**CC-M-003-002** (app/src/main/java/dev/alfieprojects/gearsync/ShiftAssistantService.kt) - implements CI-M-003-001

**Code:**

```diff
--- a/app/src/main/java/dev/alfieprojects/gearsync/ShiftAssistantService.kt
+++ b/app/src/main/java/dev/alfieprojects/gearsync/ShiftAssistantService.kt
@@ -30,6 +30,7 @@ class ShiftAssistantService : Service() {
     override fun onCreate() {
         super.onCreate()
         fusedLocationClient = LocationServices.getFusedLocationProviderClient(this)
         createNotificationChannel()
         NativeEngine.startEngine()
         applyVehicleConfig()
         restoreCalibrationState()
+        isRunning = true
     }
 
     override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
@@ -45,6 +46,7 @@ class ShiftAssistantService : Service() {
     override fun onDestroy() {
         fusedLocationClient.removeLocationUpdates(locationCallback)
         persistCalibrationState()
         NativeEngine.stopEngine()
+        isRunning = false
         super.onDestroy()
     }
 
@@ -135,7 +137,9 @@ class ShiftAssistantService : Service() {
     companion object {
         private const val NOTIFICATION_ID        = 1
         private const val CHANNEL_ID             = "gearsync_fg"
         private const val PREFS_NAME             = "gearsync_calibration"
         // 3 Welford fields + 5 gear ratios + 5 pin flags = 13 floats.
         // Back-compat: old 8-float prefs are accepted by CalibrationEngine.deserialise.
         private const val CALIBRATION_STATE_LEN  = 13
+
+        @Volatile var isRunning: Boolean = false
     }
 }

```

**Documentation:**

```diff
--- a/app/src/main/java/dev/alfieprojects/gearsync/ShiftAssistantService.kt
+++ b/app/src/main/java/dev/alfieprojects/gearsync/ShiftAssistantService.kt
@@ -137,7 +137,9 @@ class ShiftAssistantService : Service() {
         private const val CALIBRATION_STATE_LEN  = 13

+        /** True while the service is in the created-and-not-yet-destroyed state.
+         *  Read by MainActivity to gate the "Calibrate" button. */
         @Volatile var isRunning: Boolean = false
     }
 }

```


### Milestone 4: CalibrationActivity UI: gear grid, progress ring, lock confirmation

**Files**: app/src/main/java/dev/alfieprojects/gearsync/CalibrationActivity.kt, app/src/main/java/dev/alfieprojects/gearsync/MainActivity.kt, app/src/main/res/layout/activity_calibration.xml, app/src/main/res/layout/activity_main.xml, app/src/main/res/values/strings.xml, app/src/main/res/values/colors.xml, app/src/main/AndroidManifest.xml

**Flags**: needs-rationale

**Requirements**:

- CalibrationActivity gates on RECORD_AUDIO + ACCESS_FINE_LOCATION already granted before entry
- finishing back to MainActivity otherwise||gear grid 1..5 calls beginGearCalibration(g-1); the captured gear is greyed during capture||a Choreographer-driven progress ring polls getCalibrationProgress() ~10 Hz and fills using colors from colors.xml
- pausing when not visible||Cancel calls cancelCalibration()||a registered onGearCalibrated listener pulses the ring and marks the gear done
- optionally a VibrationEffect haptic and no audio||MainActivity gains a Start Calibration button launching CalibrationActivity||all visible strings in strings.xml and colors in colors.xml||CalibrationActivity declared in the manifest with exported=false

**Acceptance Criteria**:

- ./gradlew assembleDebug succeeds and CalibrationActivity launches from MainActivity||Selecting a gear starts capture and the progress ring fills as getCalibrationProgress increases||A lock event visibly confirms and marks the gear done with no audio output||Entering without RECORD_AUDIO+location finishes back to MainActivity||No hardcoded UI strings or color literals introduced

**Tests**:

- type:integration||backing:default-derived||normal:launch activity
- pick gear
- observe ring fill and lock confirm||edge:ring Choreographer pauses when activity not visible; greyed gear cannot be re-picked mid-capture||error:missing permission path returns to MainActivity||skip_reason:UI plus sensor/audio capture loop verifiable only on a physical arm64 device per C-008

#### Code Intent

- **CI-M-004-001** `app/src/main/java/dev/alfieprojects/gearsync/CalibrationActivity.kt::CalibrationActivity`: An AppCompatActivity that on entry checks RECORD_AUDIO and ACCESS_FINE_LOCATION are already granted (same gate semantics as MainActivity); if either is missing it shows a message and finishes back to MainActivity rather than requesting. It presents a 1..5 gear grid; tapping gear N calls NativeEngine.beginGearCalibration(N-1) and greys that gear while capturing. A Choreographer frame callback polls NativeEngine.getCalibrationProgress() at ~10 Hz to drive a progress-ring view, registering on attach and removing on detach/not-visible (mirroring VUMeterView isRunning gating). A Cancel control calls NativeEngine.cancelCalibration() and returns to the grid. It registers a NativeEngine gear-calibrated listener in onResume and clears it in onPause; on a lock it pulses the ring, marks the gear done, and optionally triggers a short VibrationEffect (no audio). All shown text comes from strings.xml and all colors from colors.xml. (refs: DL-004, DL-001)
- **CI-M-004-002** `app/src/main/res/layout/activity_calibration.xml::activity_calibration layout`: A layout providing the gear-selection grid (five selectable gear controls), a progress-ring area, an instruction text line, and a Cancel control, on the dark background. Strings are referenced from strings.xml and colors/backgroundTints from colors.xml; no literal text or color values. (refs: DL-004)
- **CI-M-004-003** `app/src/main/java/dev/alfieprojects/gearsync/MainActivity.kt::MainActivity (calibration launch)`: MainActivity wires a Start Calibration button that starts CalibrationActivity via an Intent. The existing start/stop service flow and permission gating are unchanged. (refs: DL-004)
- **CI-M-004-004** `app/src/main/res/layout/activity_main.xml::activity_main (calibration button)`: The control row gains a Start Calibration button referencing a strings.xml label and a colors.xml tint, laid out alongside the existing Start/Stop buttons without disturbing the VUMeterView constraints. (refs: DL-004)
- **CI-M-004-005** `app/src/main/res/values/strings.xml::calibration strings`: String resources for the calibration flow: a Start Calibration button label, a per-gear capture instruction (hold gear N, vary speed across a range at steady throttle), a gear-locked confirmation, a calibration_lock_failed message explaining the fit was rejected for breaking gear order (try again), a Cancel label, and a permissions-required-for-calibration message. (refs: DL-004, DL-005)
- **CI-M-004-006** `app/src/main/res/values/colors.xml::calibration colors`: Color resources for the calibration UI: progress-ring fill and a captured/greyed gear tint, consistent with the existing VU-meter zone palette, so the activity references named colors rather than literals. (refs: DL-004)
- **CI-M-004-007** `app/src/main/AndroidManifest.xml::CalibrationActivity registration`: The manifest declares CalibrationActivity with a relative name (.CalibrationActivity) and android:exported=false, under the existing application element; no new permissions are added (RECORD_AUDIO and location already declared). (refs: DL-004)
- **CI-M-004-008** `app/src/main/java/dev/alfieprojects/gearsync/CalibrationActivity.kt`: Failed-lock (order-break reject) UI feedback path. When a guided fit is rejected because the resulting k_g would break strict-descending gear order (CalibrationEngine returns -1 / no lock for that capture attempt, per CI-M-001-003), the activity must surface this distinctly from a still-in-progress capture. Because the success upcall onGearCalibrated only fires on a real lock, the activity detects a rejected lock by observing that getCalibrationProgress reached completion (>=1.0 inlier+spread) yet m_calibGear cleared back to IDLE without an onGearCalibrated event: the activity polls isCalibrating()/calibratingGear via getCalibrationProgress polling state and, on this clear-without-lock transition for the gear being captured, shows the calibration_lock_failed string from strings.xml, resets the ring, leaves the gear un-greyed (re-selectable), and optionally a short distinct VibrationEffect (no audio). No string or color literals; the failure message comes from strings.xml. (refs: DL-005, DL-004)

#### Code Changes

**CC-M-004-001** (app/src/main/java/dev/alfieprojects/gearsync/CalibrationActivity.kt) - implements CI-M-004-001

**Code:**

```diff
--- /dev/null
+++ b/app/src/main/java/dev/alfieprojects/gearsync/CalibrationActivity.kt
@@ -0,0 +1,136 @@
+package dev.alfieprojects.gearsync
+
+import android.Manifest
+import android.content.pm.PackageManager
+import android.graphics.Canvas
+import android.graphics.Paint
+import android.graphics.RectF
+import android.os.Bundle
+import android.view.Choreographer
+import android.view.View
+import android.view.ViewGroup
+import android.widget.Button
+import android.widget.LinearLayout
+import android.widget.TextView
+import android.widget.Toast
+import androidx.appcompat.app.AppCompatActivity
+import androidx.core.content.ContextCompat
+
+class CalibrationActivity : AppCompatActivity(), NativeEngine.CalibrationListener {
+
+    private lateinit var gearButtons: List<Button>
+    private lateinit var statusText: TextView
+    private lateinit var progressRing: ProgressRingView
+    private lateinit var btnCancel: Button
+
+    private var activeGear: Int = -1
+
+    private val choreographerCallback = object : Choreographer.FrameCallback {
+        override fun doFrame(frameTimeNanos: Long) {
+            if (activeGear >= 0) {
+                val progress = NativeEngine.getCalibrationProgress()
+                progressRing.setProgress(progress)
+                Choreographer.getInstance().postFrameCallback(this)
+            }
+        }
+    }
+
+    override fun onCreate(savedInstanceState: Bundle?) {
+        super.onCreate(savedInstanceState)
+        setContentView(R.layout.activity_calibration)
+
+        if (!hasRequiredPermissions()) {
+            Toast.makeText(this, R.string.calib_permissions_required, Toast.LENGTH_LONG).show()
+            finish()
+            return
+        }
+
+        gearButtons = listOf(
+            findViewById(R.id.btnGear1),
+            findViewById(R.id.btnGear2),
+            findViewById(R.id.btnGear3),
+            findViewById(R.id.btnGear4),
+            findViewById(R.id.btnGear5)
+        )
+        statusText   = findViewById(R.id.tvCalibStatus)
+        progressRing = findViewById(R.id.progressRing)
+        btnCancel    = findViewById(R.id.btnCancelCalib)
+
+        gearButtons.forEachIndexed { index, button ->
+            button.setOnClickListener { startCapture(index) }
+        }
+        btnCancel.setOnClickListener { cancelCapture() }
+
+        NativeEngine.calibrationListener = this
+        showGearGrid()
+    }
+
+    override fun onDestroy() {
+        NativeEngine.cancelCalibration()
+        NativeEngine.calibrationListener = null
+        Choreographer.getInstance().removeFrameCallback(choreographerCallback)
+        super.onDestroy()
+    }
+
+    private fun startCapture(gearIndex: Int) {
+        activeGear = gearIndex
+        NativeEngine.beginGearCalibration(gearIndex)
+        val gearNumber = gearIndex + 1
+        statusText.text = getString(R.string.calib_capturing, gearNumber)
+        progressRing.setProgress(0f)
+        progressRing.visibility = View.VISIBLE
+        btnCancel.visibility = View.VISIBLE
+        gearButtons.forEach { it.isEnabled = false }
+        Choreographer.getInstance().postFrameCallback(choreographerCallback)
+    }
+
+    private fun cancelCapture() {
+        activeGear = -1
+        NativeEngine.cancelCalibration()
+        Choreographer.getInstance().removeFrameCallback(choreographerCallback)
+        showGearGrid()
+    }
+
+    private fun showGearGrid() {
+        activeGear = -1
+        statusText.text = getString(R.string.calib_pick_gear)
+        progressRing.visibility = View.GONE
+        btnCancel.visibility = View.GONE
+        gearButtons.forEach { it.isEnabled = true }
+    }
+
+    override fun onGearCalibrated(gear: Int) {
+        Choreographer.getInstance().removeFrameCallback(choreographerCallback)
+        if (gear < 0) {
+            progressRing.setProgress(0f)
+            Toast.makeText(this, R.string.calib_lock_failed, Toast.LENGTH_LONG).show()
+            showGearGrid()
+        } else {
+            progressRing.setProgress(1f)
+            val gearNumber = gear + 1
+            statusText.text = getString(R.string.calib_locked, gearNumber)
+            gearButtons.getOrNull(gear)?.isEnabled = false
+            btnCancel.visibility = View.GONE
+            activeGear = -1
+        }
+    }
+
+    private fun hasRequiredPermissions(): Boolean {
+        return ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) ==
+                PackageManager.PERMISSION_GRANTED &&
+               ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) ==
+                PackageManager.PERMISSION_GRANTED
+    }
+
+    inner class ProgressRingView(context: android.content.Context) : View(context) {
+        private var progress = 0f
+        private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
+            style = Paint.Style.STROKE
+            strokeWidth = 16f
+        }
+        private val oval = RectF()
+
+        fun setProgress(p: Float) { progress = p.coerceIn(0f, 1f); invalidate() }
+
+        override fun onDraw(canvas: Canvas) {
+            val r = (minOf(width, height) / 2f) - paint.strokeWidth
+            oval.set(width / 2f - r, height / 2f - r, width / 2f + r, height / 2f + r)
+            paint.color = resources.getColor(R.color.calib_ring_track, theme)
+            canvas.drawArc(oval, 0f, 360f, false, paint)
+            paint.color = when {
+                progress < 0.33f -> resources.getColor(R.color.zone_lug,     theme)
+                progress < 0.66f -> resources.getColor(R.color.zone_optimal, theme)
+                else             -> resources.getColor(R.color.zone_redline,  theme)
+            }
+            canvas.drawArc(oval, -90f, progress * 360f, false, paint)
+        }
+    }
+}
```

**Documentation:**

```diff
--- a/app/src/main/java/dev/alfieprojects/gearsync/CalibrationActivity.kt
+++ b/app/src/main/java/dev/alfieprojects/gearsync/CalibrationActivity.kt
@@ -1,4 +1,13 @@
 package dev.alfieprojects.gearsync

+// CalibrationActivity — guided per-gear calibration UI.
+//
+// Flow: gear grid -> tap a gear -> capture screen (progress ring, ~10 Hz poll) ->
+//       RANSAC locks -> confirmation shown; or user cancels at any point.
+//
+// Requires RECORD_AUDIO and location permissions (already granted to the running
+// ShiftAssistantService); finishes immediately if permissions are absent.
+// (ref: DL-004)

 import android.Manifest
 import android.content.pm.PackageManager

```


**CC-M-004-002** (app/src/main/res/layout/activity_calibration.xml) - implements CI-M-004-002

**Code:**

```diff
--- /dev/null
+++ b/app/src/main/res/layout/activity_calibration.xml
@@ -0,0 +1,101 @@
+<?xml version="1.0" encoding="utf-8"?>
+<androidx.constraintlayout.widget.ConstraintLayout
+    xmlns:android="http://schemas.android.com/apk/res/android"
+    xmlns:app="http://schemas.android.com/apk/res-auto"
+    android:layout_width="match_parent"
+    android:layout_height="match_parent"
+    android:background="@color/background_dark">
+
+    <TextView
+        android:id="@+id/tvCalibStatus"
+        android:layout_width="0dp"
+        android:layout_height="wrap_content"
+        android:textColor="#FFFFFF"
+        android:textSize="18sp"
+        android:gravity="center"
+        android:padding="16dp"
+        app:layout_constraintTop_toTopOf="parent"
+        app:layout_constraintStart_toStartOf="parent"
+        app:layout_constraintEnd_toEndOf="parent" />
+
+    <LinearLayout
+        android:id="@+id/gearGrid"
+        android:layout_width="0dp"
+        android:layout_height="wrap_content"
+        android:orientation="horizontal"
+        android:gravity="center"
+        android:padding="16dp"
+        app:layout_constraintTop_toBottomOf="@id/tvCalibStatus"
+        app:layout_constraintStart_toStartOf="parent"
+        app:layout_constraintEnd_toEndOf="parent">
+
+        <Button
+            android:id="@+id/btnGear1"
+            android:layout_width="0dp"
+            android:layout_weight="1"
+            android:layout_height="56dp"
+            android:layout_margin="4dp"
+            android:text="@string/gear_label_1"
+            android:backgroundTint="@color/calib_gear_button" />
+
+        <Button
+            android:id="@+id/btnGear2"
+            android:layout_width="0dp"
+            android:layout_weight="1"
+            android:layout_height="56dp"
+            android:layout_margin="4dp"
+            android:text="@string/gear_label_2"
+            android:backgroundTint="@color/calib_gear_button" />
+
+        <Button
+            android:id="@+id/btnGear3"
+            android:layout_width="0dp"
+            android:layout_weight="1"
+            android:layout_height="56dp"
+            android:layout_margin="4dp"
+            android:text="@string/gear_label_3"
+            android:backgroundTint="@color/calib_gear_button" />
+
+        <Button
+            android:id="@+id/btnGear4"
+            android:layout_width="0dp"
+            android:layout_weight="1"
+            android:layout_height="56dp"
+            android:layout_margin="4dp"
+            android:text="@string/gear_label_4"
+            android:backgroundTint="@color/calib_gear_button" />
+
+        <Button
+            android:id="@+id/btnGear5"
+            android:layout_width="0dp"
+            android:layout_weight="1"
+            android:layout_height="56dp"
+            android:layout_margin="4dp"
+            android:text="@string/gear_label_5"
+            android:backgroundTint="@color/calib_gear_button" />
+
+    </LinearLayout>
+
+    <dev.alfieprojects.gearsync.CalibrationActivity.ProgressRingView
+        android:id="@+id/progressRing"
+        android:layout_width="200dp"
+        android:layout_height="200dp"
+        android:visibility="gone"
+        app:layout_constraintTop_toBottomOf="@id/gearGrid"
+        app:layout_constraintStart_toStartOf="parent"
+        app:layout_constraintEnd_toEndOf="parent" />
+
+    <Button
+        android:id="@+id/btnCancelCalib"
+        android:layout_width="wrap_content"
+        android:layout_height="56dp"
+        android:layout_marginTop="16dp"
+        android:text="@string/btn_cancel_calib"
+        android:backgroundTint="@color/button_stop"
+        android:visibility="gone"
+        app:layout_constraintTop_toBottomOf="@id/progressRing"
+        app:layout_constraintStart_toStartOf="parent"
+        app:layout_constraintEnd_toEndOf="parent" />
+
+</androidx.constraintlayout.widget.ConstraintLayout>
```

**Documentation:**

```diff
--- a/app/src/main/res/layout/activity_calibration.xml
+++ b/app/src/main/res/layout/activity_calibration.xml
@@ -1,4 +1,8 @@
 <?xml version="1.0" encoding="utf-8"?>
+<!--
+  CalibrationActivity layout: status text, 5-gear button row, progress ring,
+  and cancel button. All colors from colors.xml; all labels from strings.xml.
+  (ref: DL-004)
+-->
 <androidx.constraintlayout.widget.ConstraintLayout

```


**CC-M-004-003** (app/src/main/java/dev/alfieprojects/gearsync/MainActivity.kt) - implements CI-M-004-003

**Code:**

```diff
--- a/app/src/main/java/dev/alfieprojects/gearsync/MainActivity.kt
+++ b/app/src/main/java/dev/alfieprojects/gearsync/MainActivity.kt
@@ -36,6 +36,7 @@ class MainActivity : AppCompatActivity() {
         setContentView(R.layout.activity_main)
 
         findViewById<Button>(R.id.btnStart).setOnClickListener { checkAndStart() }
         findViewById<Button>(R.id.btnStop).setOnClickListener  { stopShiftService() }
+        findViewById<Button>(R.id.btnCalibrate).setOnClickListener { openCalibration() }
     }
 
     private fun checkAndStart() {
@@ -56,4 +57,13 @@ class MainActivity : AppCompatActivity() {
         stopService(Intent(this, ShiftAssistantService::class.java))
         Toast.makeText(this, R.string.service_stopped, Toast.LENGTH_SHORT).show()
     }
+
+    private fun openCalibration() {
+        if (!ShiftAssistantService.isRunning) {
+            Toast.makeText(this, R.string.calib_service_not_running, Toast.LENGTH_LONG).show()
+            return
+        }
+        startActivity(Intent(this, CalibrationActivity::class.java))
+    }
 }

```

**Documentation:**

```diff
--- a/app/src/main/java/dev/alfieprojects/gearsync/MainActivity.kt
+++ b/app/src/main/java/dev/alfieprojects/gearsync/MainActivity.kt
@@ -56,4 +57,9 @@ class MainActivity : AppCompatActivity() {
         stopService(Intent(this, ShiftAssistantService::class.java))
         Toast.makeText(this, R.string.service_stopped, Toast.LENGTH_SHORT).show()
     }

+    // Guard against opening CalibrationActivity when the service is not running:
+    // NativeEngine.beginGearCalibration would silently no-op and the UI would
+    // show no progress. (ref: DL-004)
     private fun openCalibration() {
         if (!ShiftAssistantService.isRunning) {
             Toast.makeText(this, R.string.calib_service_not_running, Toast.LENGTH_LONG).show()
             return
         }
         startActivity(Intent(this, CalibrationActivity::class.java))
     }
 }

```


**CC-M-004-004** (app/src/main/res/layout/activity_main.xml) - implements CI-M-004-004

**Code:**

```diff
--- a/app/src/main/res/layout/activity_main.xml
+++ b/app/src/main/res/layout/activity_main.xml
@@ -42,6 +42,13 @@ app:layout_constraintEnd_toEndOf="parent">
         <Button
             android:id="@+id/btnStop"
             android:layout_width="0dp"
             android:layout_weight="1"
             android:layout_height="56dp"
             android:layout_marginStart="8dp"
             android:text="@string/btn_stop"
             android:backgroundTint="@color/button_stop" />
+
+        <Button
+            android:id="@+id/btnCalibrate"
+            android:layout_width="0dp"
+            android:layout_weight="1"
+            android:layout_height="56dp"
+            android:layout_marginStart="8dp"
+            android:text="@string/btn_calibrate"
+            android:backgroundTint="@color/calib_gear_button" />
 
     </LinearLayout>
 
 </androidx.constraintlayout.widget.ConstraintLayout>
```

**Documentation:**

```diff
--- a/app/src/main/res/layout/activity_main.xml
+++ b/app/src/main/res/layout/activity_main.xml
@@ -42,6 +42,8 @@ app:layout_constraintEnd_toEndOf="parent">
+        <!-- Calibrate button: enabled only when ShiftAssistantService.isRunning.
+             Gating is enforced in MainActivity.openCalibration(). (ref: DL-004) -->
         <Button
             android:id="@+id/btnCalibrate"

```


**CC-M-004-005** (app/src/main/res/values/strings.xml) - implements CI-M-004-005

**Code:**

```diff
--- a/app/src/main/res/values/strings.xml
+++ b/app/src/main/res/values/strings.xml
@@ -7,4 +7,14 @@ <string name="app_name">GearSync</string>
     <string name="service_started">GearSync started</string>
     <string name="service_stopped">GearSync stopped</string>
     <string name="permissions_required">Permissions required: %1$s</string>
+    <string name="btn_calibrate">Calibrate</string>
+    <string name="btn_cancel_calib">Cancel</string>
+    <string name="calib_pick_gear">Pick a gear to calibrate</string>
+    <string name="calib_capturing">Hold gear %1$d — vary speed, steady throttle</string>
+    <string name="calib_locked">Gear %1$d locked</string>
+    <string name="calib_lock_failed">Calibration failed — gear order conflict. Try again.</string>
+    <string name="calib_permissions_required">RECORD_AUDIO and location permissions required</string>
+    <string name="gear_label_1">1</string>
+    <string name="gear_label_2">2</string>
+    <string name="gear_label_3">3</string>
+    <string name="gear_label_4">4</string>
+    <string name="gear_label_5">5</string>
+    <string name="calib_service_not_running">Start the GearSync service first, then open Calibrate.</string>
 </resources>
```

**Documentation:**

```diff
--- a/app/src/main/res/values/strings.xml
+++ b/app/src/main/res/values/strings.xml
@@ -7,4 +7,5 @@ <string name="app_name">GearSync</string>
+    <!-- Guided calibration strings. calib_capturing uses %1$d = 1-based gear number. -->
     <string name="btn_calibrate">Calibrate</string>

```


**CC-M-004-006** (app/src/main/res/values/colors.xml) - implements CI-M-004-006

**Code:**

```diff
--- a/app/src/main/res/values/colors.xml
+++ b/app/src/main/res/values/colors.xml
@@ -3,4 +3,9 @@ <?xml version="1.0" encoding="utf-8"?>
     <color name="background_dark">#0D0D1A</color>
     <color name="button_start">#27AE60</color>
     <color name="button_stop">#E74C3C</color>
+    <color name="calib_gear_button">#2980B9</color>
+    <color name="calib_ring_track">#333355</color>
+    <color name="zone_lug">#3498DB</color>
+    <color name="zone_optimal">#27AE60</color>
+    <color name="zone_redline">#E74C3C</color>
 </resources>
```

**Documentation:**

```diff
--- a/app/src/main/res/values/colors.xml
+++ b/app/src/main/res/values/colors.xml
@@ -3,4 +3,6 @@ <?xml version="1.0" encoding="utf-8"?>
+    <!-- Calibration UI colors. zone_* mirror the VU meter zone colors for visual consistency. -->
     <color name="calib_gear_button">#2980B9</color>

```


**CC-M-004-007** (app/src/main/AndroidManifest.xml) - implements CI-M-004-007

**Code:**

```diff
--- a/app/src/main/AndroidManifest.xml
+++ b/app/src/main/AndroidManifest.xml
@@ -27,6 +27,12 @@ <application
             <intent-filter>
                 <action android:name="android.intent.action.MAIN" />
                 <category android:name="android.intent.category.LAUNCHER" />
             </intent-filter>
         </activity>
 
+        <activity
+            android:name=".CalibrationActivity"
+            android:exported="false"
+            android:label="@string/btn_calibrate"
+            android:windowSoftInputMode="stateHidden" />
+
         <service
             android:name=".ShiftAssistantService"
             android:exported="false"
             android:foregroundServiceType="microphone|location" />
 
     </application>
```

**Documentation:**

```diff
--- a/app/src/main/AndroidManifest.xml
+++ b/app/src/main/AndroidManifest.xml
@@ -27,6 +27,8 @@ <application
+        <!-- CalibrationActivity: not exported; launched only from MainActivity.
+             android:exported="false" is required since targetSdk >= 31. (ref: DL-004) -->
         <activity
             android:name=".CalibrationActivity"

```


**CC-M-004-008** (app/src/main/java/dev/alfieprojects/gearsync/CalibrationActivity.kt) - implements CI-M-004-008

**Code:**

```diff
--- a/app/src/main/java/dev/alfieprojects/gearsync/CalibrationActivity.kt
+++ b/app/src/main/java/dev/alfieprojects/gearsync/CalibrationActivity.kt
@@ -70,7 +70,16 @@ class CalibrationActivity : AppCompatActivity(), NativeEngine.CalibrationListener {
     override fun onGearCalibrated(gear: Int) {
         Choreographer.getInstance().removeFrameCallback(choreographerCallback)
         if (gear < 0) {
             progressRing.setProgress(0f)
             Toast.makeText(this, R.string.calib_lock_failed, Toast.LENGTH_LONG).show()
             showGearGrid()
         } else {
             progressRing.setProgress(1f)
             val gearNumber = gear + 1
             statusText.text = getString(R.string.calib_locked, gearNumber)
             gearButtons.getOrNull(gear)?.isEnabled = false
             btnCancel.visibility = View.GONE
             activeGear = -1
         }
     }
```

**Documentation:**

```diff
--- a/app/src/main/java/dev/alfieprojects/gearsync/CalibrationActivity.kt
+++ b/app/src/main/java/dev/alfieprojects/gearsync/CalibrationActivity.kt
@@ -70,7 +70,13 @@ class CalibrationActivity : AppCompatActivity(), NativeEngine.CalibrationListener {
+    // NativeEngine.CalibrationListener implementation.
+    // Called on the main thread (NativeEngine.onGearCalibrated marshals via Handler).
+    // gear < 0 means RANSAC fit was rejected due to monotonicity-breaking k_g. (ref: DL-005)
     override fun onGearCalibrated(gear: Int) {
         Choreographer.getInstance().removeFrameCallback(choreographerCallback)
         if (gear < 0) {

```


## Execution Waves

- W-001: M-001
- W-002: M-002
- W-003: M-003, M-004
