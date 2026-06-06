Project Task: Architect a native Android manual transmission shift assistant.

You are building a high-performance, local-first Android app. The core logic must be written in C++ via the NDK to guarantee sub-millisecond execution, completely bypassing Kotlin garbage collection for the DSP pipeline.

Create the directory structure, build configurations, and core files based on the following strict requirements:

1. Build & Project Configuration:
   - Generate the `CMakeLists.txt` configured to build a native library (`native-lib.cpp`).
   - Ensure it links against `liblog`, `libandroid` (for ASensorManager), and Google Oboe (assume Oboe is available via prefab or standard inclusion).

2. Native C++ Core (`app/src/main/cpp/native-lib.cpp` & headers):
   - Implement an Oboe audio callback to ingest raw PCM data and procedurally synthesize 50ms staccato audio blips (sine waves at 2000 Hz) for gear shift feedback.
   - Implement a native thread polling `ASENSOR_TYPE_LINEAR_ACCELERATION` via `ASensorManager`.
   - Create a `CalibrationEngine` class. Implement Welford's Online Algorithm to track real-time variance and mean of the ratio (frequency / speed). Use a struct containing `int n`, `float mean`, and `float m2`. This allows non-contiguous session stitching.
   - Implement a lightweight 1D K-Means or RANSAC function to isolate 5 distinct gear ratios (k1 through k5) from the calculated frequency/speed values.
   - Expose JNI bindings: `updateGpsSpeed(float speed)`, `getVUMeterState()`, and `resumeCalibrationState(float[] stateArray)`.

3. Kotlin Architecture (`app/src/main/java/com/app/shiftassistant/`):
   - Create `ShiftAssistantService.kt`: An Android Foreground Service that maintains the GPS connection via `FusedLocationProviderClient` and pushes speed updates to the native JNI method at 1Hz.
   - Create `VUMeterView.kt`: A custom `android.view.View` using a hardware-accelerated Canvas. It should poll the native C++ state at 60 FPS and draw a minimalist, analog-style VU needle indicating the engine's RPM equivalent position across three zones (lugging, optimal, redline).

Please execute this by outputting the scaffolding, the CMake setup, the C++ DSP/Calibration engine, and the Kotlin service and UI classes. Prioritize the math and the JNI bridge structure.
