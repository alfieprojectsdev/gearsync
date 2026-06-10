**ADR 001: Native C++ Processing Core via Android NDK**

* **Context:** High-frequency digital signal processing (DSP) requires sub-millisecond execution to generate accurate auditory blips and visual feedback. Kotlin/Java garbage collection introduces non-deterministic jitter, destroying audio synchronization.
* **Decision:** All sensor polling (vibration/audio), DSP calculations (FFT), state-machine evaluations, and audio synthesis will be isolated in a native C++ core. We will use `ASensorManager` for linear acceleration, Google Oboe for low-latency AAudio input/output, and a lightweight header-only library like `kissfft`.
* **Consequences:** Guarantees deterministic latency and avoids VM overhead. Requires strict manual memory management in C++ and a JNI bridge for low-frequency data (1 Hz GPS updates, 60 FPS UI polling).

**ADR 002: Local-First Edge ML for Gear Calibration**

* **Context:** The app must dynamically learn the unique gear ratios of any vehicle ($f = k_g \cdot v$) without relying on cloud processing, heavy Python environments, or pre-trained models.
* **Decision:** We will implement traditional machine learning algorithms directly in C++. For passive auto-tuning, we will use a 1D K-Means clustering algorithm ($K=5$) to isolate gear ratios. For guided calibration, we will use RANSAC linear regression to reject structural noise (e.g., clutch slip, potholes) and find the true ratio slope.
* **Consequences:** Keeps the APK extremely small and highly performant while maintaining absolute user privacy (local-first architecture).

**ADR 003: Stateful Calibration via Welford's Algorithm**

* **Context:** Users need the ability to calibrate gears across non-contiguous driving sessions without bloating a local database with thousands of raw coordinate data points.
* **Decision:** State persistence will utilize Welford's Online Algorithm. For each gear, the system will only persist three lightweight floats: $n$ (sample count), $M_n$ (running mean), and $S_n$ (squared differences). The variance $\sigma^2 = \frac{S_n}{n}$ will serve as the confidence threshold to lock in a gear ratio.
* **Consequences:** Eliminates the need for SQLite or Room DBs for sensor data. State can be saved trivially in `SharedPreferences` and passed down to the C++ engine upon app resume.

**ADR 004: Accelerometer-FFT Vibration Sensing as an Opt-In Fusion Source**

* **Context:** The microphone FFT path is the primary engine-frequency source, but cabin audio, wind, passengers, and road noise can corrupt the dominant acoustic peak. ADR 004 Milestone 0 measured the target device's raw `ACCELEROMETER` path at ~399 Hz after declaring `HIGH_SAMPLING_RATE_SENSORS`, clearing the >=300 Hz gate needed to avoid aliasing the Wigo firing band.
* **Decision:** Add raw-accelerometer vibration sensing as a second, confidence-weighted source for engine firing frequency. The mic path remains primary and default. Vibration fusion is opt-in via `vehicle_config.json`, rate-gated by the measured accel probe, and disabled gracefully when the device is too slow, the accelerometer is unavailable, or mount coupling produces weak signal quality. DSP must reuse the existing native worker, project-owned radix-2 `fft_inplace`, and SPSC handoff style from ADR 001; no heavy dependencies are added.
* **Consequences:** Fusion can improve robustness in acoustically noisy or low-RPM cases without changing default behavior. The native/Kotlin JNI surface grows in lockstep for diagnostics, and the feature must preserve mic-only behavior until the later accelerometer ring, FFT, harmonic guard, and fusion policy milestones are implemented. phyphox is GPLv3 prior art only: ideas may inform the approach, but no code, tables, or implementation structure are copied.
