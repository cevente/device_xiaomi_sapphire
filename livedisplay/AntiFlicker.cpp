/*
 * Copyright (C) 2020 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "AntiFlickerService"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "AntiFlicker.h"
#include "display/drm/mi_disp.h"

namespace aidl {
namespace vendor {
namespace lineage {
namespace livedisplay {

static constexpr const char* kDispFeaturePath = "/dev/mi_display/disp_feature";

ndk::ScopedAStatus AntiFlicker::getEnabled(bool* aidl_return) {
    *aidl_return = mEnabled;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus AntiFlicker::setEnabled(bool enabled) {
    if (enabled == mEnabled) {
        return ndk::ScopedAStatus::ok();
    }

    int fd = open(kDispFeaturePath, O_RDWR);
    if (fd < 0) {
        LOG(ERROR) << "Failed to open " << kDispFeaturePath;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    struct disp_feature_req req;
    memset(&req, 0, sizeof(req));
    req.base.disp_id = MI_DISP_PRIMARY;
    req.feature_id = DISP_FEATURE_DC;
    req.feature_val = enabled ? FEATURE_ON : FEATURE_OFF;

    if (ioctl(fd, MI_DISP_IOCTL_SET_FEATURE, &req) < 0) {
        LOG(ERROR) << "IOCTL MI_DISP_IOCTL_SET_FEATURE failed for DC";
        close(fd);
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    close(fd);
    mEnabled = enabled;
    return ndk::ScopedAStatus::ok();
}

}  // namespace livedisplay
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
