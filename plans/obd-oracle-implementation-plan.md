# ADR 005 — Optional OBD-II Ground-Truth Oracle — Implementation Plan

Implements ADR 005 (`adr.md`). Optional, **default-off** OBD-II accessory used only for
(1) one-time calibration seeding and (2) offline fusion-error validation. The mic+accel
pipeline (ADR 001/002/004) stays the sole default and the only path required to ship.

> **Hard rule:** OBD data NEVER touches the native realtime path or the per-frame needle.
> All OBD work is Kotlin-side, off the audio/sensor threads, and writes only calibration
> state or a validation log. With the flag off, the app is byte-for-byte the current build.

## Key decisions

| ID | Decision | Rationale |
|----|----------|-----------|
| C-OBD-1 | **Milestone 0 is a hardware connectivity + PID probe spike, and all later work is gated on it.** | ELM327 clones vary wildly (BT Classic vs BLE, init quirks, "SEARCHING…"/"NO DATA", echo). Nothing downstream is worth building until RPM (PID `010C`) is confirmed readable on the *target vehicle* with a *specific adapter*. Mirrors ADR 004 M0. |
| C-OBD-2 | Default-off behind `vehicle_config.json` `useObdOracle=false`; mic+accel behaviour is unchanged when off. | Preserves the "no hardware required" tenet; OBD is an accessory, not a dependency. |
| C-OBD-3 | Kotlin-only; never the native realtime/audio/sensor path. No new native deps. | Keeps GC jitter off the DSP core (ADR 001) and the realtime contract intact. |
| C-OBD-4 | Bluetooth is runtime-permission-gated (`BLUETOOTH_CONNECT`, API 31+); absence → feature inert. | Modern Android BT permission model; must degrade silently. |
| DL-OBD-1 | Target **BT Classic SPP (RFCOMM, UUID `00001101-…`)** first; note BLE-only ELM327 variants exist as a later branch. | Most cheap ELM327 clones are BT Classic SPP. |
| DL-OBD-2 | Oracle writes calibration state via the **existing pinned-centroid path** (the 13-float SharedPreferences blob `pin0…pin4`), not a new realtime hook. | Reuses ADR 002 guided-calibration infrastructure; OBD-derived per-gear ratios are exactly "known good" pins. Avoids growing the 12↔12 JNI surface. |
| DL-OBD-3 | RPM→firing frequency uses the same `firingFactor` as the acoustic path: `f = RPM · firingFactor / 60`. | Keeps OBD ground-truth in the same units as `findDominantHz` / `f_vib`, so seeds and validation are directly comparable. |
| DL-OBD-4 | Validation logging defaults **debug-only**; shipping it in release is an open question deferred to M5. | Avoids a release BT/IO surface until proven useful. |

## Constraints

- C-OBD-1 hardware gate satisfied before any transport/seeding/validation code is reviewed.
- Pure-Kotlin ELM327 framing + PID parser must be **JVM-unit-testable** (no device): feed canned
  ELM327 byte strings, assert parsed RPM/speed. Only M0 and on-car validation need real hardware.
- No change to the realtime needle path; with the flag off, `git diff` of runtime behaviour is empty.

---

### Milestone 0: ELM327 connectivity + PID probe spike — **HARD GATE**

**Files**: throwaway spike (a debug `Activity`/instrumented test or scratch branch), not merged as-is.

**Requirements**:
- Pair a specific ELM327 adapter (BT Classic SPP) to the phone; open an RFCOMM socket.
- Run the ELM327 init handshake: `ATZ` (reset), `ATE0` (echo off), `ATL0`, `ATSP0` (auto protocol).
- Request RPM `010C` and speed `010D`; parse responses; confirm sane values on the **target Wigo**.
- Measure achievable poll rate (Hz) and per-request latency; record adapter model + protocol.

**Acceptance**:
- `010C` returns plausible RPM (idle ~850, blips track throttle) repeatably for ≥ 60 s.
- Poll rate and latency recorded. Go/no-go: if RPM cannot be read reliably, STOP — re-evaluate adapter.

**Tests**: `type:device/manual || normal: idle RPM ~850 ± reads steadily || edge: engine off → NO DATA handled || error: adapter absent/unpaired → clean failure, no crash`

### Milestone 1: Config flag + permission scaffold (no connection)

**Files**: `assets/vehicle_config.json`, `VehicleConfig.kt`, `ShiftAssistantService.kt`, `AndroidManifest.xml`, `strings.xml`.

**Requirements**:
- Add `useObdOracle` (default **false**) to `vehicle_config.json`; load in `VehicleConfig.kt`; plumb to the service. No native plumbing (this is Kotlin-only).
- Declare `BLUETOOTH_CONNECT` (+ `BLUETOOTH_SCAN` if discovery needed); runtime-request gating in the calibration UI entry point only — never on the normal start path.
- Diagnostic/state stub (e.g. an OBD status enum: disabled / no-permission / no-adapter / connected) surfaced for UI/logs.

**Acceptance**: flag off → no BT code path reachable, behaviour identical to current build; `assembleDebug` passes.

**Tests**: `type:unit || normal: flag false leaves service start path untouched || edge: flag true but permission denied → status no-permission, mic+accel still run`

### Milestone 2: ELM327 transport + PID parser (Kotlin, JVM-testable)

**Files**: new `obd/` package — `ObdConnection.kt` (RFCOMM socket + AT init), `ElmFramer.kt` (request/response framing), `PidParser.kt` (pure functions).

**Requirements**:
- RFCOMM connect/teardown off the main thread (coroutine/dispatcher); never on audio/sensor threads.
- ELM327 init sequence + per-request transaction with timeout + retry; tolerate echo, `SEARCHING...`,
  `NO DATA`, whitespace, multi-line, prompt `>` framing, and clone quirks.
- `PidParser`: `010C`→RPM `(256·A + B)/4`; `010D`→speed (km/h, byte A). Pure, no Android imports.

**Acceptance**: `PidParser` + framer pass JVM unit tests on canned ELM327 strings; transaction layer
handles timeouts without blocking; no realtime-path imports.

**Tests**: `type:unit || normal: "41 0C 1A F8" → 1726 RPM; "41 0D 50" → 80 km/h || edge: "NO DATA"/"SEARCHING..." → null, no throw || error: malformed/short frame → null`

### Milestone 3: Calibration seeding from OBD ground truth

**Files**: `obd/ObdCalibrator.kt`, `ShiftAssistantService.kt`, reuse existing calibration-state JNI (`resumeCalibrationState` / pin path).

**Requirements**:
- During an opt-in OBD calibration session: read RPM (+ speed), compute `f = RPM·firingFactor/60`,
  pair with GPS `v` (reuse the existing steady-state quality gate), form per-gear `r = f/v`.
- Cluster/aggregate per gear → write as **pinned centroids** via the existing 13-float state
  (`ratio_g` + `pin_g=1`), then `resumeCalibrationState`. No new realtime hook, no per-frame OBD.
- Pure mapping (RPM,v)→r is JVM-unit-testable.

**Acceptance**: a synthetic OBD+GPS drive yields per-gear pinned ratios matching theoretical `k_g`
within tolerance; with the flag off, no seeding occurs. Realtime path untouched.

**Tests**: `type:unit || normal: known (RPM,v) pairs → correct pinned r per gear || edge: <MIN_SPEED or unstable GPS → sample rejected || error: missing gear data → that gear left unpinned`

### Milestone 4: Offline validation harness

**Files**: `obd/ObdValidationLogger.kt` (debug-only by default, DL-OBD-4).

**Requirements**:
- During a drive with the adapter connected, log timestamped rows: OBD `f_true`, mic `dominantHz`,
  vibration `f_vib`/prominence, fused source/mode (from `nativeVibrationFusionStats`), GPS `v`, gear.
- Write CSV to app-private storage for offline analysis; quantify mic/vib/fusion error vs `f_true`.
- Use the output to tune `FUSION_AGREE_TOL`, the M5 ACF thresholds, prominence gates against real data.

**Acceptance**: CSV produced on a logged drive; columns sufficient to compute per-source error; off by
default in release builds.

**Tests**: `type:manual/device || normal: drive log has aligned f_true vs f_est rows || edge: OBD dropout → row marked, logging continues`

### Milestone 5: Docs + tenet reconciliation + ship gating

**Files**: `README.md`, `CLAUDE.md`, `adr.md`.

**Requirements**:
- Update the "no OBD-II" tenet to **"no OBD-II *required*"** (README + CLAUDE.md), describing the
  optional oracle and that the default build is unchanged.
- Promote ADR 005 from *Proposed* to *Accepted* (or record the decision), note BLE-variant follow-up.
- Decide release-vs-debug for the validation logger; document the calibration-session UX entry point.

**Acceptance**: docs coherent (no "no OBD-II" contradiction); ADR status updated.

**Tests**: `type:docs || normal: README/CLAUDE/adr agree on optional-OBD scope`

---

## Sequencing & ownership

- All Claude (codex suspended). Each milestone: branch off `main`, host/JVM tests, PR, CodeRabbit,
  append `session-notes.md`. M0 is a throwaway spike, not a merged PR.
- **Blocked on hardware:** M0 (adapter + car), M4 (on-car drive), M3/M5 on-car confirmation. M1, M2,
  and the pure mapping in M3 are device-free and can proceed now if desired — but per C-OBD-1 they
  should not be *trusted/merged as functional* until M0 confirms the adapter actually talks to the car.
- Lower priority than finishing ADR 004 M6 (on-device validation of the shipping pipeline). The OBD
  oracle's main payoff (M4) is itself the ground-truth source for tuning ADR 004 — so M6 + this plan's
  M0 are naturally done in the same on-car session.
