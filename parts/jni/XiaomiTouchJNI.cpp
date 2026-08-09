#include <jni.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <android/log.h>
#include <string.h>
#include "xiaomi_touch.h"

#define LOG_TAG "XiaomiTouch-JNI"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define TOUCH_DEV_PATH "/dev/xiaomi-touch"

extern "C" JNIEXPORT jboolean JNICALL
Java_com_xiaomi_parts_touch_TouchManager_nativeSetTouchValue(JNIEnv* /*env*/, jobject /*thiz*/, jint mode, jint value) {
    int fd = open(TOUCH_DEV_PATH, O_RDWR);
    if (fd < 0) {
        LOGE("Failed to open %s", TOUCH_DEV_PATH);
        return JNI_FALSE;
    }

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%d %d %d", mode, SET_CUR_VALUE, value);
    int written = write(fd, buf, len);
    close(fd);

    if (written < 0) {
        LOGE("Failed to write touch mode %d with value %d", mode, value);
        return JNI_FALSE;
    }

    return JNI_TRUE;
}
