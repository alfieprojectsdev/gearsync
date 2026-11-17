# Ride-along logger (mountain mode M0)

The logger records what M1–M5 of [plans/mountain-mode-plan.md](../plans/mountain-mode-plan.md) need: raw GPS, accelerometer, gyroscope, barometer, and the passenger's labels. It doesn't need the shift-assistant engine. If the engine is also running (main screen **Start**), its mic readings get logged too.

Open it from the main screen with **Ride log**. Logging continues with the screen off, but the panel keeps the screen on while it's open.

## Phone setup

- Use a rigid mount near the centre of the dash, within the passenger's reach. The passenger taps the mounted phone. Don't hand it around: once the log starts, the phone has to stay fixed to the car or the grade and curvature data is useless.
- Grant precise location and notifications when asked. GPS must be on.
- Before driving off, sit still with the engine idling for about 10 s after **Start log**. Then, on a level straight, do one gentle acceleration from rest and tap **Mark** as you begin it. M1 uses the still period to find "down" and the acceleration to find "forward".

## Modes

Pick the mode before **Start log** (the mode button is locked while recording). It's saved in the session name and `meta.json`.

| Mode | Who drives | What the passenger taps |
|---|---|---|
| coach drives | the experienced driver | What she's doing: the gear she just selected (1–5), and the phase (Build, Hold, Ease, Floor, Brake) when it changes |
| coach labels | the learner | What she would have said, the moment she'd have said it |
| coast-down | either | Run markers for the tests below |

Label meanings:

| Button | Meaning |
|---|---|
| Build | speed up now, usually before a climb |
| Hold | steady part throttle, carry it through |
| Ease | lift off a little |
| Floor | full throttle (corner exit, climb) |
| Brake | brake now, before the corner |
| 1–5 | gear: "in gear N" (coach drives) or "should be in N" (coach labels) |
| Mark | anything worth finding later; also run boundaries in coast-down |
| Undo | the previous label was a mis-tap; analysis drops it |

Each tap gives a short buzz (Undo gives a longer one). The buzz and the finger shake the phone, so analysis should ignore IMU data within about 0.3 s of each label.

## Coast-down tests

The vehicle model (plan, "Vehicle model and gear advisor") needs rolling and drag losses, and engine braking per gear. Only do these where it's legal and there's no traffic. The passenger runs the panel.

1. Rolling and drag: on a flat straight, get to about 50 km/h, shift to neutral, and coast down to about 20 km/h. Tap **Mark** at the start and at the end. Do it in both directions (to cancel wind and slight grade), twice each way.
2. Engine braking: on a steady descent, tap the gear button (e.g. **3**), then **Mark** when you lift fully off the throttle with the clutch engaged, and **Mark** again before you brake or the speed gets uncomfortable. Repeat for 1st to 4th.

A Mark pair with no gear tap before it is a neutral run.

## Getting the logs off the phone

Each session is a directory under the app's external files directory:

```powershell
adb pull /sdcard/Android/data/dev.alfieprojects.gearsync/files/ridelogs .
```

If that path isn't readable on a given phone, use `run-as` on the debug build. It goes through `cmd` because PowerShell 5.1's `>` re-encodes binary output and corrupts the tar:

```powershell
cmd /c "adb exec-out run-as dev.alfieprojects.gearsync tar -c -C /storage/emulated/0/Android/data/dev.alfieprojects.gearsync/files ridelogs > ridelogs.tar"
```

## File format

One directory per session, named `ride_<UTC start>_<mode>`, e.g. `ride_20261117T083000Z_coach-labels`.

| File | Rows | Columns |
|---|---|---|
| `gps.csv` | 1 Hz raw GNSS fixes (`GPS_PROVIDER`, not the fused provider) | `t_ns, utc_ms, provider, lat, lon, alt_ellipsoid_m, alt_msl_m, speed_mps, bearing_deg, h_acc_m, v_acc_m, speed_acc_mps, bearing_acc_deg` |
| `imu.csv` | ~100 Hz each for accel (`A`, m/s², gravity included) and gyro (`G`, rad/s) | `t_ns, rx_ns, sensor, x, y, z` |
| `baro.csv` | whatever rate the barometer delivers; empty if the phone has none | `t_ns, rx_ns, hpa` |
| `labels.csv` | one per tap | `t_ns, label` |
| `engine.csv` | 10 Hz, only while the shift-assistant engine runs (never demo frames) | `t_ns, dominant_hz, speed_mps, gear, confidence, needle` |
| `meta.json` | written at start, rewritten at stop | mode, start/end times, device, sensor details (`sensors.pressure.present` answers the barometer question), embedded `vehicle_config.json`, row counts, any write error |

All streams share one clock: `t_ns` is `SystemClock.elapsedRealtimeNanos()`. Sensor rows use the event timestamp; `rx_ns` is when the callback ran, so you can check the sensor clock. Numbers use `.` as the decimal point whatever the phone's locale. A missing value (a fix with no altitude, say) is an empty field, not 0.

`alt_ellipsoid_m` is height above the WGS84 ellipsoid, which differs from sea level (and from DEM heights) by the local geoid offset. `alt_msl_m` is only filled on Android 14+ phones that compute it; otherwise convert with a geoid model before comparing against a DEM.

## When M0 is done

- At least two ascents and two descents of the target route in each of the two coach modes.
- The coast-down runs above.
- `meta.json` from the real phone shows whether it has a barometer and what rate the gyro delivered.
