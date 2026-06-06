# GearSync

GearSync is a high-performance, local-first native Android application designed to assist manual transmission drivers. By analyzing real-time acoustic signatures and chassis vibrations, GearSync dynamically identifies optimal shifting thresholds and provides low-latency visual and auditory feedback—completely independent of an OBD-II interface or cloud connectivity.

The core digital signal processing (DSP) pipeline and machine learning calibration engines are implemented in native C++ via the Android NDK to bypass virtual machine overhead and ensure sub-millisecond execution.

---

## Features

* **Acoustic & Vibration Tachometer:** Utilizes hardware microphone PCM data and high-frequency linear accelerometer tracking to extract engine firing frequencies without modifying vehicle electronics.
* **Low-Latency Auditory Feedback:** Uses the Google Oboe library to bypass the Android audio mixer, delivering rapid, countable staccato sound cues indicating target gears.
* **Peripheral Visual Interface:** A hardware-accelerated, custom VU meter UI that provides glanceable, zero-distraction structural alignment for optimal shifting.
* **Edge ML Automated Calibration:** Integrates robust statistical algorithms (RANSAC and 1D K-Means Clustering) to seamlessly adapt to any vehicle's gear ratios and mechanical profile.
* **Fragmented Session Stitching:** Built-in state persistence utilizing Welford's Online Algorithm to allow drivers to complete calibration across multiple, non-contiguous driving sessions.

---

## Architecture Overview

GearSync leverages a clear separation of concerns between high-level Android operating system interactions and low-level mathematical processing threads:

```
[Android System]
       │ (1 Hz GPS Speed Updates)
       ▼
[JNI Bridge Surface]
       │
       ▼
[Native C++ Core Core] ◄──── [Oboe Input Stream] (Raw Audio PCM)
       │               ◄──── [ASensorManager] (Linear Acceleration)
       ├─► [FFT / Spectral Analysis Layer]
       ├─► [Edge ML Calibration / State Machine]
       └─► [Oboe Output Stream] (Procedural Synthesizer Blips)

```

### Module Responsibilities

* **Kotlin Layer (`/app/src/main/java`):** Handles lifecycle management, maps the UI Canvas pipeline, hosts the structural Foreground Service to prevent background execution suspension, and feeds GPS metrics downstream.
* **Native NDK Layer (`/app/src/main/cpp`):** Manages lock-free ring buffers, implements real-time Fourier transforms, calculates running statistical variances, and interfaces directly with raw Linux audio paths.

---

## Mathematical Foundations

When the vehicle's clutch is fully engaged, the relationship between the engine's fundamental frequency ($f$) and the actual vehicle speed ($v$) is strictly linear for each individual gear ($g$):

$$f = k_g \cdot v$$

Where $k_g$ represents a constant mechanical slope unique to that gear factor.

### 1. Engine RPM Estimation

For a standard four-stroke internal combustion engine, the operational revolutions per minute (RPM) is evaluated from the dominant spectral peak frequency ($f$) via:

$$\text{RPM} = \frac{120 \cdot f}{P}$$

Where $P$ is the explicit cylinder count of the engine block.

### 2. State Optimization (Welford's Algorithm)

To stitch fragmented calibration runs without logging thousands of raw historical coordinates, the application updates its structural confidence boundaries using Welford's method for calculating running variances:

$$\bar{x}_n = \bar{x}_{n-1} + \frac{x_n - \bar{x}_{n-1}}{n}$$

$$M_{2,n} = M_{2,n-1} + (x_n - \bar{x}_{n-1})(x_n - \bar{x}_n)$$

The tracking variance $\sigma^2 = \frac{M_{2,n}}{n}$ serves as the direct validation cutoff threshold. When variance drops below a set stability value and structural volume is achieved, the gear ratio constant is locked.

---

## Getting Started

### Prerequisites

* Android Studio Hedgehog (2023.3.1) or newer
* Android NDK (Version 26.x or newer)
* CMake 3.22.1+
* A physical Android device running API Level 26 (Android 8.0) or higher (Sensors and mic access are limited within virtual emulators).

### Installation & Build

1. Clone the repository down to your local developer workspace:
```bash
git clone https://github.com/your-repo/GearSync.git
cd GearSync

```


2. Open the project in Android Studio. The IDE will automatically sync the build files and recognize the native configuration paths designated inside `CMakeLists.txt`.
3. Ensure your local configuration points directly to your installed NDK path within your local properties file:
```properties
ndk.dir=/path/to/android-sdk/ndk/26.x.x

```


4. Compile and deploy the debug APK target onto your hardware testing device.

---

## Configuration & Calibration UX

GearSync uses a guided initialization workflow modeled after a standard digital compass calibration sequence:

1. **Setup Engine Constants:** Specify your target vehicle's cylinder layout (e.g., 3, 4, 6) in the settings menu.
2. **Execute Active Calibration:** Select an uncalibrated gear index from the primary dashboard display.
3. **Drive to Synchronize:** Operate the car normally within that designated gear setting. The tracking circle interface will visually fill as clean data streams accumulate. Erratic movements, gear shifts, or clutch engagements will automatically freeze calibration capture to isolate pure data.
4. **Completion:** A dedicated dual-tone audio alert marks target tracking success, permanently saving that specific velocity profile to memory.

---

## License

This project is licensed under the MIT License - see the `LICENSE` file for details.
