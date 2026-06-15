package dev.alfieprojects.gearsync

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.sin

/**
 * ADR 006 M2 — out-of-band shift-cue player.
 *
 * Synthesises two short linear chirps (Kotlin port of the ToneCue.h spike) and
 * plays them on a **Shared, normal-latency** AudioTrack — NOT the Exclusive
 * low-latency path the mic-capture DSP owns, so it cannot recreate the v1
 * resource contention (ADR 006, fix A). Tones sit at **1.5–2.2 kHz**, far above
 * the 20–250 Hz engine FFT band, so even when the mic hears them `findDominantHz`
 * ignores them (fix B). Pitch-direction = shift-direction: ascending = upshift,
 * descending = downshift.
 *
 * NOTE: the no-mic-contamination / no-xrun guarantees are still the ADR 006 M0
 * device gate; this class is the player, validated on-device during that probe.
 */
class CuePlayer(private val sampleRate: Int = SAMPLE_RATE) {

    private val upTrack = buildStaticTrack(renderChirpPcm(UP_START_HZ, UP_END_HZ))
    private val downTrack = buildStaticTrack(renderChirpPcm(DOWN_START_HZ, DOWN_END_HZ))
    private var released = false

    fun play(intent: CueIntent) {
        if (released) return
        val track = when (intent) {
            CueIntent.UPSHIFT -> upTrack
            CueIntent.DOWNSHIFT -> downTrack
            CueIntent.NONE -> return
        }
        // Static-buffer replay: rewind to the start, then play.
        track.stop()
        track.reloadStaticData()
        track.play()
    }

    fun release() {
        if (released) return
        released = true
        upTrack.release()
        downTrack.release()
    }

    private fun renderChirpPcm(startHz: Float, endHz: Float): ShortArray {
        val n = (DURATION_SEC * sampleRate).toInt()
        val out = ShortArray(n)
        var phase = 0.0
        for (i in 0 until n) {
            val frac = if (n > 1) i.toDouble() / (n - 1) else 0.0
            val instHz = startHz + (endHz - startHz) * frac
            phase += 2.0 * PI * instHz / sampleRate
            val env = 0.5 * (1.0 - cos(2.0 * PI * frac))   // Hann — no click transients
            out[i] = (AMPLITUDE * env * sin(phase) * Short.MAX_VALUE).toInt().toShort()
        }
        return out
    }

    private fun buildStaticTrack(pcm: ShortArray): AudioTrack {
        val sizeBytes = pcm.size * 2
        val track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    // Sonification routes through the normal mixer (Shared), not the
                    // low-latency exclusive fast path the mic stream uses.
                    .setUsage(AudioAttributes.USAGE_ASSISTANCE_SONIFICATION)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(sampleRate)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build()
            )
            .setBufferSizeInBytes(sizeBytes)
            .setTransferMode(AudioTrack.MODE_STATIC)
            .build()
        track.write(pcm, 0, pcm.size)
        return track
    }

    companion object {
        private const val SAMPLE_RATE = 44100
        private const val DURATION_SEC = 0.12f
        private const val AMPLITUDE = 0.6
        // Both in the 1.5–2.2 kHz out-of-band window; distinguished by direction.
        private const val UP_START_HZ = 1500f
        private const val UP_END_HZ = 2200f
        private const val DOWN_START_HZ = 2200f
        private const val DOWN_END_HZ = 1500f
    }
}
