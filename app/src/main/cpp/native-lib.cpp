#include <jni.h>
#include <android/log.h>
#include <android/sensor.h>

#include <oboe/Oboe.h>

#include "CalibrationEngine.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#define LOG_TAG "ShiftAssistant"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─── Constants ────────────────────────────────────────────────────────────────

static constexpr int   SAMPLE_RATE        = 48000;
static constexpr int   FFT_SIZE           = 4096;         // ~85 ms window
static constexpr float MIN_ENGINE_HZ      = 20.0f;
static constexpr float MAX_ENGINE_HZ      = 250.0f;
static constexpr float MIN_SPEED_MPS      = 1.0f;         // ignore GPS jitter below 1 m/s
static constexpr float SHIFT_ACCEL_THRESH = 4.0f;         // m/s² spike → visual shift flash
static constexpr int   SENSOR_US          = 10000;        // 100 Hz (fallback if min-delay unknown)

// ─── ADR 004 M0 in-app accel-rate probe ──────────────────────────────────────
// Requests raw ACCELEROMETER at fastest delivery and measures the rate/jitter our
// own ASensorEventQueue path actually achieves (phyphox bench read 400 Hz; this
// confirms our code path lands there too). Gate for fusion is >= ~300 Hz. (DL-001/007/009)
static constexpr float ACCEL_RATE_GATE_HZ  = 300.0f;
static constexpr int   ACCEL_PROBE_WINDOW  = 512;         // intervals per published stats window

enum VibrationDisabledReason : int {
    VIB_REASON_NONE = 0,
    VIB_REASON_CONFIG_DISABLED = 1,
    VIB_REASON_ACCEL_UNSUPPORTED = 2,
    VIB_REASON_LOW_RATE = 3,
    VIB_REASON_DSP_PENDING = 4
};

enum VibrationSourceMode : int {
    VIB_SOURCE_MIC_ONLY = 0,
    VIB_SOURCE_FUSED = 1,
    VIB_SOURCE_REJECTED_LOW_RATE = 2,
    VIB_SOURCE_REJECTED_LOW_PROMINENCE = 3
};

// ─── Shared state (atomic where possible) ────────────────────────────────────

static std::atomic<float>    g_gpsSpeed{0.0f};            // metres per second from GPS
static std::atomic<float>    g_dominantHz{0.0f};          // last FFT peak
static std::atomic<float>    g_needlePos{0.0f};           // 0.0 (lug) … 1.0 (redline)
static std::atomic<int>      g_currentGear{-1};           // 0-based gear index, -1 = unknown
static std::atomic<bool>     g_shiftDetected{false};      // set by sensor thread; consumed by VU meter

// ─── ADR 004 M0 probe stats (written by sensor thread, read via JNI diagnostic) ─
static std::atomic<float>    g_accelEffHz{0.0f};          // measured effective rate, Hz
static std::atomic<float>    g_accelMinIntervalMs{0.0f};  // min inter-arrival in last window
static std::atomic<float>    g_accelMaxIntervalMs{0.0f};  // max inter-arrival in last window
static std::atomic<float>    g_accelJitterMs{0.0f};       // stddev of inter-arrival in last window
static std::atomic<float>    g_accelSampleCount{0.0f};    // cumulative samples seen
static std::atomic<int>      g_accelSupported{-1};        // -1 unknown, 0 unsupported, 1 supported
static std::atomic<float>    g_accelRequestedHz{0.0f};    // requested raw-accel rate from min-delay

// ─── Vehicle config (set once via setVehicleConfig JNI, then read-only) ──────

static std::atomic<float>    g_toleranceLow{0.0f};            // 0 = open (no rejection)
static std::atomic<float>    g_toleranceHigh{0.0f};
static std::atomic<int>      g_stabilityWindowSamples{0};     // 0 = no window required
static std::atomic<float>    g_speedJitterThreshold{0.5f};    // m/s
static std::atomic<bool>     g_useVibrationFusion{false};     // ADR 004 opt-in; mic remains default

// ─── GPS speed stability tracking (updated in updateGpsSpeed at 1 Hz) ────────

static std::atomic<float>    g_prevGpsSpeed{-1.0f};           // -1 sentinel = first update
static std::atomic<int>      g_speedStableCount{0};

// ─── PCM ring buffer ──────────────────────────────────────────────────────────
// Written exclusively by the audio input callback — no mutex required.

static float     g_pcmRing[FFT_SIZE]{};
static uint32_t  g_pcmHead = 0;   // uint32_t: wraps at 2^32, well-defined, ~25 hours at 48 kHz

// ─── DSP worker handoff (lock-free SPSC snapshot slot) ───────────────────────
// The audio callback copies a PCM snapshot into g_dspSnapshot and bumps
// g_dspWriteSeq (release).  The DSP worker copies the snapshot into a local
// buffer and THEN releases the slot (g_dspReadSeq) BEFORE processing,
// so the input callback can safely overwrite g_dspSnapshot only after the
// local copy is complete.

static float                 g_dspSnapshot[FFT_SIZE]{};
static std::atomic<uint32_t> g_dspWriteSeq{0};
static std::atomic<uint32_t> g_dspReadSeq{0};
static std::thread           g_dspThread;
static std::atomic<bool>     g_dspRunning{false};

// ─── Calibration engine ───────────────────────────────────────────────────────

static CalibrationEngine g_calibEngine;

// ─── ADR 004 M1 vibration-fusion diagnostics ────────────────────────────────
// M1 is flag + diagnostic plumbing only. The ring/FFT/fusion path lands in
// M2-M4, so fusionActive remains false and source remains mic-primary.

static std::atomic<bool>     g_vibrationFusionActive{false};
static std::atomic<int>      g_vibrationDisabledReason{VIB_REASON_CONFIG_DISABLED};
static std::atomic<int>      g_vibrationSourceMode{VIB_SOURCE_MIC_ONLY};
static std::atomic<float>    g_vibrationHz{0.0f};
static std::atomic<float>    g_vibrationProminence{0.0f};

static void updateVibrationFusionDiagnostics() {
    const bool requested = g_useVibrationFusion.load(std::memory_order_relaxed);
    const int supported = g_accelSupported.load(std::memory_order_relaxed);
    const float effHz = g_accelEffHz.load(std::memory_order_relaxed);

    g_vibrationFusionActive.store(false, std::memory_order_relaxed);
    g_vibrationHz.store(0.0f, std::memory_order_relaxed);
    g_vibrationProminence.store(0.0f, std::memory_order_relaxed);

    if (!requested) {
        g_vibrationDisabledReason.store(VIB_REASON_CONFIG_DISABLED, std::memory_order_relaxed);
        g_vibrationSourceMode.store(VIB_SOURCE_MIC_ONLY, std::memory_order_relaxed);
    } else if (supported == 0) {
        g_vibrationDisabledReason.store(VIB_REASON_ACCEL_UNSUPPORTED, std::memory_order_relaxed);
        g_vibrationSourceMode.store(VIB_SOURCE_MIC_ONLY, std::memory_order_relaxed);
    } else if (effHz < ACCEL_RATE_GATE_HZ) {
        g_vibrationDisabledReason.store(VIB_REASON_LOW_RATE, std::memory_order_relaxed);
        g_vibrationSourceMode.store(VIB_SOURCE_REJECTED_LOW_RATE, std::memory_order_relaxed);
    } else {
        // Rate gate is open, but M1 intentionally does not implement the
        // accelerometer SPSC ring, vibration FFT, or fusion policy yet.
        g_vibrationDisabledReason.store(VIB_REASON_DSP_PENDING, std::memory_order_relaxed);
        g_vibrationSourceMode.store(VIB_SOURCE_MIC_ONLY, std::memory_order_relaxed);
    }
}

// ─── JVM callback state ──────────────────────────────────────────────────────
//
// The DSP worker thread is not JVM-attached at the time a gear-lock fires; it
// cannot call FindClass or use local refs cached on the JNI startup thread.
// Solution: cache g_jvm in JNI_OnLoad, then cache a NewGlobalRef jclass and a
// static jmethodID in startEngine. The worker attaches once on thread start and
// detaches at exit. (ref: DL-003)

static JavaVM*   g_jvm              = nullptr;
static jclass    g_engClass         = nullptr;  // NewGlobalRef — freed in stopEngine
static jmethodID g_onGearCalibrated = nullptr;  // static method; valid for process lifetime

// Cache the JavaVM pointer at library load time. (ref: DL-003)
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

// ─── Inline radix-2 Cooley–Tukey FFT ─────────────────────────────────────────

static void fft_inplace(std::vector<std::complex<float>>& a) {
    const int n = static_cast<int>(a.size());
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * static_cast<float>(M_PI) / static_cast<float>(len);
        std::complex<float> wlen(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (int j = 0; j < len / 2; ++j) {
                std::complex<float> u = a[i + j];
                std::complex<float> v = a[i + j + len / 2] * w;
                a[i + j]           = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

// ─── DSP: find dominant frequency in engine band ──────────────────────────────

static float findDominantHz(const float* pcm, int len) {
    std::vector<std::complex<float>> buf(len);
    for (int i = 0; i < len; ++i) {
        float w = 0.54f - 0.46f * std::cos(2.0f * static_cast<float>(M_PI) * i / (len - 1));
        buf[i]  = { pcm[i] * w, 0.0f };
    }
    fft_inplace(buf);

    int binMin = static_cast<int>(MIN_ENGINE_HZ * len / SAMPLE_RATE);
    int binMax = static_cast<int>(MAX_ENGINE_HZ * len / SAMPLE_RATE);
    binMax     = std::min(binMax, len / 2 - 1);

    float peakMag = 0.0f;
    int   peakBin = binMin;
    for (int b = binMin; b <= binMax; ++b) {
        float mag = std::abs(buf[b]);
        if (mag > peakMag) { peakMag = mag; peakBin = b; }
    }
    return static_cast<float>(peakBin) * SAMPLE_RATE / static_cast<float>(len);
}

// ─── DSP worker thread ────────────────────────────────────────────────────────
// Runs all heavy computation (FFT, Welford, K-Means, needle mapping) off the
// real-time audio thread to prevent buffer underruns and audio glitches.

static void dspWorkerFn() {
    // Thread-local copy of the snapshot — avoids any overlap with the input callback.
    float localSnapshot[FFT_SIZE];

    // Attach the DSP worker to the JVM for the lifetime of the thread.
    // A per-callback attach/detach would be cheaper in the no-lock case but risks
    // leaking a detach on early exit; one attach + one detach is simpler and safe.
    // (ref: DL-003)
    JNIEnv* dspEnv  = nullptr;
    bool    attached = false;
    if (g_jvm) {
        jint rc  = g_jvm->AttachCurrentThread(&dspEnv, nullptr);
        attached = (rc == JNI_OK);
        if (!attached) LOGE("DSP thread: JVM attach failed (%d)", rc);
    }

    while (g_dspRunning.load(std::memory_order_relaxed)) {
        uint32_t w = g_dspWriteSeq.load(std::memory_order_acquire);
        uint32_t r = g_dspReadSeq.load(std::memory_order_relaxed);

        if (w == r) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Copy the shared snapshot to local storage BEFORE releasing the slot.
        // Releasing readSeq first would allow the input callback to overwrite
        // g_dspSnapshot while we are still copying from it.
        std::memcpy(localSnapshot, g_dspSnapshot, sizeof(localSnapshot));
        g_dspReadSeq.store(w, std::memory_order_relaxed);  // slot is now free for next snapshot

        float hz    = findDominantHz(localSnapshot, FFT_SIZE);
        float speed = g_gpsSpeed.load();
        g_dominantHz.store(hz);

        if (speed >= MIN_SPEED_MPS) {
            float ratio     = hz / speed;
            int   stableReq = g_stabilityWindowSamples.load(std::memory_order_relaxed);
            int   stable    = g_speedStableCount.load(std::memory_order_relaxed);

            // Only feed learning when GPS speed has been stable for the configured window.
            if (stableReq == 0 || stable >= stableReq) {
                // Guided capture and passive learning are mutually exclusive per sample.
                // feedCalibrationSample suppresses passive feeding for the duration of a
                // capture session. (ref: DL-005, C-001)
                bool locked = false;
                int  calibratingGear = -1;
                if (g_calibEngine.isCalibrating()) {
                    // Cache the gear under capture BEFORE the feed: a successful lock
                    // resets m_calibGear to -1, and classifyGear(ratio) would reclassify
                    // only the last sample — which the asymmetric band can map to -1 or a
                    // neighbour even on a good fit, misreporting the lock. (ref: DL-003)
                    calibratingGear = g_calibEngine.calibratingGear();
                    locked = g_calibEngine.feedCalibrationSample(speed, hz);
                } else {
                    g_calibEngine.updateWelford(ratio);
                }

                // Fire the upcall after feedCalibrationSample returns (m_mutex released),
                // reporting the exact gear the user calibrated. The order-break guard in
                // feedCalibrationSample guarantees that gear stays at its index. (ref: DL-003)
                if (locked && attached && g_engClass && g_onGearCalibrated &&
                    calibratingGear >= 0) {
                    dspEnv->CallStaticVoidMethod(g_engClass, g_onGearCalibrated,
                                                 static_cast<jint>(calibratingGear));
                    // A failed callback must not crash the DSP thread.
                    if (dspEnv->ExceptionCheck()) dspEnv->ExceptionClear();
                }
            }

            int gear = g_calibEngine.classifyGear(ratio);
            g_currentGear.store(gear);

            if (gear >= 0) {
                auto  ratios = g_calibEngine.getGearRatios();
                float lo     = (gear < NUM_GEARS - 1) ? ratios[gear + 1] : ratios[gear] * 0.8f;
                float hi     = ratios[gear];
                float pos    = (hi - lo) > 1e-6f ? (ratio - lo) / (hi - lo) : 0.5f;
                g_needlePos.store(std::max(0.0f, std::min(1.0f, pos)));
            } else {
                g_needlePos.store(0.0f);  // reset to lug end when gear is unknown
            }
        }
    }

    // Detach before thread exit — matches the AttachCurrentThread above. (ref: DL-003)
    if (attached) g_jvm->DetachCurrentThread();
}

// ─── Audio input callback (microphone → PCM ring buffer → DSP handoff) ───────
// This callback is wait-free: no mutex, no allocation, no blocking call.

class InputCallback : public oboe::AudioStreamDataCallback {
public:
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* /*stream*/,
                                          void* audioData,
                                          int32_t numFrames) override {
        auto* samples = static_cast<float*>(audioData);

        // Single-producer write into ring buffer — no lock needed.
        for (int i = 0; i < numFrames; ++i) {
            g_pcmRing[g_pcmHead % FFT_SIZE] = samples[i];
            ++g_pcmHead;
        }

        // Hand off a snapshot to the DSP worker only when the previous one has
        // been consumed (write seq == read seq), preventing snapshot trampling.
        if (g_pcmHead >= FFT_SIZE &&
            g_dspWriteSeq.load(std::memory_order_relaxed) ==
            g_dspReadSeq.load(std::memory_order_relaxed)) {
            uint32_t start = g_pcmHead % FFT_SIZE;
            for (int i = 0; i < FFT_SIZE; ++i)
                g_dspSnapshot[i] = g_pcmRing[(start + i) % FFT_SIZE];
            // Release ordering ensures snapshot writes are visible before the seq bump.
            g_dspWriteSeq.fetch_add(1, std::memory_order_release);
        }

        return oboe::DataCallbackResult::Continue;
    }
};

// ─── Stream handle (input only — no audio output stream) ─────────────────────

static std::shared_ptr<oboe::AudioStream> g_inputStream;
static InputCallback                      g_inputCallback;

// Returns true only if the input stream opened and started successfully.
static bool openStreams() {
    oboe::AudioStreamBuilder inBuilder;
    inBuilder.setDirection(oboe::Direction::Input)
             ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
             ->setSharingMode(oboe::SharingMode::Exclusive)
             ->setFormat(oboe::AudioFormat::Float)
             ->setChannelCount(1)
             ->setSampleRate(SAMPLE_RATE)
             ->setDataCallback(&g_inputCallback);

    oboe::Result result = inBuilder.openStream(g_inputStream);
    if (result != oboe::Result::OK || !g_inputStream) {
        LOGE("Failed to open input stream: %s", oboe::convertToText(result));
        return false;
    }
    g_inputStream->requestStart();
    LOGI("Oboe input stream open — sample rate %d Hz", SAMPLE_RATE);
    return true;
}

static void closeStreams() {
    if (g_inputStream) {
        g_inputStream->stop();
        g_inputStream->close();
        g_inputStream.reset();
    }
}

// ─── Sensor thread (raw ACCELEROMETER, fastest rate) ─────────────────────────
// ADR 004 M0: reads raw ACCELEROMETER (not the fused LINEAR_ACCELERATION, which
// is often rate-throttled — DL-007) at the fastest delivery the device allows,
// measures the effective rate/jitter our own queue path achieves, and still
// drives shift-event detection. Gravity is removed for spike detection via a
// slow EMA baseline; the future vibration FFT removes it as a DC term.

static ASensorManager*    g_sensorManager    = nullptr;
static ASensorEventQueue* g_sensorEventQueue = nullptr;
static std::thread        g_sensorThread;
static std::atomic<bool>  g_sensorRunning{false};

// Publish one window of inter-arrival statistics from worker-local accumulators.
static void publishAccelStats(const float* intervalsMs, int count, float cumulativeSamples) {
    if (count <= 0) return;
    float sum = 0.0f, mn = intervalsMs[0], mx = intervalsMs[0];
    for (int i = 0; i < count; ++i) {
        const float v = intervalsMs[i];
        sum += v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    const float mean = sum / static_cast<float>(count);
    float var = 0.0f;
    for (int i = 0; i < count; ++i) {
        const float d = intervalsMs[i] - mean;
        var += d * d;
    }
    var /= static_cast<float>(count);
    const float effHz = (mean > 0.0f) ? 1000.0f / mean : 0.0f;

    g_accelEffHz.store(effHz);
    g_accelMinIntervalMs.store(mn);
    g_accelMaxIntervalMs.store(mx);
    g_accelJitterMs.store(std::sqrt(var));
    g_accelSampleCount.store(cumulativeSamples);
    updateVibrationFusionDiagnostics();
    LOGI("Accel probe: %.1f Hz (gate %.0f → %s), interval mean=%.2f min=%.2f max=%.2f jitter=%.2f ms",
         effHz, ACCEL_RATE_GATE_HZ, effHz >= ACCEL_RATE_GATE_HZ ? "PASS" : "LOW",
         mean, mn, mx, std::sqrt(var));
}

static void sensorThreadFn() {
    ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);

    g_sensorManager = ASensorManager_getInstanceForPackage("dev.alfieprojects.gearsync");
    if (!g_sensorManager) {
        LOGE("ASensorManager unavailable");
        g_accelSupported.store(0);
        updateVibrationFusionDiagnostics();
        g_sensorRunning.store(false);  // reflect actual stopped state
        return;
    }

    const ASensor* accel = ASensorManager_getDefaultSensor(
            g_sensorManager, ASENSOR_TYPE_ACCELEROMETER);
    if (!accel) {
        LOGE("ACCELEROMETER sensor unavailable");
        g_accelSupported.store(0);
        updateVibrationFusionDiagnostics();
        g_sensorRunning.store(false);  // reflect actual stopped state
        return;
    }

    g_sensorEventQueue = ASensorManager_createEventQueue(
            g_sensorManager, looper, ALOOPER_EVENT_INPUT, nullptr, nullptr);
    if (!g_sensorEventQueue) {
        LOGE("Failed to create sensor event queue");
        g_accelSupported.store(0);
        updateVibrationFusionDiagnostics();
        g_sensorRunning.store(false);
        return;
    }

    // Request the fastest delivery the sensor advertises; fall back to SENSOR_US.
    int minDelayUs = ASensor_getMinDelay(accel);
    if (minDelayUs <= 0) minDelayUs = SENSOR_US;
    if (ASensorEventQueue_enableSensor(g_sensorEventQueue, accel) < 0 ||
        ASensorEventQueue_setEventRate(g_sensorEventQueue, accel, minDelayUs) < 0) {
        LOGE("Failed to enable/configure accelerometer event rate");
        ASensorManager_destroyEventQueue(g_sensorManager, g_sensorEventQueue);
        g_sensorEventQueue = nullptr;
        g_accelSupported.store(0);
        updateVibrationFusionDiagnostics();
        g_sensorRunning.store(false);
        return;
    }
    // Only now is the probe genuinely live — publish support after success.
    const float requestedHz = minDelayUs > 0 ? 1000000.0f / static_cast<float>(minDelayUs) : 0.0f;
    g_accelRequestedHz.store(std::isfinite(requestedHz) ? requestedHz : 0.0f);
    g_accelSupported.store(1);
    updateVibrationFusionDiagnostics();
    LOGI("Sensor thread running: raw ACCELEROMETER at %d µs (min-delay)", minDelayUs);

    // Worker-local rate-probe accumulators (no heap alloc in the loop).
    float    intervalsMs[ACCEL_PROBE_WINDOW];
    int      intervalCount   = 0;
    int64_t  prevTs          = 0;
    float    cumulativeCount = 0.0f;
    float    gravityEma      = 9.81f;   // slow baseline ≈ gravity, removed for spike detect

    ASensorEvent events[8];
    while (g_sensorRunning.load()) {
        int n = ASensorEventQueue_getEvents(g_sensorEventQueue, events, 8);
        if (n > 0) {
            for (int i = 0; i < n; ++i) {
                const float ax = events[i].acceleration.x;
                const float ay = events[i].acceleration.y;
                const float az = events[i].acceleration.z;
                const float mag = std::sqrt(ax*ax + ay*ay + az*az);

                // Gravity-removed spike detection (raw accel includes ~9.81 g). (C-007)
                gravityEma += 0.02f * (mag - gravityEma);
                if (std::fabs(mag - gravityEma) > SHIFT_ACCEL_THRESH) {
                    g_shiftDetected.store(true);
                }

                // Rate probe: accumulate inter-arrival intervals from event timestamps.
                const int64_t ts = events[i].timestamp;  // ns, monotonic
                if (prevTs != 0 && ts > prevTs) {
                    if (intervalCount < ACCEL_PROBE_WINDOW) {
                        intervalsMs[intervalCount++] =
                            static_cast<float>(ts - prevTs) / 1.0e6f;
                    }
                }
                prevTs = ts;
                cumulativeCount += 1.0f;

                if (intervalCount >= ACCEL_PROBE_WINDOW) {
                    publishAccelStats(intervalsMs, intervalCount, cumulativeCount);
                    intervalCount = 0;
                }
            }
        }
        ALooper_pollOnce(2, nullptr, nullptr, nullptr);
    }

    // Flush any partial window so a short session still reports a rate.
    publishAccelStats(intervalsMs, intervalCount, cumulativeCount);

    ASensorEventQueue_disableSensor(g_sensorEventQueue, accel);
    ASensorManager_destroyEventQueue(g_sensorManager, g_sensorEventQueue);
    g_sensorEventQueue = nullptr;
    LOGI("Sensor thread stopped");
}

// ─── JNI entry points ─────────────────────────────────────────────────────────

extern "C" {

JNIEXPORT void JNICALL
Java_dev_alfieprojects_gearsync_NativeEngine_startEngine(JNIEnv* env, jclass cls) {
    // Global class ref + static method ID are cached here (not in JNI_OnLoad) because
    // FindClass from JNI_OnLoad uses the system class loader and cannot find app classes.
    // cls is valid on this call (invoked from app code on the main thread). (ref: DL-003)
    if (g_engClass == nullptr) {
        g_engClass = static_cast<jclass>(env->NewGlobalRef(cls));
        g_onGearCalibrated = env->GetStaticMethodID(g_engClass, "onGearCalibrated", "(I)V");
        if (!g_onGearCalibrated) {
            // ProGuard must keep NativeEngine.onGearCalibrated; covered by existing
            // -keep class ...NativeEngine { *; } rule. (ref: C-007)
            LOGE("onGearCalibrated method not found -- callback disabled");
        }
    }

    // Reset ring-buffer head so stale samples from a prior session are not fed
    // into the first DSP snapshot after restart.
    g_pcmHead = 0;
    std::memset(g_pcmRing, 0, sizeof(g_pcmRing));

    // Reset GPS stability counters so the stability window starts fresh.
    g_prevGpsSpeed.store(-1.0f);
    g_speedStableCount.store(0);

    if (!openStreams()) {
        LOGE("Engine startup aborted: audio input stream unavailable");
        return;
    }
    g_dspRunning.store(true);
    g_dspThread = std::thread(dspWorkerFn);

    g_sensorRunning.store(true);
    g_sensorThread = std::thread(sensorThreadFn);
    LOGI("Native engine started");
}

JNIEXPORT void JNICALL
Java_dev_alfieprojects_gearsync_NativeEngine_stopEngine(JNIEnv* env, jclass) {
    g_sensorRunning.store(false);
    if (g_sensorThread.joinable()) g_sensorThread.join();

    g_dspRunning.store(false);
    if (g_dspThread.joinable()) g_dspThread.join();

    closeStreams();

    // Release the global ref created in startEngine. The DSP thread is already
    // joined above, so no callbacks can fire after this point. (ref: DL-003)
    if (g_engClass) {
        env->DeleteGlobalRef(g_engClass);
        g_engClass         = nullptr;
        g_onGearCalibrated = nullptr;
    }
    LOGI("Native engine stopped");
}

// Called from ShiftAssistantService at 1 Hz.
// Also updates the GPS stability counter used to gate Welford updates.
JNIEXPORT void JNICALL
Java_dev_alfieprojects_gearsync_NativeEngine_updateGpsSpeed(JNIEnv*, jclass, jfloat speed) {
    float prev         = g_prevGpsSpeed.load(std::memory_order_relaxed);
    float jitterThresh = g_speedJitterThreshold.load(std::memory_order_relaxed);

    if (prev < 0.0f) {
        // First update after start — initialise without incrementing stable count.
        g_speedStableCount.store(0, std::memory_order_relaxed);
    } else if (std::fabs(speed - prev) <= jitterThresh) {
        g_speedStableCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        // Speed changed beyond jitter threshold — reset stability window.
        g_speedStableCount.store(0, std::memory_order_relaxed);
    }
    g_prevGpsSpeed.store(speed, std::memory_order_relaxed);
    g_gpsSpeed.store(speed);
}

// Called from VUMeterView at 60 FPS.
// Returns float[6]: [needlePos, dominantHz, speedMps, gear+1 (1-based, 0=unknown),
//                    confidence, shiftDetected (1.0 = shift event pending)]
// shiftDetected is cleared (consumed) on each read so the flash is one-shot.
JNIEXPORT jfloatArray JNICALL
Java_dev_alfieprojects_gearsync_NativeEngine_nativeVUMeterState(JNIEnv* env, jclass) {
    jfloatArray result = env->NewFloatArray(6);
    if (!result) return nullptr;

    float needle   = g_needlePos.load();
    float hz       = g_dominantHz.load();
    float speed    = g_gpsSpeed.load();
    int   gear     = g_currentGear.load();  // 0-based, -1 = unknown
    float variance = g_calibEngine.getVariance();
    float conf     = (variance > 0.0f) ? 1.0f / (1.0f + variance) : 0.0f;
    // exchange clears the flag atomically so the VU meter flash fires exactly once per event.
    float shift    = g_shiftDetected.exchange(false) ? 1.0f : 0.0f;

    float buf[6] = {
        needle,
        hz,
        speed,
        static_cast<float>(gear + 1),  // 1-based; 0 = unknown
        conf,
        shift
    };
    env->SetFloatArrayRegion(result, 0, 6, buf);
    return result;
}

// ADR 004 M0 diagnostic. Returns float[6]:
//   [effectiveHz, minIntervalMs, maxIntervalMs, jitterMs, cumulativeSamples,
//    supported (1 yes / 0 no / -1 unknown)]
// effectiveHz >= 300 means our own raw-ACCELEROMETER path clears the fusion gate. (DL-009)
JNIEXPORT jfloatArray JNICALL
Java_dev_alfieprojects_gearsync_NativeEngine_nativeAccelProbeStats(JNIEnv* env, jclass) {
    jfloatArray result = env->NewFloatArray(6);
    if (!result) return nullptr;
    float buf[6] = {
        g_accelEffHz.load(),
        g_accelMinIntervalMs.load(),
        g_accelMaxIntervalMs.load(),
        g_accelJitterMs.load(),
        g_accelSampleCount.load(),
        static_cast<float>(g_accelSupported.load())
    };
    env->SetFloatArrayRegion(result, 0, 6, buf);
    return result;
}

// ADR 004 M1 diagnostic. Returns float[8]:
//   [requestedAccelHz, measuredAccelHz, useVibrationFusion (1/0),
//    fusionActive (1/0), disabledReasonCode, latestVibrationHz,
//    vibrationProminence, sourceModeCode]
JNIEXPORT jfloatArray JNICALL
Java_dev_alfieprojects_gearsync_NativeEngine_nativeVibrationFusionStats(JNIEnv* env, jclass) {
    updateVibrationFusionDiagnostics();
    jfloatArray result = env->NewFloatArray(8);
    if (!result) return nullptr;
    float buf[8] = {
        g_accelRequestedHz.load(),
        g_accelEffHz.load(),
        g_useVibrationFusion.load() ? 1.0f : 0.0f,
        g_vibrationFusionActive.load() ? 1.0f : 0.0f,
        static_cast<float>(g_vibrationDisabledReason.load()),
        g_vibrationHz.load(),
        g_vibrationProminence.load(),
        static_cast<float>(g_vibrationSourceMode.load())
    };
    env->SetFloatArrayRegion(result, 0, 8, buf);
    return result;
}

// Restore Welford + gear-ratio state from a previous session.
// stateArray layout: [n_float, mean, m2, ratio0, ratio1, ratio2, ratio3, ratio4]
JNIEXPORT void JNICALL
Java_dev_alfieprojects_gearsync_NativeEngine_resumeCalibrationState(JNIEnv* env,
                                                                 jclass,
                                                                 jfloatArray stateArray) {
    if (!stateArray) return;
    jsize len = env->GetArrayLength(stateArray);
    if (len < 3 + NUM_GEARS) return;

    jfloat* data = env->GetFloatArrayElements(stateArray, nullptr);
    if (!data) return;
    g_calibEngine.deserialise(data, static_cast<int>(len));
    env->ReleaseFloatArrayElements(stateArray, data, JNI_ABORT);

    LOGI("Calibration state restored — n=%d mean=%.4f",
         g_calibEngine.getSampleCount(), g_calibEngine.getMean());
}

// Serialise current calibration state for SharedPreferences persistence.
JNIEXPORT jfloatArray JNICALL
Java_dev_alfieprojects_gearsync_NativeEngine_saveCalibrationState(JNIEnv* env, jclass) {
    // LEN = 3 Welford + 5 ratios + 5 pin flags = 13 floats. (ref: DL-002)
    constexpr int LEN = 3 + NUM_GEARS + NUM_GEARS;
    jfloatArray result = env->NewFloatArray(LEN);
    if (!result) return nullptr;

    float buf[LEN]{};
    g_calibEngine.serialise(buf, LEN);
    env->SetFloatArrayRegion(result, 0, LEN, buf);
    return result;
}

// Called from ShiftAssistantService after startEngine, with parameters derived
// from vehicle_config.json.  Seeds the calibration engine and configures the
// GPS stability window and tolerance band used by classifyGear.
JNIEXPORT void JNICALL
Java_dev_alfieprojects_gearsync_NativeEngine_setVehicleConfig(JNIEnv*     env,
                                                           jclass,
                                                           jfloatArray kSeedsArr,
                                                           jfloat      toleranceLow,
                                                           jfloat      toleranceHigh,
                                                           jint        stabilityWindowSamples,
                                                           jfloat      speedJitterThresholdMps,
                                                           jboolean    useVibrationFusion) {
    if (!kSeedsArr) return;
    jsize len = env->GetArrayLength(kSeedsArr);
    if (len < NUM_GEARS) return;

    jfloat* seeds = env->GetFloatArrayElements(kSeedsArr, nullptr);
    if (!seeds) return;

    g_calibEngine.seedCentroids(seeds, static_cast<int>(len), toleranceLow, toleranceHigh);
    env->ReleaseFloatArrayElements(kSeedsArr, seeds, JNI_ABORT);

    g_toleranceLow.store(toleranceLow);
    g_toleranceHigh.store(toleranceHigh);
    g_stabilityWindowSamples.store(stabilityWindowSamples);
    g_speedJitterThreshold.store(speedJitterThresholdMps);
    g_useVibrationFusion.store(useVibrationFusion == JNI_TRUE);
    updateVibrationFusionDiagnostics();

    LOGI("Vehicle config applied — tol=[%.3f, %.3f] stableWin=%d jitter=%.2f m/s vibFusion=%s",
         static_cast<float>(toleranceLow), static_cast<float>(toleranceHigh),
         static_cast<int>(stabilityWindowSamples),
         static_cast<float>(speedJitterThresholdMps),
         useVibrationFusion == JNI_TRUE ? "on" : "off");
}

// ─── Guided calibration control (called from CalibrationActivity) ────────────

// Delegate to CalibrationEngine.beginGearCalibration. Passive K-Means feeding
// is suppressed from the DSP worker while a capture is active. (ref: DL-005)
JNIEXPORT void JNICALL
Java_dev_alfieprojects_gearsync_NativeEngine_beginGearCalibration(JNIEnv*, jclass, jint gear) {
    g_calibEngine.beginGearCalibration(static_cast<int>(gear));
    LOGI("Guided calibration started for gear %d", static_cast<int>(gear));
}

// Discard captured samples and return to passive mode. (ref: DL-006)
JNIEXPORT void JNICALL
Java_dev_alfieprojects_gearsync_NativeEngine_cancelCalibration(JNIEnv*, jclass) {
    g_calibEngine.cancelCalibration();
    LOGI("Guided calibration cancelled");
}

// Returns calibrationProgress() — range [0, 1]; polled by CalibrationActivity at ~10 Hz.
JNIEXPORT jfloat JNICALL
Java_dev_alfieprojects_gearsync_NativeEngine_getCalibrationProgress(JNIEnv*, jclass) {
    return static_cast<jfloat>(g_calibEngine.calibrationProgress());
}

} // extern "C"
