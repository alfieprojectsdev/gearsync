package dev.alfieprojects.gearsync

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.Choreographer
import android.view.MotionEvent
import android.view.View
import android.widget.Toast
import kotlin.math.PI
import kotlin.math.max
import kotlin.math.sin

/**
 * Horizontal segmented VU meter driven by native C++ state at 60 FPS.
 * The native payload remains [needlePos, dominantHz, speedMps, gear, confidence, shiftDetected].
 */
class VUMeterView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    // Raw state from native engine
    private var needlePos      = 0f   // 0.0 … 1.0
    private var gear           = 0    // 1-based, 0 = uncalibrated
    private var shiftDetected  = false
    private var gearLabel      = NEUTRAL_LABEL

    // Smooth fill using exponential moving average to avoid jitter.
    private var smoothNeedle   = 0f
    private val smoothAlpha    = 0.18f

    // Gear-change flash outlines the active bars and decays over ~400 ms.
    private var flashAlpha     = 0f                   // 0.0 … 255.0
    private val FLASH_DECAY    = 255f / (60f * 0.4f)  // fade in 0.4 s

    private val lugSegmentColor     = context.getColor(R.color.vu_lug_segment)
    private val optSegmentColor     = context.getColor(R.color.vu_opt_segment)
    private val shiftSegmentColor   = context.getColor(R.color.vu_shift_segment)
    private val inactiveSegmentColor = context.getColor(R.color.vu_inactive_segment)
    private val lugAmbientColor     = context.getColor(R.color.vu_ambient_lug_base)
    private val shiftAmbientColor   = context.getColor(R.color.vu_ambient_shift_base)
    private val needleEdgeColor     = context.getColor(R.color.vu_needle_edge)
    private val targetMarkerColor   = context.getColor(R.color.vu_target_marker)

    // Reused per-frame to avoid allocation in onDraw.
    private val segmentRect   = RectF()

    private val segmentPaint  = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
    }
    private val needleEdgePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = needleEdgeColor
        style = Paint.Style.FILL
    }
    private val targetMarkerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = targetMarkerColor
        style = Paint.Style.FILL
    }
    private val flashPaint    = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        style = Paint.Style.FILL
    }
    private val textStrokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.BLACK
        textAlign = Paint.Align.CENTER
        isFakeBoldText = true
        style = Paint.Style.STROKE
        strokeWidth = 8f
    }
    private val textPaint     = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        textAlign = Paint.Align.CENTER
        isFakeBoldText = true
        style = Paint.Style.FILL
    }

    // ─── 60 FPS loop via Choreographer ───────────────────────────────────────

    private val choreographer  = Choreographer.getInstance()
    private var isRunning      = false

    private val frameCallback  = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            val state = NativeEngine.getVUMeterState()
            if (state != null && state.size >= 5) {
                needlePos     = state[0]
                gear          = state[3].toInt()
                shiftDetected = state.size >= 6 && state[5] != 0f
                gearLabel     = if (gear > 0) {
                    GEAR_LABELS.getOrElse(gear) { NEUTRAL_LABEL }
                } else {
                    NEUTRAL_LABEL
                }
            }

            smoothNeedle += smoothAlpha * (needlePos.coerceIn(0f, 1f) - smoothNeedle)

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

    // ─── Hidden demo toggle: triple-tap the upper-right corner ───────────────
    // Flips NativeEngine.demoMode so the meter animates from synthetic DebugSweep
    // frames with no mic/GPS/sensors/service — works in any build (an easter-egg
    // demo, not the `sweep` build type).

    private var cornerTapCount = 0
    private var lastCornerTapMs = 0L

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_DOWN) {
            val inCorner = event.x > width * CORNER_HIT_FRACTION &&
                           event.y < height * CORNER_HIT_FRACTION
            val now = System.currentTimeMillis()
            if (inCorner) {
                cornerTapCount =
                    if (now - lastCornerTapMs <= TRIPLE_TAP_WINDOW_MS) cornerTapCount + 1 else 1
                lastCornerTapMs = now
                if (cornerTapCount >= 3) {
                    cornerTapCount = 0
                    NativeEngine.demoMode = !NativeEngine.demoMode
                    Toast.makeText(
                        context,
                        if (NativeEngine.demoMode) R.string.demo_on else R.string.demo_off,
                        Toast.LENGTH_SHORT
                    ).show()
                }
                return true
            }
            cornerTapCount = 0
        }
        return super.onTouchEvent(event)
    }

    // ─── Drawing ─────────────────────────────────────────────────────────────

    override fun onDraw(canvas: Canvas) {
        val fill = smoothNeedle.coerceIn(0f, 1f)
        val alertFill = needlePos.coerceIn(0f, 1f)

        drawPeripheralWash(canvas, alertFill)
        drawSegments(canvas, fill)
        drawTargetMarker(canvas)
        drawNeedleEdge(canvas, fill)
        drawGearLabel(canvas)
    }

    private fun drawPeripheralWash(canvas: Canvas, fill: Float) {
        when {
            fill < LUG_END -> {
                val intensity = (LUG_END - fill) / LUG_END
                val alpha = (intensity * LUG_MAX_ALPHA).toInt().coerceIn(0, LUG_MAX_ALPHA)
                canvas.drawColor(withAlpha(lugAmbientColor, alpha))
            }
            fill > OPT_END -> {
                val alpha = if (fill > PULSE_START) {
                    val seconds = System.currentTimeMillis() / 1000.0
                    val wave = (sin(seconds * TWO_PI * PULSE_HZ) + 1.0) * 0.5
                    (PULSE_MIN_ALPHA + wave * (PULSE_MAX_ALPHA - PULSE_MIN_ALPHA)).toInt()
                } else {
                    val intensity = ((fill - OPT_END) / (PULSE_START - OPT_END)).coerceIn(0f, 1f)
                    (intensity * SHIFT_RAMP_MAX_ALPHA).toInt()
                }.coerceIn(0, 255)
                canvas.drawColor(withAlpha(shiftAmbientColor, alpha))
            }
        }
    }

    private fun drawSegments(canvas: Canvas, fill: Float) {
        val meterWidth = width.toFloat()
        val meterHeight = height.toFloat()
        val gap = SEGMENT_GAP_PX
        val segmentWidth = (meterWidth - gap * (SEGMENT_COUNT - 1)) / SEGMENT_COUNT
        val top = meterHeight * 0.18f
        val bottom = meterHeight * 0.72f
        val radius = (minOf(segmentWidth, bottom - top) * CORNER_FRACTION)
        val activeSegments = (fill * SEGMENT_COUNT + 0.999f).toInt().coerceIn(0, SEGMENT_COUNT)

        var left = 0f
        for (i in 0 until SEGMENT_COUNT) {
            segmentPaint.color = when {
                i >= activeSegments -> inactiveSegmentColor
                i < LUG_SEGMENTS -> lugSegmentColor
                i < OPT_SEGMENTS -> optSegmentColor
                else -> shiftSegmentColor
            }
            val right = left + segmentWidth
            segmentRect.set(left, top, right, bottom)
            canvas.drawRoundRect(segmentRect, radius, radius, segmentPaint)

            if (flashAlpha > 0f && i < activeSegments && i >= OPT_SEGMENTS) {
                flashPaint.alpha = (flashAlpha * 0.35f).toInt().coerceIn(0, 90)
                canvas.drawRoundRect(segmentRect, radius, radius, flashPaint)
            }

            left = right + gap
        }
    }

    // Bright vertical cap at the fill boundary — a "needle tip" the eye catches in
    // peripheral vision even when the segment colours blur together at a glance.
    private fun drawNeedleEdge(canvas: Canvas, fill: Float) {
        val activeSegments = (fill * SEGMENT_COUNT + 0.999f).toInt().coerceIn(0, SEGMENT_COUNT)
        if (activeSegments <= 0) return

        val meterHeight = height.toFloat()
        val gap = SEGMENT_GAP_PX
        val segmentWidth = (width.toFloat() - gap * (SEGMENT_COUNT - 1)) / SEGMENT_COUNT
        val top = meterHeight * 0.14f
        val bottom = meterHeight * 0.76f
        // Right edge of the last active segment.
        val edgeX = activeSegments * (segmentWidth + gap) - gap
        val half = NEEDLE_EDGE_WIDTH_PX / 2f
        segmentRect.set(edgeX - half, top, edgeX + half, bottom)
        canvas.drawRoundRect(segmentRect, half, half, needleEdgePaint)
    }

    // Static amber marker at the optimal→redline boundary: the upshift point.
    private fun drawTargetMarker(canvas: Canvas) {
        val meterHeight = height.toFloat()
        val gap = SEGMENT_GAP_PX
        val segmentWidth = (width.toFloat() - gap * (SEGMENT_COUNT - 1)) / SEGMENT_COUNT
        val top = meterHeight * 0.10f
        val bottom = meterHeight * 0.80f
        // Centre of the gap between the last optimal and first redline segment.
        val markerX = OPT_SEGMENTS * (segmentWidth + gap) - gap / 2f
        val half = TARGET_MARKER_WIDTH_PX / 2f
        segmentRect.set(markerX - half, top, markerX + half, bottom)
        canvas.drawRoundRect(segmentRect, half, half, targetMarkerPaint)
    }

    private fun drawGearLabel(canvas: Canvas) {
        val textSize = height * 0.38f
        val baseline = height * 0.56f
        textStrokePaint.textSize = textSize
        textPaint.textSize = textSize
        textPaint.alpha = if (gear > 0) 255 else 135
        canvas.drawText(gearLabel, width / 2f, baseline, textStrokePaint)
        canvas.drawText(gearLabel, width / 2f, baseline, textPaint)
    }

    private fun withAlpha(color: Int, alpha: Int): Int {
        return Color.argb(alpha, Color.red(color), Color.green(color), Color.blue(color))
    }

    private companion object {
        private const val CORNER_HIT_FRACTION = 0.75f   // upper-right quadrant beyond this
        private const val TRIPLE_TAP_WINDOW_MS = 600L    // max gap between demo-toggle taps

        private const val SEGMENT_COUNT = 24
        private const val LUG_SEGMENTS = 8
        private const val OPT_SEGMENTS = 16
        private const val SEGMENT_GAP_PX = 6f
        private const val CORNER_FRACTION = 0.22f          // segment corner rounding
        private const val NEEDLE_EDGE_WIDTH_PX = 7f        // bright fill-boundary cap
        private const val TARGET_MARKER_WIDTH_PX = 5f      // upshift-point tick

        private const val LUG_END = 0.33f
        private const val OPT_END = 0.66f
        private const val PULSE_START = 0.85f
        private const val LUG_MAX_ALPHA = 89
        private const val SHIFT_RAMP_MAX_ALPHA = 64
        private const val PULSE_MIN_ALPHA = 38
        private const val PULSE_MAX_ALPHA = 150            // stronger redline urgency
        private const val PULSE_HZ = 4.0
        private const val TWO_PI = 2.0 * PI

        private const val NEUTRAL_LABEL = "—"
        private val GEAR_LABELS = arrayOf(
            NEUTRAL_LABEL,
            "G1",
            "G2",
            "G3",
            "G4",
            "G5"
        )
    }
}
