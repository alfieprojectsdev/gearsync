package dev.alfieprojects.gearsync

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.Choreographer
import android.view.View
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

/**
 * Analog VU-meter needle view driven by native C++ state at 60 FPS.
 *
 * Three arc zones sweep from 135° (bottom-left) to 45° (top-right), clockwise,
 * covering SWEEP_ANGLE = 270° in Android's Canvas coordinate system (0° = east):
 *   Lugging   0 %–33 %   blue
 *   Optimal  33 %–66 %   green
 *   Redline  66 %–100 %  red
 *
 * Visual feedback features:
 * - Arc zone brightness scales with calibration confidence (dim until locked in).
 * - A white gear-change flash ring fades out over ~400 ms when a shift is detected.
 * - Stability indicator: needle is drawn dimmer while GPS speed is not yet stable.
 */
class VUMeterView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    // Raw state from native engine
    private var needlePos      = 0f   // 0.0 … 1.0
    private var dominantHz     = 0f
    private var speedMps       = 0f
    private var gear           = 0    // 1-based, 0 = uncalibrated
    private var confidence     = 0f
    private var shiftDetected  = false

    // Smooth needle using exponential moving average to avoid jitter
    private var smoothNeedle   = 0f
    private val smoothAlpha    = 0.18f

    // Gear-change flash — decays each frame at 60 FPS (~400 ms at alpha 255)
    private var flashAlpha     = 0f                   // 0.0 … 255.0
    private val FLASH_DECAY    = 255f / (60f * 0.4f)  // fade in 0.4 s

    // Arc geometry: sweep from START_ANGLE for SWEEP_ANGLE degrees
    private val START_ANGLE    = 135f
    private val SWEEP_ANGLE    = 270f
    private val ARC_WIDTH      = 28f

    private val arcRect        = RectF()

    private val arcPaintLug    = buildPaint(Color.parseColor("#4A90D9"), ARC_WIDTH)
    private val arcPaintOpt    = buildPaint(Color.parseColor("#27AE60"), ARC_WIDTH)
    private val arcPaintRed    = buildPaint(Color.parseColor("#E74C3C"), ARC_WIDTH)
    private val arcPaintBg     = buildPaint(Color.parseColor("#1A1A2E"), ARC_WIDTH)

    private val flashPaint     = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color       = Color.WHITE
        strokeWidth = ARC_WIDTH * 0.6f
        style       = Paint.Style.STROKE
        strokeCap   = Paint.Cap.BUTT
    }
    private val needlePaint    = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color       = Color.WHITE
        strokeWidth = 5f
        style       = Paint.Style.STROKE
        strokeCap   = Paint.Cap.ROUND
    }
    private val pivotPaint     = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        style = Paint.Style.FILL
    }
    private val labelPaint     = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color     = Color.argb(180, 255, 255, 255)
        textSize  = 36f
        textAlign = Paint.Align.CENTER
    }
    private val valuePaint     = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color          = Color.WHITE
        textSize       = 52f
        textAlign      = Paint.Align.CENTER
        isFakeBoldText = true
    }

    // ─── 60 FPS loop via Choreographer ───────────────────────────────────────

    private val choreographer  = Choreographer.getInstance()
    private var isRunning      = false

    private val frameCallback  = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            val state = NativeEngine.getVUMeterState()
            if (state != null && state.size >= 6) {
                needlePos     = state[0]
                dominantHz    = state[1]
                speedMps      = state[2]
                gear          = state[3].toInt()
                confidence    = state[4]
                shiftDetected = state[5] != 0f
            }

            smoothNeedle += smoothAlpha * (needlePos - smoothNeedle)

            // Latch flash at full brightness on shift event; decay every frame.
            if (shiftDetected) flashAlpha = 255f
            else flashAlpha = max(0f, flashAlpha - FLASH_DECAY)

            invalidate()
            if (isRunning) choreographer.postFrameCallback(this)
        }
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        setLayerType(LAYER_TYPE_HARDWARE, null)
        if (visibility == VISIBLE) {
            isRunning = true
            choreographer.postFrameCallback(frameCallback)
        }
    }

    override fun onDetachedFromWindow() {
        isRunning = false
        choreographer.removeFrameCallback(frameCallback)
        super.onDetachedFromWindow()
    }

    override fun onVisibilityChanged(changedView: View, visibility: Int) {
        super.onVisibilityChanged(changedView, visibility)
        if (visibility == VISIBLE && !isRunning) {
            isRunning = true
            choreographer.postFrameCallback(frameCallback)
        } else if (visibility != VISIBLE && isRunning) {
            isRunning = false
            choreographer.removeFrameCallback(frameCallback)
        }
    }

    // ─── Drawing ─────────────────────────────────────────────────────────────

    override fun onDraw(canvas: Canvas) {
        val cx = width  / 2f
        val cy = height / 2f
        val r  = min(cx, cy) * 0.82f

        arcRect.set(cx - r, cy - r, cx + r, cy + r)

        // Background arc
        canvas.drawArc(arcRect, START_ANGLE, SWEEP_ANGLE, false, arcPaintBg)

        // Zone arcs — dim when confidence is low so the driver can see calibration is still learning.
        val zoneAlpha = (80 + (confidence * 175f).toInt()).coerceIn(80, 255)
        val third     = SWEEP_ANGLE / 3f
        arcPaintLug.alpha = zoneAlpha
        arcPaintOpt.alpha = zoneAlpha
        arcPaintRed.alpha = zoneAlpha
        canvas.drawArc(arcRect, START_ANGLE,           third, false, arcPaintLug)
        canvas.drawArc(arcRect, START_ANGLE + third,   third, false, arcPaintOpt)
        canvas.drawArc(arcRect, START_ANGLE + 2*third, third, false, arcPaintRed)

        // Gear-change flash ring — full-circle white arc that fades after a shift event.
        if (flashAlpha > 0f) {
            flashPaint.alpha = flashAlpha.toInt()
            canvas.drawArc(arcRect, START_ANGLE, SWEEP_ANGLE, false, flashPaint)
        }

        // Needle — use kotlin.math.PI for idiomatic conversion from degrees.
        val needleAngle = (START_ANGLE + smoothNeedle * SWEEP_ANGLE) * PI / 180.0
        val nx          = (cx + r * cos(needleAngle)).toFloat()
        val ny          = (cy + r * sin(needleAngle)).toFloat()
        val innerR      = r * 0.30f
        val ix          = (cx + innerR * cos(needleAngle)).toFloat()
        val iy          = (cy + innerR * sin(needleAngle)).toFloat()

        // Dim needle while engine has no GPS lock or speed is zero.
        needlePaint.alpha = if (speedMps > 0f) 255 else 100
        canvas.drawLine(ix, iy, nx, ny, needlePaint)

        // Pivot dot
        pivotPaint.alpha = needlePaint.alpha
        canvas.drawCircle(cx, cy, 10f, pivotPaint)

        // Gear number — white when known, muted when uncalibrated.
        val gearText = if (gear > 0) "GEAR $gear" else "—"
        valuePaint.alpha = if (gear > 0) 255 else 120
        canvas.drawText(gearText, cx, cy + r * 0.55f, valuePaint)

        // Hz readout below gear number.
        val hzText = if (dominantHz > 0f) "${dominantHz.toInt()} Hz" else "···"
        canvas.drawText(hzText, cx, cy + r * 0.78f, labelPaint)

        // Confidence ring — thin stroke that fills clockwise as K-Means locks in.
        if (confidence > 0f) {
            val confPaint = Paint(arcPaintOpt).apply {
                strokeWidth = 4f
                alpha       = 140
            }
            canvas.drawArc(arcRect, START_ANGLE, SWEEP_ANGLE * confidence, false, confPaint)
        }
    }

    // ─── Helpers ─────────────────────────────────────────────────────────────

    private fun buildPaint(color: Int, width: Float) = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        this.color       = color
        this.strokeWidth = width
        this.style       = Paint.Style.STROKE
        this.strokeCap   = Paint.Cap.BUTT
    }
}
