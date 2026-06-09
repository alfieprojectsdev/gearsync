package dev.alfieprojects.gearsync

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat

class MainActivity : AppCompatActivity() {

    private val requiredPermissions = buildList {
        add(Manifest.permission.RECORD_AUDIO)
        add(Manifest.permission.ACCESS_FINE_LOCATION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            add(Manifest.permission.POST_NOTIFICATIONS)
    }.toTypedArray()

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { grants ->
        val denied = grants.filterValues { !it }.keys
        if (denied.isEmpty()) startShiftService()
        else Toast.makeText(
            this,
            getString(R.string.permissions_required, denied.joinToString()),
            Toast.LENGTH_LONG
        ).show()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        findViewById<Button>(R.id.btnStart).setOnClickListener { checkAndStart() }
        findViewById<Button>(R.id.btnStop).setOnClickListener  { stopShiftService() }
        findViewById<Button>(R.id.btnCalibrate).setOnClickListener { openCalibration() }
    }

    private fun checkAndStart() {
        val missing = requiredPermissions.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) startShiftService()
        else permissionLauncher.launch(missing.toTypedArray())
    }

    private fun startShiftService() {
        val intent = Intent(this, ShiftAssistantService::class.java)
        ContextCompat.startForegroundService(this, intent)
        Toast.makeText(this, R.string.service_started, Toast.LENGTH_SHORT).show()
    }

    private fun stopShiftService() {
        stopService(Intent(this, ShiftAssistantService::class.java))
        Toast.makeText(this, R.string.service_stopped, Toast.LENGTH_SHORT).show()
    }

    // Guard against opening CalibrationActivity when the service is not running:
    // NativeEngine.beginGearCalibration would silently no-op and the UI would
    // show no progress. (ref: DL-004)
    private fun openCalibration() {
        if (!ShiftAssistantService.isRunning) {
            Toast.makeText(this, R.string.calib_service_not_running, Toast.LENGTH_LONG).show()
            return
        }
        startActivity(Intent(this, CalibrationActivity::class.java))
    }
}
