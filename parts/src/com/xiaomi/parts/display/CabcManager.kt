package com.xiaomi.parts.display

import android.util.Log

class CabcManager {
    companion object {
        private const val TAG = "CabcManager"

        init {
            try {
                System.loadLibrary("xiaomiparts_jni")
            } catch (e: UnsatisfiedLinkError) {
                Log.e(TAG, "Failed to load xiaomiparts_jni library", e)
            }
        }
    }

    private external fun nativeSetCabcMode(mode: Int): Boolean

    fun setCabc(mode: Int): Boolean {
        return nativeSetCabcMode(mode)
    }
}
