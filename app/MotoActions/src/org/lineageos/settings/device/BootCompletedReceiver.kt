/*
 * SPDX-FileCopyrightText: 2015 The CyanogenMod Project
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.settings.device

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.SystemProperties
import android.os.UserHandle
import android.util.Log

class BootCompletedReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        Log.i(TAG, "Booting")
        context.startServiceAsUser(
            Intent(context, MotoActionsService::class.java),
            UserHandle.CURRENT,
        )
        felicaDisabler(context)
    }

    private fun felicaDisabler(context: Context) {
        val sku = SystemProperties.get("ro.boot.hardware.sku", "")
        val isJapaneseVariant = sku == "XT2307-3"
        val flag = if (isJapaneseVariant) {
            PackageManager.COMPONENT_ENABLED_STATE_ENABLED
        } else {
            PackageManager.COMPONENT_ENABLED_STATE_DISABLED
        }
        try {
            context.packageManager.setApplicationEnabledSetting("com.felicanetworks.mfc", flag, 0)
        } catch (e: IllegalArgumentException) {
            Log.e(TAG, "Failed to set Felica enabled state", e)
        }
    }

    companion object {
        private const val TAG = "BootCompletedReceiver"
    }
}
