package com.xiaomi.parts.touch

import android.util.Log

class TouchManager {
    companion object {
        private const val TAG = "TouchManager"

        // Mode type mappings from xiaomi_touch.h[span_2](start_span)[span_2](end_span)
        const val TOUCH_GAME_MODE = 0
        const val TOUCH_EXPERT_MODE = 6
        const val TOUCH_RESIST_RF = 12
        const val TOUCH_DOUBLETAP_MODE = 14
        const val TOUCH_GRIP_MODE = 15

        init {
            try {
                System.loadLibrary("libxiaomiparts_touch_jni")
            } catch (e: UnsatisfiedLinkError) {
                Log.e(TAG, "Failed to load touch jni library", e)
            }
        }
    }

    private external fun nativeSetTouchValue(mode: Int, value: Int): Boolean

    fun setTouchMode(mode: Int, enabled: Boolean): Boolean {
        return nativeSetTouchValue(mode, if (enabled) 1 else 0)
    }
}
