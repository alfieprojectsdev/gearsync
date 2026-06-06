#### System Architecture & Concurrency

1. **Foreground Service (`ShiftAssistantService`):** A Kotlin-level Android Foreground Service ensures the OS does not suspend processing while the screen is locked or the app is minimized.
2. **JNI Boundary:** * *Kotlin -> C++:* `updateGpsSpeed(float speedMps)` (Called at 1 Hz).
* *C++ -> Kotlin:* UI thread polls native state `getEngineState()` at 60 Hz to drive the Canvas animation. Calibration Engine fires `onGearCalibrated(int gear)` via JNI callback.


3. **Thread Pools (C++):**
* *Audio Thread:* Oboe callback running at real-time priority.
* *Sensor Looper:* Dedicated thread polling `ASENSOR_TYPE_LINEAR_ACCELERATION`.
* *DSP Worker:* Pulls from lock-free ring buffers to compute FFTs and ML updates.



#### Signal Processing Pipeline

1. Convert incoming audio PCM and linear acceleration arrays into the frequency domain via FFT.
2. Extract the fundamental frequency peak ($f$) between 20 Hz and 250 Hz.
3. Compute the instantaneous operational ratio $R_{obs} = \frac{f}{v}$.
4. Feed $R_{obs}$ into the Welford State Machine (if in calibration mode) or the k-NN classifier (if in standard driving mode) to determine the current gear.

#### Output Interfaces

1. **Visual (VU Meter Canvas):** A subclassed `android.view.View`. Overrides `onDraw(Canvas)` to paint an analog needle against a 3-zone sweeping arc (Lugging, Optimal, Redline). Hardware-accelerated and entirely devoid of standard Android widget XML overhead.
2. **Audio (Oboe Synth):** Procedurally generates 50ms sine wave bursts between 1,000 Hz and 3,000 Hz. Fires $X$ countable bursts matching the target gear ratio upon crossing the shift threshold.
