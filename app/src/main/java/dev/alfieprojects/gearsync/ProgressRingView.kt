package dev.alfieprojects.gearsync

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.View

// Circular progress ring for guided calibration.
//
// Top-level View (not an inner class) so LayoutInflater can construct it from
// activity_calibration.xml via the (Context, AttributeSet) constructor.
// Fill colour steps through the zone palette (lug → optimal → redline) as
// progress increases; the track is drawn underneath. (ref: DL-004)
class ProgressRingView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    private var progress = 0f

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 16f
    }
    private val oval = RectF()

    /** Set progress in [0, 1]; clamps and triggers a redraw. */
    fun setProgress(p: Float) {
        progress = p.coerceIn(0f, 1f)
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        val r = (minOf(width, height) / 2f) - paint.strokeWidth
        oval.set(width / 2f - r, height / 2f - r, width / 2f + r, height / 2f + r)

        paint.color = resources.getColor(R.color.calib_ring_track, context.theme)
        canvas.drawArc(oval, 0f, 360f, false, paint)

        paint.color = when {
            progress < 0.33f -> resources.getColor(R.color.zone_lug, context.theme)
            progress < 0.66f -> resources.getColor(R.color.zone_optimal, context.theme)
            else             -> resources.getColor(R.color.zone_redline, context.theme)
        }
        canvas.drawArc(oval, -90f, progress * 360f, false, paint)
    }
}
