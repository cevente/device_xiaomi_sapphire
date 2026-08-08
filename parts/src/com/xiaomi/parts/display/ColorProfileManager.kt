package com.xiaomi.parts.display

import android.util.Log

class ColorProfileManager {
    companion object {
        private const val TAG = "ColorProfileManager"

        init {
            try {
                System.loadLibrary("xiaomiparts_jni")
            } catch (e: UnsatisfiedLinkError) {
                Log.e(TAG, "Failed to load xiaomiparts_jni library", e)
            }
        }
    }

    private external fun nativeSetCrcMode(mode: Int): Boolean

    fun setProfile(mode: Int): Boolean {
        return nativeSetCrcMode(mode)
    }
}
