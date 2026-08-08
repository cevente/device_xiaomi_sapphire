/*
 * SPDX-FileCopyrightText: 2019-2025 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "SunlightEnhancementService"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "SunlightEnhancement.h"
#include "mi_disp.h"

namespace aidl {
namespace vendor {
namespace lineage {
namespace livedisplay {

static constexpr const char* kDispFeaturePath = "/dev/mi_display/disp_feature";
static constexpr const char* kBrightnessPath = "/sys/class/backlight/panel0-backlight/brightness";

ndk::ScopedAStatus SunlightEnhancement::getEnabled(bool* _aidl_return) {
    *_aidl_return = mEnabled;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus SunlightEnhancement::setEnabled(bool enabled) {
    if (enabled == mEnabled) {
        return ndk::ScopedAStatus::ok();
    }

    if (enabled) {
        // Read and store current brightness
        std::string buf;
        if (android::base::ReadFileToString(kBrightnessPath, &buf)) {
            mStoredBrightness = android::base::Trim(buf);
        } else {
            LOG(ERROR) << "Failed to read current brightness from " << kBrightnessPath;
        }
    }

    // Apply HBM via IOCTL
    int fd = open(kDispFeaturePath, O_RDWR);
    if (fd < 0) {
        LOG(ERROR) << "Failed to open " << kDispFeaturePath;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    struct disp_feature_req req;
    memset(&req, 0, sizeof(req));
    req.base.disp_id = MI_DISP_PRIMARY;
    req.feature_id = DISP_FEATURE_HBM;
    req.feature_val = enabled ? FEATURE_ON : FEATURE_OFF;

    if (ioctl(fd, MI_DISP_IOCTL_SET_FEATURE, &req) < 0) {
        LOG(ERROR) << "IOCTL MI_DISP_IOCTL_SET_FEATURE failed for HBM";
        close(fd);
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    close(fd);

    if (!enabled && !mStoredBrightness.empty()) {
        // Restore previous brightness
        if (!android::base::WriteStringToFile(mStoredBrightness + "\n", kBrightnessPath)) {
            LOG(ERROR) << "Failed to restore brightness to " << kBrightnessPath;
        }
        mStoredBrightness.clear();
    }

    mEnabled = enabled;
    return ndk::ScopedAStatus::ok();
}

}  // namespace livedisplay
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
