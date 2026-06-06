package com.app.shiftassistant

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.SweepGradient
import android.util.AttributeSet
import android.view.Choreographer
import android.view.View
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin

/**
 * Analog VU-meter needle view driven by native C++ state at 60 FPS.
 *
 * Three arc zones sweep from 225° (bottom-left) to 315° (bottom-right):
 *   Lugging   0 %–33 %   blue
 *   Optimal  33 %–66 %   green
 *   Redline  66 %–100 %  red
 *
 * Zone boundaries and SHIFT_THRESHOLD are derived from gear calibration
 * confidence returned in getVUMeterState()[4].
 */
class VUMeterView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    // Raw state from native engine
    private var needlePos    = 0f   // 0.0 … 1.0
    private var dominantHz   = 0f
    private var speedMps     = 0f
    private var gear         = 0    // 1-based, 0 = uncalibrated
    private var confidence   = 0f

    // Smooth needle using exponential moving average to avoid jitter
    private var smoothNeedle = 0f
    private val smoothAlpha  = 0.18f

    // Arc geometry: sweep from START_ANGLE for SWEEP_ANGLE degrees
    private val START_ANGLE  = 135f
    private val SWEEP_ANGLE  = 270f
    private val ARC_WIDTH    = 28f

    private val arcRect      = RectF()

    private val arcPaintLug  = buildPaint(Color.parseColor("#4A90D9"), ARC_WIDTH)
    private val arcPaintOpt  = buildPaint(Color.parseColor("#27AE60"), ARC_WIDTH)
    private val arcPaintRed  = buildPaint(Color.parseColor("#E74C3C"), ARC_WIDTH)
    private val arcPaintBg   = buildPaint(Color.parseColor("#1A1A2E"), ARC_WIDTH)

    private val needlePaint  = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color       = Color.WHITE
        strokeWidth = 4f
        style       = Paint.Style.STROKE
        strokeCap   = Paint.Cap.ROUND
    }
    private val pivotPaint   = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        style = Paint.Style.FILL
    }
    private val labelPaint   = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color     = Color.argb(180, 255, 255, 255)
        textSize  = 36f
        textAlign = Paint.Align.CENTER
    }
    private val valuePaint   = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color     = Color.WHITE
        textSize  = 52f
        textAlign = Paint.Align.CENTER
        isFakeBoldText = true
    }

    // ─── 60 FPS loop via Choreographer ───────────────────────────────────────

    private val choreographer = Choreographer.getInstance()

    private val frameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            val state = NativeEngine.getVUMeterState()
            if (state != null && state.size >= 5) {
                needlePos  = state[0]
                dominantHz = state[1]
                speedMps   = state[2]
                gear       = state[3].toInt()
                confidence = state[4]
            }
            smoothNeedle += smoothAlpha * (needlePos - smoothNeedle)
            invalidate()
            choreographer.postFrameCallback(this)
        }
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        setLayerType(LAYER_TYPE_HARDWARE, null)
        choreographer.postFrameCallback(frameCallback)
    }

    override fun onDetachedFromWindow() {
        choreographer.removeFrameCallback(frameCallback)
        super.onDetachedFromWindow()
    }

    // ─── Drawing ─────────────────────────────────────────────────────────────

    override fun onDraw(canvas: Canvas) {
        val cx   = width  / 2f
        val cy   = height / 2f
        val r    = min(cx, cy) * 0.82f
        val half = ARC_WIDTH / 2f

        arcRect.set(cx - r, cy - r, cx + r, cy + r)

        // Background arc
        canvas.drawArc(arcRect, START_ANGLE, SWEEP_ANGLE, false, arcPaintBg)

        // Zone arcs (each covers 1/3 of the sweep)
        val third = SWEEP_ANGLE / 3f
        canvas.drawArc(arcRect, START_ANGLE,           third, false, arcPaintLug)
        canvas.drawArc(arcRect, START_ANGLE + third,   third, false, arcPaintOpt)
        canvas.drawArc(arcRect, START_ANGLE + 2*third, third, false, arcPaintRed)

        // Needle
        val needleAngle = Math.toRadians((START_ANGLE + smoothNeedle * SWEEP_ANGLE).toDouble())
        val nx = (cx + r * cos(needleAngle)).toFloat()
        val ny = (cy + r * sin(needleAngle)).toFloat()
        val innerR = r * 0.30f
        val ix = (cx + innerR * cos(needleAngle)).toFloat()
        val iy = (cy + innerR * sin(needleAngle)).toFloat()

        needlePaint.strokeWidth = 5f
        canvas.drawLine(ix, iy, nx, ny, needlePaint)

        // Pivot dot
        canvas.drawCircle(cx, cy, 10f, pivotPaint)

        // Gear label
        val gearText = if (gear > 0) "GEAR $gear" else "—"
        canvas.drawText(gearText, cx, cy + r * 0.55f, valuePaint)

        // Hz / speed readout below
        val hzText = if (dominantHz > 0f) "${dominantHz.toInt()} Hz" else "···"
        canvas.drawText(hzText, cx, cy + r * 0.78f, labelPaint)

        // Confidence ring (thin stroke that fills clockwise)
        if (confidence > 0f) {
            val confPaint = Paint(arcPaintOpt).apply {
                strokeWidth = 4f
                alpha       = 120
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
