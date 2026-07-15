/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.settings.device

import android.os.Bundle
import android.util.Log
import com.android.settingslib.widget.SettingsBasePreferenceFragment
import androidx.preference.Preference
import androidx.preference.PreferenceManager
import com.android.settingslib.widget.MainSwitchPreference
import com.android.settingslib.widget.SelectorWithWidgetPreference
import java.io.BufferedWriter
import java.io.FileWriter
import java.io.IOException

class TurboChargingFragment : SettingsBasePreferenceFragment() {

    private var turboEnabled = false

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        setPreferencesFromResource(R.xml.charging_panel, rootKey)

        val sharedPrefs = PreferenceManager.getDefaultSharedPreferences(requireContext())
        turboEnabled = sharedPrefs.getBoolean("turbo_enable", true)

        val prefSlow = findPreference<SelectorWithWidgetPreference>("turbo_current_slow")
        val prefMedium = findPreference<SelectorWithWidgetPreference>("turbo_current_medium")
        val prefMax = findPreference<SelectorWithWidgetPreference>("turbo_current_max")

        fun updateSelectedPreference(value: String) {
            prefSlow?.isChecked = value == "4000000"
            prefMedium?.isChecked = value == "6000000"
            prefMax?.isChecked = value == "6800000"
        }

        val currentVal = sharedPrefs.getString("turbo_current", "6800000") ?: "6800000"
        updateSelectedPreference(currentVal)

        val listener = Preference.OnPreferenceClickListener { preference ->
            val newValue = when (preference.key) {
                "turbo_current_slow" -> "4000000"
                "turbo_current_medium" -> "6000000"
                "turbo_current_max" -> "6800000"
                else -> "6800000"
            }
            sharedPrefs.edit().putString("turbo_current", newValue).apply()
            updateSelectedPreference(newValue)
            updateChargeCurrent()
            true
        }

        prefSlow?.onPreferenceClickListener = listener
        prefMedium?.onPreferenceClickListener = listener
        prefMax?.onPreferenceClickListener = listener

        val switchPreference = findPreference<MainSwitchPreference>("turbo_enable")
        switchPreference?.setOnPreferenceChangeListener { _, newValue ->
            val enabled = newValue as Boolean
            sharedPrefs.edit().putBoolean("turbo_enable", enabled).apply()
            updateChargeCurrent()
            true
        }
    }

    private fun updateChargeCurrent() {
        turboEnabled = PreferenceManager.getDefaultSharedPreferences(requireContext()).getBoolean("turbo_enable", true)
        Log.i(TAG, "isTurbo=$turboEnabled")
        val defaultValue = "3000000"
        val currentValue = if (turboEnabled) {
            PreferenceManager.getDefaultSharedPreferences(requireContext()).getString("turbo_current", "6800000") ?: "6800000"
        } else {
            defaultValue
        }
        Log.i(TAG, "currentValue=$currentValue")
        writeChargeCurrent(currentValue)
    }

    private fun writeChargeCurrent(value: String) {
        try {
            value.toInt()
            BufferedWriter(FileWriter(CHARGE_CURRENT_FILE)).use { writer ->
                writer.write(value)
                Log.i(TAG, "Updated Charging current to $value")
            }
        } catch (e: NumberFormatException) {
            Log.e(TAG, "Invalid charge current value: $value", e)
        } catch (e: IOException) {
            Log.e(TAG, "Failed to update charge current", e)
        }
    }

    companion object {
        private const val TAG = "TurboChargingFragment"
        private const val CHARGE_CURRENT_FILE = "/sys/devices/platform/charger/power_supply/mtk-master-charger/constant_charge_current_max"
    }
}
