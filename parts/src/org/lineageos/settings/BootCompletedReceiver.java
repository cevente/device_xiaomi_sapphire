package org.lineageos.settings;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import androidx.preference.PreferenceManager;
import com.xiaomi.parts.touch.TouchManager;

public class BootCompletedReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(final Context context, Intent intent) {
        if (!intent.getAction().equals(Intent.ACTION_BOOT_COMPLETED)) {
            return;
        }

        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        TouchManager manager = new TouchManager();

        manager.setTouchMode(TouchManager.TOUCH_DOUBLETAP_MODE, prefs.getBoolean("touch_dt2w", false));
        manager.setTouchMode(TouchManager.TOUCH_GAME_MODE, prefs.getBoolean("touch_game", false));
        manager.setTouchMode(TouchManager.TOUCH_EXPERT_MODE, prefs.getBoolean("touch_expert", false));
        manager.setTouchMode(TouchManager.TOUCH_RESIST_RF, prefs.getBoolean("touch_rf", false));
        manager.setTouchMode(TouchManager.TOUCH_GRIP_MODE, prefs.getBoolean("touch_grip", false));
    }
}
