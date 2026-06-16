#pragma once

// ADR 008 — gear-display hysteresis + transient gating (host-testable, pure).
//
// The 1 Hz GPS speed is a hard hardware wall (ADR 007: NMEA also caps at 1 Hz on
// the target phones), so during acceleration `r = f/v` smears as `v` goes stale
// and `classifyGear` can flip between adjacent gears frame-to-frame. This guards
// the *displayed* gear (it does not improve accuracy — it hides twitch):
//
//   1. Hysteresis: a new classification must hold for `stableFrames` consecutive
//      frames before it replaces the committed gear. Single-frame neighbour flips
//      never reach the screen.
//   2. Transient gate: when gravity-removed accelerometer magnitude is high (hard
//      accel/brake — exactly when v is stalest), freeze the committed gear and
//      restart the candidate count, so we never latch a transient misclassification.
//
// Rejected alternative (ADR 008): integrating accel into velocity (dead-reckoning).
// On a tilted dash mount gravity (9.8 m/s^2) projects onto the forward axis and
// swamps real ~1-2 m/s^2 motion; without full orientation fusion the integrated v
// is dominated by tilt, worse than holding last-GPS. OBD-II (ADR 005) is the real
// accuracy lever.

struct GearHysteresis {
    int committed = -1;   // gear shown to the UI (0-based, -1 unknown)
    int candidate = -1;   // classification currently accumulating frames
    int count = 0;

    // classified : latest classifyGear() result (-1 = unknown)
    // transient  : true when linear-accel magnitude is high (hold, don't latch)
    // stableFrames: consecutive consistent frames required to commit (>=1)
    int update(int classified, bool transient, int stableFrames) {
        if (transient) {
            // Hold the last committed gear; drop any in-progress candidate so a
            // mid-transient misclassification can't accumulate toward a commit.
            candidate = committed;
            count = 0;
            return committed;
        }
        if (classified == candidate) {
            if (count < stableFrames) ++count;
        } else {
            candidate = classified;
            count = 1;
        }
        if (count >= stableFrames) committed = candidate;
        return committed;
    }

    void reset() {
        committed = -1;
        candidate = -1;
        count = 0;
    }
};
