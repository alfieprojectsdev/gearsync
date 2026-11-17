# Mountain mode plan: codifying a co-driver for mountain roads

Status: proposal, nothing built. Would become ADR 009 if accepted.

## Why refocus

The current product infers RPM from engine sound, then tells the driver when to shift. On a flat road that answer is low value (most drivers shift fine by feel) and the input is the weakest link in the system: cabin noise, 1 Hz GPS smearing `r = f/v`, and a validation gate (C-008) that still needs the car.

Mountain driving is the opposite case. The decisions are harder and the cost of a wrong one is higher: a stall at the apex of an uphill hairpin, the wrong gear entering a corner, brakes overheating on a long descent. Most of those decisions can be made from speed, grade, curvature and a vehicle model, without hearing the engine at all. The mic becomes optional confirmation instead of the foundation.

The reference behaviour is a passenger who learned on Baguio roads telling a less experienced driver when to build speed, ease off, floor it, carry momentum through curves and pick the gear for the climb ahead. The goal is to turn that into rules the phone can apply, then check the rules against her.

## Racing concepts, mapped

Drifting and road driving optimise different things. A drift run maximises controlled slip angle and style inside a closed course. A mountain road drive should minimise risk and workload while keeping enough momentum that a 1.0 L car never bogs down mid-climb. So the mapping keeps the car-control mechanics and throws away the objective.

| What she said | Racing or drifting concept | What the phone measures | Cue |
|---|---|---|---|
| "Speed up now" before a climb | Run-up: exit speed onto a straight decides the whole straight | Grade ahead from the route profile, current speed vs reference | BUILD |
| "Ease up" mid-corner or near a crest | Lift-off weight transfer: lifting loads the front tyres. In a FWD car it tightens the line instead of understeering | Lateral accel near budget, curvature still increasing, crest or blind corner ahead | EASE |
| "Floor it" | Corner exit / track-out: full throttle only once the wheel is unwinding | Yaw rate falling, curvature ahead decreasing, grade positive, next corner far enough away | FLOOR |
| "Use the curve's momentum" | Slow in, fast out; maintenance throttle through the corner; linking corners as one unit | Speed loss across the corner vs reference, throttle phase from net longitudinal accel | HOLD |
| Gear by how steep it gets | Drifters choose the corner's gear before initiation so the engine stays in its band through the whole corner and never shift mid-corner | Speed, grade, power-vs-demand model per gear | Target gear, shown before the corner |
| (descents) Low gear, light brakes | Brake management over a stint | Retarding power needed vs engine braking in current gear | Downshift, brake-heat warning |
| (implied) Brake before, not in, the corner | Straight-line braking to a fixed brake point | Brake point per corner from the reference run | BRAKE |

Rally is a closer model than drifting for the anticipation part. A rally co-driver reads pace notes ("left 3 tightens, crest, don't cut") a few seconds before each feature, which is what she was doing. The plan below generates pace notes from route data and from her recorded run.

### What does not transfer

These stay out, and the cue policy has to make them impossible to suggest:

- Deliberate slip of any kind. Clutch kick, handbrake, feint entries and throttle oversteer have no use on a public road, and the Wigo is FWD anyway, so throttle mid-corner produces understeer, not rotation.
- The racing line across the full road width. On a two-way mountain road the line is constrained to your lane. A late apex inside your lane is the useful part: you see further around a blind corner before committing, and the exit doesn't run wide into the oncoming lane.
- Driving at the grip limit. Racers run at the edge of the friction circle. Mountain mode sets a lateral-accel budget well inside it (start at 0.3 g, tune from her data) and never combines a BRAKE cue with high lateral load.
- Cue speeds above what she drove. The reference run is a ceiling, not a target to beat.

For the "can I stop in the distance I can see" bound, police advanced-driving training (UK Roadcraft) teaches limit-point analysis: read the point where the road disappears and match speed to it. Curvature plus a sight-distance estimate from the route geometry gives a rough version of that.

## Inputs

### GPS (already wired at 1 Hz)

Only speed reaches native code today (`updateGpsSpeed(float)`). Mountain mode needs the full `Location`: position, Doppler speed, bearing, altitude, and the horizontal and vertical accuracy fields.

| Quantity | How | Quality at 1 Hz |
|---|---|---|
| Velocity | `Location.getSpeed()` (Doppler-derived) | Good; lag is the ADR 007 problem, tolerable at mountain speeds |
| Displacement along route | ∫v dt between fixes, snapped to route at each fix | Good for 50–200 m lookahead |
| Longitudinal accel | Differentiated Doppler speed, low-passed | Noisy, about 1 s late. Fine for classifying throttle phase, not for transients |
| Ascent/descent | Δaltitude / Δs over a 50–100 m window | Poor alone. GPS vertical error is usually worse than horizontal, so it's one input to the grade filter, not the answer |
| Heading | `getBearing()` | Needed to tell adjacent switchback legs apart during map-matching |

### Road geometry projected onto a DEM (offline, once per route)

An offline route builder in `scripts/` takes OSM road geometry for a corridor and samples elevation from a DEM, producing a route file the app loads the same way it loads `vehicle_config.json`. New route = new file, no recompile, no runtime network. Local-first holds.

Candidate DEMs: Copernicus GLO-30 and SRTM 1-arcsecond (both about 30 m, free). NAMRIA's IfSAR-derived 5 m DEM would be much better for the Cordillera if it can be obtained; check its licence and availability.

Per route point the file stores distance `s`, lat/lon, elevation `z`, grade, curvature `κ`, and a corner list (start, apex, end, direction, severity on a 1–6 pace-note scale from minimum radius).

Known problems to design for:

- A 30 m cell on a hillside road blends the road cut with the slope above and below it, and two switchback legs 10 m apart can share a cell. Raw DEM grade will be wrong in exactly the places that matter. Smooth the profile, clamp grade to physically plausible values, and replace it with driven data (below) wherever a recorded run exists.
- OSM node spacing on mountain roads is uneven. Resample to fixed `Δs` with a spline before computing curvature, then compare against gyro-measured curvature from a drive.
- OSM geometry can be offset from the real road by several metres. Map-matching needs a corridor tolerance, and the corner list should be re-anchored from recorded yaw data.

### Phone sensors (partly wired)

- Raw accelerometer: already on the native sensor thread at about 399 Hz (ADR 004). Once the mount orientation is known, the forward axis gives `a_meas = dv/dt + g·sin θ`, so grade `θ` falls out from `a_meas` minus GPS-derived `dv/dt`. This is the "full orientation fusion" ADR 008 said dead-reckoning was missing; here it estimates grade, not velocity, which is far less sensitive to drift.
- Gyroscope: not used today. Yaw rate gives curvature directly (`κ = ω_z / v`) and tells entry from exit (yaw rate rising vs falling).
- Barometer (`TYPE_PRESSURE`): not used today. Relative altitude over tens of seconds is much steadier than GPS altitude. Not every phone has one; check the A07 and A56 in M0. Fan and window changes shift cabin pressure, so treat it as a slow input.
- Mic: optional. If present and confident, it confirms the current gear through the existing `classifyGear` path. Mountain mode must work with it off.

Grade is the fused output: DEM prior from the route file, corrected by barometer, accelerometer pitch and GPS altitude through a complementary filter, with the DEM weight dropping wherever recorded runs exist.

## The reference run

Her drive is the ground truth, the same role ADR 005 gives an OBD adapter for RPM. Two recording modes:

1. She drives, the phone records. This yields a speed, gear and throttle-phase profile against `s`: where she brakes, which gear she holds through each hairpin, where she's at full throttle. Racing data systems call this a reference lap and overlay later laps on it.
2. You drive, she rides along and taps a passenger panel when she would have said something: BUILD, EASE, FLOOR, BRAKE, gear up, gear down. This yields labelled moments with the full sensor state attached, which is what the rules get fitted and scored against.

Mode 2 is the more valuable dataset because it captures her judgment about your driving, including the times she would have spoken and the times she let it go.

Throttle phase without a throttle sensor: net propulsive accel = `dv/dt + g·sin θ` + rolling and drag terms. The forward accelerometer axis already reads `dv/dt + g·sin θ`, so in practice it's the measured longitudinal accel plus rolling and drag. Clearly positive is BUILD/FLOOR, near zero is HOLD, negative but small is EASE or engine braking, strongly negative is BRAKE.

## Vehicle model and gear advisor

Road speed to engine RPM from `vehicle_config.json` (`ratios`, `finalDriveRatio`, `nominalCircumferenceMeters` = 1.816 m):

| Speed | 1st | 2nd | 3rd | 4th |
|---|---|---|---|---|
| 15 km/h | 2104 | 1130 | 732 | 538 |
| 20 km/h | 2806 | 1507 | 976 | 717 |
| 30 km/h | 4209 | 2260 | 1464 | 1076 |
| 40 km/h | 5612 | 3014 | 1952 | 1434 |

Steady-state engine torque needed to hold speed on a grade, assuming 1000 kg loaded, rolling coefficient 0.012, 90 % drivetrain efficiency, no aero (all to be replaced by measured values):

| Grade | 1st | 2nd | 3rd |
|---|---|---|---|
| 8 % | 19 Nm | 35 Nm | 54 Nm |
| 12 % | 27 Nm | 50 Nm | 78 Nm |
| 15 % | 33 Nm | 62 Nm | 95 Nm |

The 1KR-FE's peak torque is roughly 90 Nm (confirm against the spec sheet) and it makes less than that at low RPM. So on a 15 % hairpin taken at 20 km/h, 3rd needs more than the engine's peak at under 1000 RPM, and 2nd needs most of the available torque at about 1500 RPM with nothing left to accelerate. 1st at 2800 RPM has margin. That is the "pick the gear before the hairpin" rule expressed as numbers, and it's the core of the advisor.

Uphill rule: for the corner ahead, predict the minimum speed (from the reference run, or from `v = √(a_budget / κ)` without one), then pick the highest gear whose RPM at that speed is above a floor and whose torque margin at the predicted grade exceeds a threshold. Show that gear before the brake point.

Downhill rule: retarding power needed is `m·v·(g·sin|θ| − dv/dt)` minus rolling and drag, so speed the driver lets build up isn't counted as brake work. Engine braking per gear comes from a coast-down test (M0). Whatever is left is brake work. Integrate it with a simple first-order cooling term into a brake-heat index; when it climbs on a long descent, cue a downshift. This replaces the folk rule "go down in the gear you'd climb in" with the same idea computed from the actual grade.

Additional config fields needed: loaded mass, a torque curve (a handful of RPM/Nm points), an RPM floor and ceiling for advice, and the per-gear engine-braking curve from M0.

## Cue policy

A pure state machine (same pattern as `CueState.kt`) takes fused state plus the route lookahead and emits at most one phase cue and one target gear. Lead time is distance-based: cue at `v·t_lead` ahead of the feature, starting with `t_lead` = 3 s, tuned against her tap timings.

Gates:

- Map-match confidence below threshold (position ambiguous between switchback legs, or off-route) suppresses every cue.
- Speed above the reference run's speed at this `s` suppresses BUILD and FLOOR; EASE and BRAKE stay live, since that's when they're needed.
- FLOOR unless yaw rate is falling, curvature ahead is decreasing, grade is positive, and the next corner is beyond a minimum distance.
- BRAKE while lateral accel exceeds half the budget.
- Any cue within a cooldown of the last one, so the driver isn't flooded in a run of switchbacks.

## Display and audio

Eyes belong on the road more on a mountain road than anywhere else, which argues for audio. ADR 006 kept audio opt-in because the speaker could pollute the mic's engine band. If mountain mode runs with the mic off, that conflict disappears, and spoken pace notes ("right hairpin, first gear") through Android TTS become possible on the same Shared/normal-latency output ADR 006 already uses.

Screen: one large target-gear numeral, one phase word or colour, and the next corner as a pace note with distance. No needle animation competing for attention. Landscape lock from ADR 008 stays.

## Milestones

Each milestone ends in something checkable. Device-free work first, following the M0-gate discipline of ADRs 004 and 007.

- M0 instrumented ride-along. Logger for full GPS `Location`, raw accel, gyro, barometer if present, mic `f` if enabled; passenger tap panel; export via `adb`. Coast-down tests on a flat road in neutral (rolling and drag) and on a known grade in each gear with throttle closed (engine braking). Done when there are logged runs of the target route covering at least two ascents and two descents in each recording mode, and the barometer question is answered for the actual phone.
- M1 mount calibration. Gravity vector at rest gives "down", the first straight-line acceleration gives "forward". Done when a host test on M0 logs recovers grade on a known straight within a stated error.
- M2 route builder. `scripts/` tool: OSM corridor + DEM → route file with `s`, `z`, grade, `κ`, corners. Done when its grade and corner list are compared against the M0 driven profile and the error is reported per corner.
- M3 state estimator. Map-matching with heading, fused grade, curvature from gyro, throttle phase. Runs at about 10 Hz, so it can live in Kotlin and be JVM-tested by replaying M0 logs. It needs its own low-rate Kotlin `SensorManager` listeners for accel, gyro and barometer (the native thread's 399 Hz accel stream stays dedicated to vibration DSP), which keeps the JNI surface unchanged. Done when replayed logs produce stable `s`, grade and phase with no switchback mis-matches.
- M4 vehicle model and gear advisor. Done when, replayed over her runs, the advised gear matches the gear she used through each hairpin, with every disagreement listed.
- M5 cue policy in shadow mode. Cues are computed and logged but not shown. Scored against her taps: per-cue precision and recall, and median lead-time difference. Done when the numbers are good enough that she agrees to try them live.
- M6 display and audio, ADR 009 written. Done when a demo-mode replay of a logged run plays the cues end to end.
- M7 live drive with her in the passenger seat. Measured outcomes: mid-corner shifts, uphill near-stalls (RPM under the floor in 1st or 2nd on grade), brake-heat peaks on descents, and how often she still had to speak.

## Questions to ask her before M0

Her rules are tacit, so ask before recording, and again after watching a replay of her own run:

- On an uphill hairpin, when do you choose the gear: at the brake point, at turn-in, or earlier?
- What makes you say "floor it": something you see, the sound, or the feel of the car?
- What does "use the curve's momentum" mean to you in your feet: not braking, staying on part throttle, or something else?
- When do you go down a gear on a descent, and how do you know the brakes are working too hard?
- Which corners on the route do you treat differently from how they look, and why?

## Open decisions

- Does mountain mode become the primary product, with flat-road shift advice as a secondary mode, or a parallel mode?
- Audio on by default inside mountain mode? It reverses the v2 visual-only default for this mode only.
- First target route. One well-logged route is worth more than a builder that handles all of them.
- DEM source: Copernicus/SRTM now, NAMRIA IfSAR if it can be licensed.
