package org.lineageos.settings;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.util.Log;
import androidx.preference.PreferenceManager;

import org.lineageos.settings.display.KcalUtils;
import org.lineageos.settings.utils.HapticUtils;
import org.lineageos.settings.refreshrate.RefreshUtils;
import org.lineageos.settings.thermal.ThermalUtils;
import com.xiaomi.parts.display.ColorProfileManager;

public class BootCompletedReceiver extends BroadcastReceiver {

    private static final boolean DEBUG = false;
    private static final String TAG = "XiaomiParts";

    @Override
    public void onReceive(final Context context, Intent intent) {
        if (!intent.getAction().equals(Intent.ACTION_BOOT_COMPLETED)) {
            return;
        }

        if (DEBUG) Log.d(TAG, "Received boot completed intent");

        SharedPreferences sharedPrefs = PreferenceManager.getDefaultSharedPreferences(context);

        // Legacy Restorations (if applicable to your build)
        if (KcalUtils.isKcalSupported()) KcalUtils.writeCurrentSettings(sharedPrefs);
        HapticUtils.restoreLevel(context);
        RefreshUtils.startService(context);
        ThermalUtils.startService(context);

        // Hardware Color Profile Restoration
        int savedCrcMode = sharedPrefs.getInt("hardware_crc_mode", 0); // Default to CRC_OFF (0)
        boolean crcSuccess = new ColorProfileManager().setProfile(savedCrcMode);
        
        if (DEBUG) Log.d(TAG, "Restored CRC Mode: " + savedCrcMode + " | Success: " + crcSuccess);
    }
}
