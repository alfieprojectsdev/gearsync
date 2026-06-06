#include <jni.h>
#include <android/log.h>
#include <android/sensor.h>

#include <oboe/Oboe.h>

#include "CalibrationEngine.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
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
static constexpr int   BLIP_SAMPLES       = SAMPLE_RATE * 50 / 1000; // 50 ms
static constexpr float BLIP_AMPLITUDE     = 0.45f;
static constexpr float MIN_ENGINE_HZ      = 20.0f;
static constexpr float MAX_ENGINE_HZ      = 250.0f;
static constexpr float MIN_SPEED_MPS      = 1.0f;   // ignore GPS jitter below 1 m/s
static constexpr float SHIFT_ACCEL_THRESH = 4.0f;   // m/s² spike → trigger blip
static constexpr int   SENSOR_US          = 10000;  // 100 Hz

// ─── Shared state (atomic where possible) ────────────────────────────────────

static std::atomic<float> g_gpsSpeed{0.0f};           // metres per second from GPS
static std::atomic<float> g_dominantHz{0.0f};         // last FFT peak
static std::atomic<float> g_needlePos{0.0f};          // 0.0 (lug) … 1.0 (redline)
static std::atomic<int>   g_currentGear{-1};          // 0-based gear index, -1 = unknown
static std::atomic<bool>  g_triggerBlip{false};       // request blip on next output frame
static std::atomic<float> g_audioCueFrequency{14200.0f}; // synthesized blip frequency in Hz

// ─── PCM ring buffer ──────────────────────────────────────────────────────────

static float        g_pcmRing[FFT_SIZE]{};
static int          g_pcmHead = 0;
static std::mutex   g_pcmMutex;

// ─── Calibration engine ───────────────────────────────────────────────────────

static CalibrationEngine g_calibEngine;

// ─── Inline radix-2 Cooley–Tukey FFT ─────────────────────────────────────────

static void fft_inplace(std::vector<std::complex<float>>& a) {
    const int n = static_cast<int>(a.size());
    // Bit-reversal permutation
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    // Butterfly stages
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
    // Hamming window + complex input
    std::vector<std::complex<float>> buf(len);
    for (int i = 0; i < len; ++i) {
        float w = 0.54f - 0.46f * std::cos(2.0f * static_cast<float>(M_PI) * i / (len - 1));
        buf[i]  = { pcm[i] * w, 0.0f };
    }
    fft_inplace(buf);

    // Find peak magnitude bin in [MIN_ENGINE_HZ, MAX_ENGINE_HZ]
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

// ─── Audio input callback (microphone → PCM → FFT) ───────────────────────────

class InputCallback : public oboe::AudioStreamDataCallback {
public:
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* /*stream*/,
                                          void* audioData,
                                          int32_t numFrames) override {
        auto* samples = static_cast<float*>(audioData);

        // Write into ring buffer
        {
            std::lock_guard<std::mutex> lk(g_pcmMutex);
            for (int i = 0; i < numFrames; ++i) {
                g_pcmRing[g_pcmHead % FFT_SIZE] = samples[i];
                g_pcmHead++;
            }
        }

        // Process once we have a full window
        if (g_pcmHead >= FFT_SIZE) {
            float snapshot[FFT_SIZE];
            {
                std::lock_guard<std::mutex> lk(g_pcmMutex);
                // Copy in time order from the ring
                int start = g_pcmHead % FFT_SIZE;
                for (int i = 0; i < FFT_SIZE; ++i)
                    snapshot[i] = g_pcmRing[(start + i) % FFT_SIZE];
            }

            float hz    = findDominantHz(snapshot, FFT_SIZE);
            float speed = g_gpsSpeed.load();
            g_dominantHz.store(hz);

            if (speed >= MIN_SPEED_MPS) {
                float ratio = hz / speed;
                g_calibEngine.updateWelford(ratio);

                int gear = g_calibEngine.classifyGear(ratio);
                g_currentGear.store(gear);

                // Map to needle position using gear-band boundaries
                auto ratios = g_calibEngine.getGearRatios();
                if (gear >= 0) {
                    // Needle: 0 = bottom of gear band, 1 = top of next gear's redline
                    float lo = (gear < NUM_GEARS - 1) ? ratios[gear + 1] : ratios[gear] * 0.8f;
                    float hi = ratios[gear];
                    float pos = (hi - lo) > 1e-6f ? (ratio - lo) / (hi - lo) : 0.5f;
                    g_needlePos.store(std::max(0.0f, std::min(1.0f, pos)));
                }
            }
        }

        return oboe::DataCallbackResult::Continue;
    }
};

// ─── Audio output callback (sine blip synthesis) ──────────────────────────────

class OutputCallback : public oboe::AudioStreamDataCallback {
public:
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* /*stream*/,
                                          void* audioData,
                                          int32_t numFrames) override {
        auto* out = static_cast<float*>(audioData);

        if (g_triggerBlip.exchange(false)) {
            m_blipRemaining = BLIP_SAMPLES;
        }

        // Read frequency once per buffer to keep the hot loop free of atomic loads.
        // 14200 Hz at 48 kHz (phase inc ≈ 1.857 rad/sample) is well below Nyquist;
        // the same holds at 44.1 kHz (Nyquist 22050 Hz), so no aliasing can occur.
        const float phaseInc = 2.0f * static_cast<float>(M_PI) *
                               g_audioCueFrequency.load() / SAMPLE_RATE;

        for (int i = 0; i < numFrames; ++i) {
            if (m_blipRemaining > 0) {
                // Short attack / decay envelope to avoid clicks
                float env = 1.0f;
                int   attack  = BLIP_SAMPLES / 10;
                int   release = BLIP_SAMPLES / 10;
                int   elapsed = BLIP_SAMPLES - m_blipRemaining;
                if (elapsed < attack)
                    env = static_cast<float>(elapsed) / attack;
                else if (m_blipRemaining < release)
                    env = static_cast<float>(m_blipRemaining) / release;

                out[i]   = BLIP_AMPLITUDE * env * std::sin(m_phase);
                m_phase += phaseInc;
                if (m_phase > 2.0f * static_cast<float>(M_PI))
                    m_phase -= 2.0f * static_cast<float>(M_PI);
                --m_blipRemaining;
            } else {
                out[i]  = 0.0f;
                m_phase = 0.0f;
            }
        }
        return oboe::DataCallbackResult::Continue;
    }

private:
    float m_phase          = 0.0f;
    int   m_blipRemaining  = 0;
};

// ─── Stream handles ───────────────────────────────────────────────────────────

static std::shared_ptr<oboe::AudioStream> g_inputStream;
static std::shared_ptr<oboe::AudioStream> g_outputStream;
static InputCallback                      g_inputCallback;
static OutputCallback                     g_outputCallback;

static void openStreams() {
    oboe::AudioStreamBuilder inBuilder;
    inBuilder.setDirection(oboe::Direction::Input)
             .setPerformanceMode(oboe::PerformanceMode::LowLatency)
             .setSharingMode(oboe::SharingMode::Exclusive)
             .setFormat(oboe::AudioFormat::Float)
             .setChannelCount(1)
             .setSampleRate(SAMPLE_RATE)
             .setDataCallback(&g_inputCallback);

    oboe::Result result = inBuilder.openStream(g_inputStream);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open input stream: %s", oboe::convertToText(result));
        return;
    }
    g_inputStream->requestStart();

    oboe::AudioStreamBuilder outBuilder;
    outBuilder.setDirection(oboe::Direction::Output)
              .setPerformanceMode(oboe::PerformanceMode::LowLatency)
              .setSharingMode(oboe::SharingMode::Exclusive)
              .setFormat(oboe::AudioFormat::Float)
              .setChannelCount(1)
              .setSampleRate(SAMPLE_RATE)
              .setDataCallback(&g_outputCallback);

    result = outBuilder.openStream(g_outputStream);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open output stream: %s", oboe::convertToText(result));
        return;
    }
    g_outputStream->requestStart();
    LOGI("Oboe streams open — sample rate %d Hz", SAMPLE_RATE);
}

static void closeStreams() {
    if (g_inputStream)  { g_inputStream->stop();  g_inputStream->close();  g_inputStream.reset(); }
    if (g_outputStream) { g_outputStream->stop(); g_outputStream->close(); g_outputStream.reset(); }
}

// ─── Sensor thread (LINEAR_ACCELERATION at 100 Hz) ───────────────────────────

static ASensorManager*    g_sensorManager   = nullptr;
static ASensorEventQueue* g_sensorEventQueue= nullptr;
static std::thread        g_sensorThread;
static std::atomic<bool>  g_sensorRunning{false};

static void sensorThreadFn() {
    ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);

    g_sensorManager = ASensorManager_getInstanceForPackage("com.app.shiftassistant");
    if (!g_sensorManager) {
        LOGE("ASensorManager unavailable");
        return;
    }

    const ASensor* accel = ASensorManager_getDefaultSensor(
            g_sensorManager, ASENSOR_TYPE_LINEAR_ACCELERATION);
    if (!accel) {
        LOGE("LINEAR_ACCELERATION sensor unavailable");
        return;
    }

    g_sensorEventQueue = ASensorManager_createEventQueue(
            g_sensorManager, looper, ALOOPER_EVENT_INPUT, nullptr, nullptr);

    ASensorEventQueue_enableSensor(g_sensorEventQueue, accel);
    ASensorEventQueue_setEventRate(g_sensorEventQueue, accel, SENSOR_US);
    LOGI("Sensor thread running at %d µs", SENSOR_US);

    ASensorEvent events[8];
    while (g_sensorRunning.load()) {
        int n = ASensorEventQueue_getEvents(g_sensorEventQueue, events, 8);
        if (n > 0) {
            for (int i = 0; i < n; ++i) {
                float ax = events[i].acceleration.x;
                float ay = events[i].acceleration.y;
                float az = events[i].acceleration.z;
                float magnitude = std::sqrt(ax*ax + ay*ay + az*az);

                // A sharp deceleration spike suggests a gear shift.
                if (magnitude > SHIFT_ACCEL_THRESH) {
                    g_triggerBlip.store(true);
                }
            }
        }
        // Yield briefly to avoid busy-spinning when no events are ready.
        ALooper_pollOnce(2 /*ms timeout*/, nullptr, nullptr, nullptr);
    }

    ASensorEventQueue_disableSensor(g_sensorEventQueue, accel);
    ASensorManager_destroyEventQueue(g_sensorManager, g_sensorEventQueue);
    g_sensorEventQueue = nullptr;
    LOGI("Sensor thread stopped");
}

// ─── JNI entry points ─────────────────────────────────────────────────────────

extern "C" {

JNIEXPORT void JNICALL
Java_com_app_shiftassistant_NativeEngine_startEngine(JNIEnv*, jclass) {
    openStreams();
    g_sensorRunning.store(true);
    g_sensorThread = std::thread(sensorThreadFn);
    LOGI("Native engine started");
}

JNIEXPORT void JNICALL
Java_com_app_shiftassistant_NativeEngine_stopEngine(JNIEnv*, jclass) {
    g_sensorRunning.store(false);
    if (g_sensorThread.joinable()) g_sensorThread.join();
    closeStreams();
    LOGI("Native engine stopped");
}

// Called from ShiftAssistantService at 1 Hz.
JNIEXPORT void JNICALL
Java_com_app_shiftassistant_NativeEngine_updateGpsSpeed(JNIEnv*, jclass, jfloat speed) {
    g_gpsSpeed.store(speed);
}

// Called from VUMeterView at 60 FPS.
// Returns float[5]: [needlePos, dominantHz, speed, gear+1 (1-based), confidence]
JNIEXPORT jfloatArray JNICALL
Java_com_app_shiftassistant_NativeEngine_getVUMeterState(JNIEnv* env, jclass) {
    jfloatArray result = env->NewFloatArray(5);
    if (!result) return nullptr;

    float needle = g_needlePos.load();
    float hz     = g_dominantHz.load();
    float speed  = g_gpsSpeed.load();
    int   gear   = g_currentGear.load();  // 0-based, -1 = unknown

    // Confidence proxy: variance of calibration engine (lower = more confident)
    float variance   = g_calibEngine.getVariance();
    float confidence = (variance > 0.0f) ? 1.0f / (1.0f + variance) : 0.0f;

    float buf[5] = {
        needle,
        hz,
        speed,
        static_cast<float>(gear + 1),   // 1-based for Kotlin display (0 = unknown)
        confidence
    };
    env->SetFloatArrayRegion(result, 0, 5, buf);
    return result;
}

// Restore Welford + gear-ratio state from a previous session.
// stateArray layout: [n_float, mean, m2, ratio0, ratio1, ratio2, ratio3, ratio4]
JNIEXPORT void JNICALL
Java_com_app_shiftassistant_NativeEngine_resumeCalibrationState(JNIEnv* env,
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
Java_com_app_shiftassistant_NativeEngine_saveCalibrationState(JNIEnv* env, jclass) {
    constexpr int LEN = 3 + NUM_GEARS;
    jfloatArray result = env->NewFloatArray(LEN);
    if (!result) return nullptr;

    float buf[LEN]{};
    g_calibEngine.serialise(buf, LEN);
    env->SetFloatArrayRegion(result, 0, LEN, buf);
    return result;
}

// Called from Kotlin settings UI to update the blip frequency at runtime.
JNIEXPORT void JNICALL
Java_com_app_shiftassistant_NativeEngine_setAudioCueFrequency(JNIEnv*, jclass, jfloat hz) {
    g_audioCueFrequency.store(hz);
    LOGI("Audio cue frequency set to %.1f Hz", static_cast<float>(hz));
}

} // extern "C"
