#include <jni.h>
#include <android/log.h>
#include <android/sensor.h>

#include <oboe/Oboe.h>

#include "AccelVibrationDsp.h"
#include "CalibrationEngine.h"
#include "FusionPolicy.h"
#include "GearHysteresis.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
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
// ADR 008 gear-display hysteresis: a classification must hold this many consecutive
// DSP frames before it replaces the shown gear; while linear accel exceeds the
// transient threshold (hard accel/brake → stale GPS v), the gear is held.
static constexpr int   GEAR_STABLE_FRAMES   = 3;
static constexpr float GEAR_TRANSIENT_ACCEL = 2.0f;      // m/s² gravity-removed

// ─── ADR 004 M0 in-app accel-rate probe ──────────────────────────────────────
// Requests raw ACCELEROMETER at fastest delivery and measures the rate/jitter our
// own ASensorEventQueue path actually achieves (phyphox bench read 400 Hz; this
// confirms our code path lands there too). Gate for fusion is >= ~300 Hz. (DL-001/007/009)
static constexpr float ACCEL_RATE_GATE_HZ  = 300.0f;
static constexpr int   ACCEL_PROBE_WINDOW  = 512;         // intervals per published stats window
static constexpr uint32_t ACCEL_RING_SIZE  = 1024;        // > 0.64 s @ 400 Hz, power of two
static constexpr uint32_t ACCEL_RING_MASK  = ACCEL_RING_SIZE - 1;
static_assert((ACCEL_RING_SIZE & ACCEL_RING_MASK) == 0, "ACCEL_RING_SIZE must be power of two");

enum VibrationDisabledReason : int {
    VIB_REASON_NONE = 0,             // gate open; fusion live (M4)
    VIB_REASON_CONFIG_DISABLED = 1,
    VIB_REASON_ACCEL_UNSUPPORTED = 2,
    VIB_REASON_LOW_RATE = 3
};

// Numerically mirrored by FusionSourceMode in FusionPolicy.h.
enum VibrationSourceMode : int {
    VIB_SOURCE_MIC_ONLY = 0,
    VIB_SOURCE_FUSED = 1,
    VIB_SOURCE_REJECTED_LOW_RATE = 2,
    VIB_SOURCE_REJECTED_LOW_PROMINENCE = 3,
    VIB_SOURCE_REJECTED_DISAGREEMENT = 4
};
static_assert(VIB_SOURCE_MIC_ONLY == FUSION_SRC_MIC_ONLY &&
              VIB_SOURCE_FUSED == FUSION_SRC_FUSED &&
              VIB_SOURCE_REJECTED_LOW_PROMINENCE == FUSION_SRC_REJECTED_LOW_PROMINENCE &&
              VIB_SOURCE_REJECTED_DISAGREEMENT == FUSION_SRC_REJECTED_DISAGREEMENT,
              "VibrationSourceMode must stay in lockstep with FusionSourceMode");

// ─── Shared state (atomic where possible) ────────────────────────────────────

static std::atomic<float>    g_gpsSpeed{0.0f};            // metres per second from GPS
static std::atomic<float>    g_dominantHz{0.0f};          // last FFT peak
static std::atomic<float>    g_needlePos{0.0f};           // 0.0 (lug) … 1.0 (redline)
static std::atomic<int>      g_currentGear{-1};           // 0-based gear index, -1 = unknown
static std::atomic<bool>     g_shiftDetected{false};      // set by sensor thread; consumed by VU meter
static std::atomic<float>    g_linearAccelMag{0.0f};      // ADR 008: gravity-removed |accel|, m/s²

// ─── ADR 004 M0 probe stats (written by sensor thread, read via JNI diagnostic) ─
static std::atomic<float>    g_accelEffHz{0.0f};          // measured effective rate, Hz
static std::atomic<float>    g_accelMinIntervalMs{0.0f};  // min inter-arrival in last window
static std::atomic<float>    g_accelMaxIntervalMs{0.0f};  // max inter-arrival in last window
static std::atomic<float>    g_accelJitterMs{0.0f};       // stddev of inter-arrival in last window
static std::atomic<float>    g_accelSampleCount{0.0f};    // cumulative samples seen
static std::atomic<int>      g_accelSupported{-1};        // -1 unknown, 0 unsupported, 1 supported
static std::atomic<float>    g_accelRequestedHz{0.0f};    // requested raw-accel rate from min-delay

// ─── ADR 004 accelerometer SPSC ring ────────────────────────────────────────
// Sensor thread is the only producer. The DSP worker is the only consumer.
// Entries are timestamped raw-ACCELEROMETER magnitudes. M3 consumes them in the
// existing DSP worker for resampling + vibration FFT; fusion policy waits for M4.

static AccelSample           g_accelRing[ACCEL_RING_SIZE]{};
static std::atomic<uint32_t> g_accelRingWriteSeq{0};
static std::atomic<uint32_t> g_accelRingReadSeq{0};
static std::atomic<uint32_t> g_accelRingDropped{0};
static std::atomic<uint32_t> g_accelRingWritten{0};
static std::atomic<uint32_t> g_accelRingRead{0};
static std::atomic<float>    g_accelRingLatestMagnitude{0.0f};

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

// ─── ADR 004 vibration-fusion diagnostics ───────────────────────────────────
// M3 adds the vibration FFT estimate only. Fusion policy lands in M4, so
// fusionActive remains false and source remains mic-primary.

static std::atomic<bool>     g_vibrationFusionActive{false};
static std::atomic<int>      g_vibrationDisabledReason{VIB_REASON_CONFIG_DISABLED};
static std::atomic<int>      g_vibrationSourceMode{VIB_SOURCE_MIC_ONLY};
static std::atomic<float>    g_vibrationHz{0.0f};
static std::atomic<float>    g_vibrationProminence{0.0f};

static void updateVibrationFusionDiagnostics() {
    const bool requested = g_useVibrationFusion.load(std::memory_order_relaxed);
    const int supported = g_accelSupported.load(std::memory_order_relaxed);
    const float effHz = g_accelEffHz.load(std::memory_order_relaxed);

    if (!requested) {
        g_vibrationFusionActive.store(false, std::memory_order_relaxed);
        g_vibrationHz.store(0.0f, std::memory_order_relaxed);
        g_vibrationProminence.store(0.0f, std::memory_order_relaxed);
        g_vibrationDisabledReason.store(VIB_REASON_CONFIG_DISABLED, std::memory_order_relaxed);
        g_vibrationSourceMode.store(VIB_SOURCE_MIC_ONLY, std::memory_order_relaxed);
    } else if (supported == 0) {
        g_vibrationFusionActive.store(false, std::memory_order_relaxed);
        g_vibrationHz.store(0.0f, std::memory_order_relaxed);
        g_vibrationProminence.store(0.0f, std::memory_order_relaxed);
        g_vibrationDisabledReason.store(VIB_REASON_ACCEL_UNSUPPORTED, std::memory_order_relaxed);
        g_vibrationSourceMode.store(VIB_SOURCE_MIC_ONLY, std::memory_order_relaxed);
    } else if (effHz < ACCEL_RATE_GATE_HZ) {
        g_vibrationFusionActive.store(false, std::memory_order_relaxed);
        g_vibrationHz.store(0.0f, std::memory_order_relaxed);
        g_vibrationProminence.store(0.0f, std::memory_order_relaxed);
        g_vibrationDisabledReason.store(VIB_REASON_LOW_RATE, std::memory_order_relaxed);
        g_vibrationSourceMode.store(VIB_SOURCE_REJECTED_LOW_RATE, std::memory_order_relaxed);
    } else {
        // Gate open: M3 has published f_vib/prominence. The per-frame fusion
        // decision in dspWorkerFn owns g_vibrationSourceMode / g_vibrationFusionActive
        // (it runs after this on the same thread and persists between PCM frames),
        // so this only sets the disabled-reason. Do not clobber source/active here.
        g_vibrationDisabledReason.store(VIB_REASON_NONE, std::memory_order_relaxed);
    }
}

static void resetAccelRing() {
    g_accelRingWriteSeq.store(0, std::memory_order_relaxed);
    g_accelRingReadSeq.store(0, std::memory_order_relaxed);
    g_accelRingDropped.store(0, std::memory_order_relaxed);
    g_accelRingWritten.store(0, std::memory_order_relaxed);
    g_accelRingRead.store(0, std::memory_order_relaxed);
    g_accelRingLatestMagnitude.store(0.0f, std::memory_order_relaxed);
}

static inline void writeAccelRingSample(int64_t timestampNs, float magnitude) {
    if (timestampNs <= 0 || !std::isfinite(magnitude)) return;

    const uint32_t w = g_accelRingWriteSeq.load(std::memory_order_relaxed);
    const uint32_t r = g_accelRingReadSeq.load(std::memory_order_acquire);
    if (static_cast<uint32_t>(w - r) >= ACCEL_RING_SIZE) {
        g_accelRingDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    g_accelRing[w & ACCEL_RING_MASK] = AccelSample{timestampNs, magnitude};
    g_accelRingLatestMagnitude.store(magnitude, std::memory_order_relaxed);
    g_accelRingWritten.fetch_add(1, std::memory_order_relaxed);
    g_accelRingWriteSeq.store(w + 1, std::memory_order_release);
}

static int drainAccelRingToWindow(AccelSample* window, int& head, int& count) {
    const uint32_t r = g_accelRingReadSeq.load(std::memory_order_relaxed);
    const uint32_t w = g_accelRingWriteSeq.load(std::memory_order_acquire);
    const uint32_t available = static_cast<uint32_t>(w - r);
    if (!window || available == 0) return 0;

    uint32_t drained = 0;
    for (uint32_t seq = r; seq != w; ++seq) {
        const AccelSample sample = g_accelRing[seq & ACCEL_RING_MASK];
        if (sample.timestampNs > 0 && std::isfinite(sample.magnitude)) {
            window[head] = sample;
            head = (head + 1) % ACCEL_VIBRATION_FFT_SIZE;
            if (count < ACCEL_VIBRATION_FFT_SIZE) ++count;
            g_accelRingLatestMagnitude.store(sample.magnitude, std::memory_order_relaxed);
            ++drained;
        }
    }
    g_accelRingRead.fetch_add(available, std::memory_order_relaxed);
    g_accelRingReadSeq.store(w, std::memory_order_release);
    return static_cast<int>(drained);
}

static void copyAccelWindowOrdered(const AccelSample* window, int head, AccelSample* ordered) {
    for (int i = 0; i < ACCEL_VIBRATION_FFT_SIZE; ++i) {
        ordered[i] = window[(head + i) % ACCEL_VIBRATION_FFT_SIZE];
    }
}

static bool accelVibrationEstimateEnabled() {
    return g_useVibrationFusion.load(std::memory_order_relaxed) &&
           g_accelSupported.load(std::memory_order_relaxed) == 1 &&
           g_accelEffHz.load(std::memory_order_relaxed) >= ACCEL_RATE_GATE_HZ;
}

static void publishAccelVibrationEstimate(const AccelVibrationEstimate& estimate) {
    if (estimate.valid) {
        g_vibrationHz.store(estimate.hz, std::memory_order_relaxed);
        g_vibrationProminence.store(estimate.prominence, std::memory_order_relaxed);
    } else {
        g_vibrationHz.store(0.0f, std::memory_order_relaxed);
        g_vibrationProminence.store(0.0f, std::memory_order_relaxed);
    }
    updateVibrationFusionDiagnostics();
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

// ─── DSP: find dominant frequency in engine band ──────────────────────────────

// outProminence (optional, ADR 004 M4): peak / median-band-magnitude — the same
// confidence metric the vibration estimate reports, so the fusion policy can
// weight mic vs. vibration symmetrically.
static float findDominantHz(const float* pcm, int len, float* outProminence = nullptr) {
    std::vector<std::complex<float>> buf(len);
    for (int i = 0; i < len; ++i) {
        float w = 0.54f - 0.46f * std::cos(2.0f * static_cast<float>(M_PI) * i / (len - 1));
        buf[i]  = { pcm[i] * w, 0.0f };
    }
    fft_inplace(buf);

    int binMin = static_cast<int>(MIN_ENGINE_HZ * len / SAMPLE_RATE);
    int binMax = static_cast<int>(MAX_ENGINE_HZ * len / SAMPLE_RATE);
    binMin     = std::max(1, binMin);
    binMax     = std::min(binMax, len / 2 - 1);

    float peakMag = 0.0f;
    int   peakBin = binMin;
    for (int b = binMin; b <= binMax; ++b) {
        float mag = std::abs(buf[b]);
        if (mag > peakMag) { peakMag = mag; peakBin = b; }
    }

    if (outProminence) {
        float prominence = 0.0f;
        if (binMax >= binMin && peakMag > 0.0f) {
            // Median band magnitude as the noise floor (nth_element). This runs on
            // the DSP worker, which already heap-allocates `buf` above — not a
            // wait-free path, so the small band vector is acceptable here.
            const int count = binMax - binMin + 1;
            std::vector<float> band(count);
            for (int b = binMin; b <= binMax; ++b) band[b - binMin] = std::abs(buf[b]);
            const int mid = count / 2;
            std::nth_element(band.begin(), band.begin() + mid, band.end());
            const float noiseFloor = std::max(band[mid], 1.0e-6f);
            prominence = peakMag / noiseFloor;
        }
        *outProminence = prominence;
    }

    return static_cast<float>(peakBin) * SAMPLE_RATE / static_cast<float>(len);
}

// ─── DSP worker thread ────────────────────────────────────────────────────────
// Runs all heavy computation (FFT, Welford, K-Means, needle mapping) off the
// real-time audio thread to prevent buffer underruns and audio glitches.

static void dspWorkerFn() {
    // Thread-local copy of the snapshot — avoids any overlap with the input callback.
    float localSnapshot[FFT_SIZE];
    AccelSample accelWindow[ACCEL_VIBRATION_FFT_SIZE]{};
    AccelSample accelOrdered[ACCEL_VIBRATION_FFT_SIZE]{};
    AccelVibrationScratch accelScratch{};
    int accelWindowHead = 0;
    int accelWindowCount = 0;
    int accelSamplesSinceEstimate = 0;
    GearHysteresis gearHysteresis;   // ADR 008: stabilises the displayed gear

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
        const int drained = drainAccelRingToWindow(
                accelWindow, accelWindowHead, accelWindowCount);
        accelSamplesSinceEstimate += drained;

        if (accelSamplesSinceEstimate >= ACCEL_VIBRATION_HOP_SIZE &&
            accelWindowCount >= ACCEL_VIBRATION_FFT_SIZE) {
            if (accelVibrationEstimateEnabled()) {
                copyAccelWindowOrdered(accelWindow, accelWindowHead, accelOrdered);
                const float accelHz = g_accelEffHz.load(std::memory_order_relaxed);
                publishAccelVibrationEstimate(estimateAccelVibrationHz(
                        accelOrdered, ACCEL_VIBRATION_FFT_SIZE, accelHz, accelScratch));
            } else {
                publishAccelVibrationEstimate({false, 0.0f, 0.0f, 0.0f});
            }
            accelSamplesSinceEstimate = 0;
        }

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

        float micProm = 0.0f;
        float hz      = findDominantHz(localSnapshot, FFT_SIZE, &micProm);
        float speed   = g_gpsSpeed.load();
        g_dominantHz.store(hz);  // diagnostic stays the raw mic peak

        // ─── ADR 004 M4 — mic-primary fusion ─────────────────────────────────
        // The gate mirrors accelVibrationEstimateEnabled(); when closed, selectFusedHz
        // returns the mic estimate untouched (fusion-off == legacy mic-only path).
        const bool  gateOpen = accelVibrationEstimateEnabled();
        const float vibHz    = g_vibrationHz.load(std::memory_order_relaxed);
        const float vibProm  = g_vibrationProminence.load(std::memory_order_relaxed);
        const bool  vibValid = gateOpen && vibHz > 0.0f && vibProm > 0.0f;
        const FusionDecision fd =
                selectFusedHz(hz, micProm, vibValid, vibHz, vibProm, gateOpen);
        const float selectedHz = fd.selectedHz;

        // This per-frame decision is authoritative for the source/active diagnostics
        // (updateVibrationFusionDiagnostics defers to it while the gate is open).
        g_vibrationSourceMode.store(fd.sourceMode, std::memory_order_relaxed);
        g_vibrationFusionActive.store(fd.fused, std::memory_order_relaxed);

        if (speed >= MIN_SPEED_MPS) {
            float ratio     = selectedHz / speed;
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
                    locked = g_calibEngine.feedCalibrationSample(speed, selectedHz);
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

            // ADR 008: stabilise the displayed gear against 1 Hz-GPS twitch — require
            // GEAR_STABLE_FRAMES consecutive consistent classifications, and freeze
            // during high linear-accel transients (when v is stalest).
            const int rawGear = g_calibEngine.classifyGear(ratio);
            const bool transient =
                g_linearAccelMag.load(std::memory_order_relaxed) > GEAR_TRANSIENT_ACCEL;
            const int gear = gearHysteresis.update(rawGear, transient, GEAR_STABLE_FRAMES);
            g_currentGear.store(gear);

            // Needle must stay consistent with the *displayed* gear. Only re-derive it
            // when the held gear matches the live classification and we're not in a
            // transient — otherwise the live `ratio` may sit outside the held gear's
            // band and the needle would peg to an extreme (a false "shift now") while
            // the gear shows a stale number. In that case freeze the needle (hold the
            // last good value) so gear and needle never contradict each other.
            if (!transient && gear >= 0 && gear == rawGear) {
                auto  ratios = g_calibEngine.getGearRatios();
                float lo     = (gear < NUM_GEARS - 1) ? ratios[gear + 1] : ratios[gear] * 0.8f;
                float hi     = ratios[gear];
                float pos    = (hi - lo) > 1e-6f ? (ratio - lo) / (hi - lo) : 0.5f;
                g_needlePos.store(std::max(0.0f, std::min(1.0f, pos)));
            } else if (!transient && gear < 0) {
                g_needlePos.store(0.0f);  // committed unknown (not a transient) → lug end
            }
            // else: gear held vs. live classification, or transient → leave needle frozen.
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
                const float linAccel = std::fabs(mag - gravityEma);
                g_linearAccelMag.store(linAccel, std::memory_order_relaxed);  // ADR 008 transient gate
                if (linAccel > SHIFT_ACCEL_THRESH) {
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
                writeAccelRingSample(ts, mag);

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
    resetAccelRing();

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

// ADR 004 diagnostic. Returns float[12]:
//   [requestedAccelHz, measuredAccelHz, useVibrationFusion (1/0),
//    fusionActive (1/0), disabledReasonCode, latestVibrationHz,
//    vibrationProminence, sourceModeCode, accelRingWritten, accelRingRead,
//    accelRingDropped, latestAccelMagnitude]
JNIEXPORT jfloatArray JNICALL
Java_dev_alfieprojects_gearsync_NativeEngine_nativeVibrationFusionStats(JNIEnv* env, jclass) {
    updateVibrationFusionDiagnostics();
    jfloatArray result = env->NewFloatArray(12);
    if (!result) return nullptr;
    float buf[12] = {
        g_accelRequestedHz.load(),
        g_accelEffHz.load(),
        g_useVibrationFusion.load() ? 1.0f : 0.0f,
        g_vibrationFusionActive.load() ? 1.0f : 0.0f,
        static_cast<float>(g_vibrationDisabledReason.load()),
        g_vibrationHz.load(),
        g_vibrationProminence.load(),
        static_cast<float>(g_vibrationSourceMode.load()),
        static_cast<float>(g_accelRingWritten.load()),
        static_cast<float>(g_accelRingRead.load()),
        static_cast<float>(g_accelRingDropped.load()),
        g_accelRingLatestMagnitude.load()
    };
    env->SetFloatArrayRegion(result, 0, 12, buf);
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
