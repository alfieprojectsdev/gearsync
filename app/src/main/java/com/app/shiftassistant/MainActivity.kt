package com.app.shiftassistant

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
        else Toast.makeText(this, "Permissions required: $denied", Toast.LENGTH_LONG).show()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        findViewById<Button>(R.id.btnStart).setOnClickListener { checkAndStart() }
        findViewById<Button>(R.id.btnStop).setOnClickListener  { stopShiftService() }
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
        Toast.makeText(this, "GearSync started", Toast.LENGTH_SHORT).show()
    }

    private fun stopShiftService() {
        stopService(Intent(this, ShiftAssistantService::class.java))
        Toast.makeText(this, "GearSync stopped", Toast.LENGTH_SHORT).show()
    }
}
