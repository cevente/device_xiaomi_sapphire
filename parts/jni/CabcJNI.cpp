#include <jni.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <android/log.h>
#include <string.h>
#include "mi_disp.h"

#define LOG_TAG "XiaomiParts-JNI"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define MI_DISPLAY_DEVICE "/dev/mi_display/disp_feature"

extern "C" JNIEXPORT jboolean JNICALL
Java_com_xiaomi_parts_display_CabcManager_nativeSetCabcMode(JNIEnv* env, jobject thiz, jint mode) {
    int fd = open(MI_DISPLAY_DEVICE, O_RDWR);
    if (fd < 0) {
        LOGE("Failed to open %s", MI_DISPLAY_DEVICE);
        return JNI_FALSE;
    }

    struct disp_feature_req req;
    memset(&req, 0, sizeof(req));
    req.base.disp_id = MI_DISP_PRIMARY; //[span_2](start_span)[span_2](end_span)
    req.feature_id = DISP_FEATURE_CABC; //[span_3](start_span)[span_3](end_span)
    req.feature_val = mode;

    int ret = ioctl(fd, MI_DISP_IOCTL_SET_FEATURE, &req); //[span_4](start_span)[span_4](end_span)
    close(fd);

    if (ret < 0) {
        LOGE("Failed to set CABC mode: %d", mode);
        return JNI_FALSE;
    }

    return JNI_TRUE;
}
