package dev.alfieprojects.gearsync

import android.content.Context
import org.json.JSONObject

/**
 * Loaded once at startup from assets/vehicle_config.json.
 * Computes theoretical k_g seed values (Hz per m/s) from transmission specs.
 */
data class VehicleConfig(
    val vehicle: String,
    /** Theoretical k_g = ratio_g * finalDrive * firingFactor / circumference, descending (gear 1 first). */
    val kSeeds: FloatArray,
    val toleranceLow: Float,
    val toleranceHigh: Float,
    val steadyStateWindowSeconds: Int,
    val speedJitterThresholdMps: Float,
    val useVibrationFusion: Boolean
) {
    companion object {
        private const val ASSET = "vehicle_config.json"

        fun load(context: Context): VehicleConfig {
            val json = JSONObject(
                context.assets.open(ASSET).bufferedReader().use { it.readText() }
            )

            val vehicle     = json.getString("vehicle")
            val engine      = json.getJSONObject("engine")
            val transmission = json.getJSONObject("transmission")
            val tire        = json.getJSONObject("tire")
            val calibration = json.getJSONObject("calibration")

            val cylinders    = engine.getInt("cylinders")
            val strokeCycle  = engine.getInt("strokeCycle")
            // Firing events per crankshaft revolution for a 4-stroke engine.
            val firingFactor = cylinders.toFloat() / (strokeCycle.toFloat() / 2f)

            val gearCount    = transmission.getInt("gears")
            val ratiosArr    = transmission.getJSONArray("ratios")
            val finalDrive   = transmission.getDouble("finalDriveRatio").toFloat()
            val circumference = tire.getDouble("nominalCircumferenceMeters").toFloat()

            // k_g = transmissionRatio_g * finalDrive * firingFactor / circumference
            // Stored descending (gear 1 = index 0 = highest ratio) to match CalibrationEngine.
            val kSeeds = FloatArray(gearCount) { g ->
                (ratiosArr.getDouble(g).toFloat() * finalDrive * firingFactor) / circumference
            }

            return VehicleConfig(
                vehicle               = vehicle,
                kSeeds                = kSeeds,
                toleranceLow          = calibration.getDouble("ratioToleranceLow").toFloat(),
                toleranceHigh         = calibration.getDouble("ratioToleranceHigh").toFloat(),
                steadyStateWindowSeconds = calibration.getInt("steadyStateWindowSeconds"),
                speedJitterThresholdMps  = calibration.getDouble("speedJitterThresholdMps").toFloat(),
                useVibrationFusion       = calibration.optBoolean("useVibrationFusion", false)
            )
        }
    }
}
