# GearSync — Wigo Test-Drive Guide (ADR 004 M6)

On-device validation of the full acoustic + vibration fusion pipeline (ADR 004 M1–M5) on the
target vehicle (Toyota Wigo 1.0 E M/T). This is the one milestone that cannot be done off-device.

## Build / install

A ready debug APK with **vibration fusion enabled** is at repo root:
`gearsync-testdrive-fusion.apk`.

```bash
adb install -r gearsync-testdrive-fusion.apk
```

> The shipped `vehicle_config.json` keeps `useVibrationFusion: false` (mic-primary default). The
> test-drive APK is built with it **true** so the drive exercises the accel ring, vibration FFT,
> fusion policy, and harmonic guard. Flip the default to `true` in `vehicle_config.json` only after
> this drive confirms fusion helps (that decision is the point of M6).

## Phone prep

- Mount the phone on a **rigid dash/windshield mount** — vibration sensing needs solid chassis
  coupling; a loose mount rattles and the fusion policy will (correctly) drop the vibration weight.
- Grant permissions when prompted: **Microphone**, **Location (precise)**, **Notifications**.
- Screen on, app foreground (the VU meter is the UI; foreground-location is sufficient).

## Drive procedure

1. Engine on, app open, tap **Start**. Confirm the foreground notification appears.
2. Calibration is **seeded from theory** (Wigo ratios in `vehicle_config.json`) — it works from the
   first drive; no manual calibration required. (Optional: the **Calibrate** button runs guided
   per-gear capture if you want tighter centroids.)
3. Drive normally through all 5 gears. Watch the horizontal VU meter:
   - **Blue (0–33%)** = lugging → downshift. **Green (33–66%)** = optimal hold.
   - **Red (66–100%)** = approaching redline → upshift.
4. Robustness checks (the reason vibration fusion exists): repeat a pull in 2nd/3rd with
   **window open at speed** and with the **radio on**. Mic-only would lose the peak; fused should
   hold the needle steadier.

## What to capture (makes M6 a real validation, not a blind drive)

Debug builds log a diagnostic line ~every 2 s. Filter logcat:

```bash
adb logcat -s ShiftAssistant
```

Two lines matter:

- **Accel probe** (startup): `Accel probe: <Hz> Hz (gate 300 → supported/REJECTED ...)`. Confirm
  the measured rate is **≥ 300 Hz** (Wigo phone bench-read ~400 Hz). If REJECTED, the device caps
  sensor rate and fusion stays mic-only — note the model.
- **Drive diagnostics** (periodic):
  `drive accelHz=.. fusion=.. active=.. reason=.. vibHz=.. prom=.. src=.. | micHz=.. speed=.. gear=.. needle=..`
  - `fusion=1` config on, `accelHz≥300`, `reason=0` → gate open.
  - `active=1` → vibration actually influenced the selected frequency this frame.
  - `src`: 0 mic-only, 1 fused, 2 rejected-low-rate, 3 rejected-low-prominence, 4 rejected-disagreement.
  - Compare `vibHz` vs `micHz` across the rev range — they should track (both are firing frequency).
  - **Harmonic guard:** if `vibHz` ≈ `micHz` rather than 2×/3× it at high RPM, the guard is working.

## Pass criteria

- Probe ≥ 300 Hz on the Wigo phone (fusion gate opens).
- `vibHz` tracks `micHz` within ~10% through the rev range; no 2×/3× ghost latching at high RPM.
- With window/radio noise, fused needle is visibly steadier than the mic-only behaviour.
- No crashes / audio glitches over a full drive.

## After the drive

- If fusion helped: set `useVibrationFusion: true` in `vehicle_config.json` (ships the feature on).
- Capture the logcat for offline threshold tuning (`FUSION_AGREE_TOL`, ACF guard constants). The
  ADR 005 OBD-II oracle (`plans/obd-oracle-implementation-plan.md`) would provide exact-RPM ground
  truth for that tuning.
