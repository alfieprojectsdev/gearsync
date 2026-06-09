# Technical Specification: VU Meter UI Redesign & Peripheral Alerts

## 1. Objective

Refactor `VUMeterView.kt` from a radial/needle dial into a high-visibility, horizontal multi-segmented bar graph resembling a classic analog hardware VU meter. Enhance driver ergonomics by leveraging peripheral vision: the entire view background must dynamically alpha-blend to alert colors when the vehicle drops into the lugging zone (0–33%) or enters the optimal shift window (66–100%).

## 2. Architectural Impact & JNI Alignment

The native JNI payload from C++ (`getVUMeterState(): FloatArray`) remains structurally unchanged to preserve the lock-free real-time DSP pipeline. `VUMeterView` ingests the core 5-float state, with an optional 6th `shiftDetected` flag (`1.0` = shift event pending, cleared on read) when present:


$$\text{state} = [ \text{needlePos}, \text{dominantHz}, \text{speedMps}, \text{gear}, \text{confidence}, (\text{optional } \text{shiftDetected}) ]$$

* **`needlePos` ($[0.0, 1.0]$):** Re-mapped from a radial angle to a horizontal fill percentage across the segmented bars.
* **`gear` ($0-5$):** Rendered as a massive, high-contrast digital glyph overlaid on the center of the VU meter for instantaneous glanceability.

---

## 3. Visual Layout Specification

### A. The Segmented Bar Graph (The Analog VU Feel)

Instead of a continuous line or a sweep needle, the meter will draw **24 distinct vertical rectangular segments** arranged horizontally.

* **Segments 1–8 (0% to 33%):** Lugging Zone $\rightarrow$ Deep Blue.
* **Segments 9–16 (33% to 66%):** Optimal Zone $\rightarrow$ Vibrant Emerald Green.
* **Segments 17–24 (66% to 100%):** Shift/Redline Zone $\rightarrow$ High-Intensity Amber/Red.

```text
+-------------------------------------------------------------+
|  [ GEAR 2 ]                                                 |
|  ||||||||        ||||||||        ||||||||                   |
|  [Lug: Blue]    [Opt: Green]    [Shift: Pulsing Red!]       |
+-------------------------------------------------------------+
   ^ Active        ^ Active        ^ Active & Flashing

```

### B. Peripheral Background Wash & Pulse Mechanics

To ensure "eyes-on-the-road" feedback, the view background (`Canvas.drawColor()`) will dynamically calculate its background color and opacity based on `needlePos` every Choreographer frame (60 FPS):

* **`needlePos` in $[0.33, 0.66]$ (Green Zone):** Background remains completely transparent or dark neutral (`#121212`). Zero peripheral distraction.
* **`needlePos` in $[0.0, 0.33)$ (Lugging / Downshift):** Background washes with a solid Blue color (`#0022FF`). The alpha opacity linearly scales from `0%` (at $0.33$) up to a maximum of `35%` (at $0.0$).
* **`needlePos` in $(0.66, 1.0]$ (Upshift Zone):** Background washes with a solid Red color (`#FF0000`).
* **The Pulse Trigger:** To prevent sensory acclimation, if `needlePos` exceeds `0.85`, the background alpha must modulate over time using a sine wave driven by the system clock:

$$\alpha(t) = \alpha_{\text{base}} + \alpha_{\text{amplitude}} \cdot \sin(2\pi \cdot f_{\text{pulse}} \cdot t)$$


* Where $f_{\text{pulse}} = 4.0\text{ Hz}$ (four bright flashes per second) cycling between `15%` and `50%` opacity.



---

## 4. Codebase Modification Plan

### Step 1: Update `colors.xml`

Add the high-saturation color palette required for both the segments and the peripheral ambient masks.

```xml
<resources>
    <!-- Segment States -->
    <color name="vu_lug_segment">#00D4FF</color>
    <color name="vu_opt_segment">#00E676</color>
    <color name="vu_shift_segment">#FF1744</color>
    <color name="vu_inactive_segment">#2A2A2A</color>
    
    <!-- Ambient Flash Colors -->
    <color name="vu_ambient_lug_base">#0022FF</color>
    <color name="vu_ambient_shift_base">#FF0000</color>
</resources>

```

### Step 2: Refactor `VUMeterView.kt`

Modify the `onDraw(canvas: Canvas)` method inside `VUMeterView.kt`. Replace the radial needle calculations with a linear layout loop.

```kotlin
// Pseudocode specification for Claude Code implementation

override fun onDraw(canvas: Canvas) {
    super.onDraw(canvas)
    if (!isRunning) return

    val state = NativeEngine.getVUMeterState()
    val needlePos = state[0].coerceIn(0f, 1f)
    val gear = state[3].toInt()

    // 1. Calculate and Draw Peripheral Background Wash
    drawPeripheralWash(canvas, needlePos)

    // 2. Draw Segmented VU Bars
    val totalSegments = 24
    val padding = 6f // pixels between bars
    val availableWidth = width - (padding * (totalSegments - 1))
    val segmentWidth = availableWidth / totalSegments

    for (i in 0 until totalSegments) {
        val segmentThreshold = i.toFloat() / totalSegments.toFloat()
        val isActive = needlePos >= segmentThreshold

        // Determine segment color family based on its position index
        val colorRes = when {
            !isActive -> R.color.vu_inactive_segment
            i < 8 -> R.color.vu_lug_segment       // 0-33%
            i < 16 -> R.color.vu_opt_segment      // 33-66%
            else -> R.color.vu_shift_segment       // 66-100%
        }
        
        segmentPaint.color = context.getColor(colorRes)
        
        val left = i * (segmentWidth + padding)
        val right = left + segmentWidth
        canvas.drawRect(left, 0f, right, height.toFloat() * 0.7f, segmentPaint)
    }

    // 3. Draw Large Center HUD Gear Text (Glanceable)
    if (gear > 0) {
        textPaint.color = Color.WHITE
        textPaint.textSize = height * 0.25f // Massive scaling
        canvas.drawText("G$gear", width / 2f, height * 0.95f, textPaint)
    } else {
        textPaint.color = Color.GRAY
        canvas.drawText("N/??", width / 2f, height * 0.95f, textPaint)
    }
}

private fun drawPeripheralWash(canvas: Canvas, needlePos: Float) {
    val currentTimeMs = System.currentTimeMillis()
    
    if (needlePos < 0.33f) {
        // Lugging Warning: Linear ramp up opacity as it gets closer to stalling
        val intensity = (0.33f - needlePos) / 0.33f
        val alpha = (intensity * 0.35f * 255).toInt()
        canvas.drawColor(ColorUtils.setAlphaComponent(context.getColor(R.color.vu_ambient_lug_base), alpha))
    } else if (needlePos > 0.66f) {
        // Shift Warning: 4Hz Pulse if deep in the redline (>85%)
        val alphaTarget = if (needlePos > 0.85f) {
            val pulse = (sin(currentTimeMs / 1000.0 * 2.0 * Math.PI * 4.0) + 1.0) / 2.0 // 0.0 to 1.0
            (0.15f + (pulse * 0.35f)) // Oscillate between 15% and 50% opacity
        } else {
            ((needlePos - 0.66f) / 0.19f) * 0.25f // Linear ramp up to 25% opacity
        }
        val alpha = (alphaTarget * 255).toInt().coerceIn(0, 255)
        canvas.drawColor(ColorUtils.setAlphaComponent(context.getColor(R.color.vu_ambient_shift_base), alpha))
    }
}

```

## 5. Verification Criteria for Claude Code Execution

1. **Zero Allocations in Render Path:** Ensure `Paint` objects, colors, and math helpers are instantiated in `init{}` or overridden lifecycle states, never inside `onDraw` to maintain the **Zero-GC jitter guarantee**.
2. **Invalidation Loop:** Verify `Choreographer` or `postInvalidateOnAnimation()` runs smoothly without locking up the UI thread when rendering ambient alpha blending.
3. **Boundary Safety:** The background must return to `Color.TRANSPARENT` or its baseline theme value instantly the moment `needlePos` crosses back into the `0.33 - 0.66` safety buffer zone.