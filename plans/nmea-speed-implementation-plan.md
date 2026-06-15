# ADR 007 — Low-Latency GPS Velocity via Raw NMEA — Implementation Plan

Implements ADR 007 (`adr.md`). Optional, **default-off** raw-NMEA Doppler speed path to cut the
1 Hz / Kalman-fused velocity latency that smears `r = f/v` during acceleration. The
`FusedLocationProviderClient` 1 Hz path stays the default and fallback.

> **Hard rule:** with `useNmeaSpeed` off, the app is byte-for-byte the current build — no NMEA
> listener registered, fused 1 Hz speed unchanged. NMEA never touches the audio/sensor hot paths.

## Key decisions

| ID | Decision | Rationale |
|----|----------|-----------|
| C-NMEA-1 | **Milestone 0 is an on-device NMEA-rate probe, and all later work is gated on it.** | The entire premise is "NMEA fires faster than 1 Hz." That is **chipset-dependent**; many phones emit NMEA at 1 Hz. Measure `$GxRMC` delivery rate + jitter on the *target* phone first; if 1 Hz, stop — no benefit. Mirrors ADR 004 M0. |
| C-NMEA-2 | Default-off behind `vehicle_config.json` `useNmeaSpeed=false`; fused path is the fallback. | Don't regress devices where NMEA is 1 Hz, unavailable, or worse. |
| C-NMEA-3 | **Parse field 7 in Kotlin; cross JNI as `injectGpsSpeed(float mps, long elapsedNs)`.** | Marshalling a raw `String` into C++ (`GetStringUTFChars`) is *more* JNI overhead, not less. Splitting one CSV line in Kotlin is trivial. |
| C-NMEA-4 | **Anchor with `SystemClock.elapsedRealtimeNanos()` captured in the callback**, not the `onNmeaMessage` timestamp arg. | The callback's `timestamp` is epoch-ms fix time, not monotonic. Alignment needs one monotonic clock shared with the engine-`f` ring. True hardware-instant recovery is approximate — document the residual. |
| C-NMEA-5 | New native method must keep the **JNI export-count invariant** (currently 12↔12) in lockstep + update CLAUDE.md. | Mismatch = UnsatisfiedLinkError. |

## Constraints

- C-NMEA-1 device gate satisfied before any consumer/alignment code is trusted/merged.
- The `$GxRMC` field-7 parser is **pure and host-testable** (no Android): feed canned sentences,
  assert parsed m/s + validity. (See the spike, below.)
- With the flag off: no listener, no behaviour change. Fused path always available as fallback.

---

### Milestone 0: On-device NMEA-rate probe — **HARD GATE**

**Files**: throwaway spike (debug activity / instrumented), not merged as-is.

**Requirements**:
- Register `OnNmeaMessageListener` on the platform `LocationManager`; count `$GxRMC` sentences/sec and
  inter-arrival jitter over ~60 s of driving on the target Wigo phone. Log chipset/model.

**Acceptance**:
- Measured `$GxRMC` rate recorded. **Go only if usefully > 1 Hz** (e.g. ≥ 2 Hz). If 1 Hz, STOP — the
  premise is false on this device; keep the fused path.

**Tests**: `type:device/manual || normal: rate + jitter logged while driving || edge: no NMEA permission/stream → reported unsupported || error: parser sees only $GxGGA (no speed) → RMC-absent flagged`

### Milestone 1: Pure `$GxRMC` field-7 parser (host-testable)

**Files**: `app/src/main/cpp/NmeaSpeed.h` (or Kotlin equiv) + `app/src/main/cpp/test/spike_nmea_parser_host.cpp` (already drafted as the spike).

**Requirements**:
- Parse `$GPRMC`/`$GNRMC`: isolate field 2 (status — require `'A'`) and field 7 (speed, knots) →
  m/s (×0.514444). Reject malformed/short/`'V'`-status sentences → invalid. Tolerant of `$GN`/`$GP`
  talkers and an optional checksum.

**Acceptance**: host unit tests pass on canned valid/invalid sentences; pure, no Android imports.

**Tests**: `type:unit || normal: "$GPRMC,...,A,...,022.4,..." → 11.52 m/s || edge: status 'V' → invalid || error: truncated/empty field 7 → invalid`

### Milestone 2: Kotlin NMEA listener + JNI sink

**Files**: `NmeaVelocityProvider.kt`, `ShiftAssistantService.kt`, `NativeEngine.kt`, `native-lib.cpp`.

**Requirements**:
- Register/unregister `OnNmeaMessageListener` (gated on `useNmeaSpeed` + M0 rate). In the callback:
  filter `$GxRMC`, parse (M1), capture `SystemClock.elapsedRealtimeNanos()`, call new
  `injectGpsSpeed(mps, elapsedNs)`. Keep JNI parity (update count + CLAUDE.md).
- Native `injectGpsSpeed` stores the latest Doppler `v` + its anchor; coexists with `updateGpsSpeed`
  (fused fallback).

**Acceptance**: flag on + adequate rate → native receives multi-Hz `v`; flag off → no listener. `assembleDebug` passes; JNI parity holds.

**Tests**: `type:unit/manual || normal: RMC stream → native v updates at measured rate || edge: flag off → no registration || error: parse fail → sample dropped, fused path intact`

### Milestone 3: Native time-aligned (f, v) pairing

**Files**: `native-lib.cpp` (DSP worker).

**Requirements**:
- Timestamp each engine-`f` estimate with `elapsedRealtimeNanos` into a small fixed `(f, tNs)` ring.
- On an `injectGpsSpeed` update, look back for the `f` nearest the velocity's anchor (within a tolerance
  window) and feed that historically-aligned `(f, v)` to Welford/K-Means instead of the latest `f`.
- With NMEA off, behaviour is the current latest-`f` pairing (unchanged).

**Acceptance**: synthetic timestamped streams pair the correct historical `f`; alignment error within
tolerance; no hot-path allocation. Quality-gate relaxation (learn during accel) evaluated on-device.

**Tests**: `type:unit || normal: jittered f-ring + delayed v → correct historical f paired || edge: v older than ring → oldest f or skip || error: empty ring → skip, no crash`

### Milestone 4: Validation + docs

**Files**: `README.md`, `CLAUDE.md`, `adr.md`, `docs/TEST-DRIVE.md`.

**Requirements**: drive with NMEA on vs off; measure false-transition reduction during 2nd-gear pulls
(ADR 005 OBD true-RPM as ground truth if available). Document the flag, the rate gate, the clock-domain
caveat; promote ADR 007 from Proposed.

**Acceptance**: documented benefit (or documented "1 Hz on this device → not worth it"); docs coherent.

**Tests**: `type:manual/device || normal: fewer false transitions with NMEA on || edge: identical if device NMEA is 1 Hz`

---

## Sequencing & ownership

- All Claude. Each milestone: branch off `main`, host/JVM tests, PR, CodeRabbit, append `session-notes.md`. M0 is a throwaway spike.
- **Device-free now:** the M1 parser + its host spike, and the M3 alignment logic (host-testable with synthetic timestamps). **Device-gated:** M0 rate probe and trusting M2/M4 (need real NMEA on the Wigo phone).
- Lower priority than ADR 004 M6 (the drive). Naturally validated in the same on-car session: the M0 probe rides along with the M6 drive, and OBD (ADR 005) quantifies the payoff.
