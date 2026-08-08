package org.lineageos.settings;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.util.Log;
import androidx.preference.PreferenceManager;

import com.xiaomi.parts.display.CabcManager;

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

        // Restore CABC Mode (Default to 0 / Off)
        int savedCabcMode = sharedPrefs.getInt("lcd_cabc_mode", 0); 
        boolean cabcSuccess = new CabcManager().setCabc(savedCabcMode);
        
        if (DEBUG) Log.d(TAG, "Restored CABC Mode: " + savedCabcMode + " | Success: " + cabcSuccess);
    }
}
