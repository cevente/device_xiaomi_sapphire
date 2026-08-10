/*
 * Copyright (C) 2026 Cedric Loste
 *
 * Based on original LineageOS LiveDisplay codes
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "SunlightEnhancementService"

#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

#include "SunlightEnhancement.h"
#include "mi_disp.h"

namespace aidl {
namespace vendor {
namespace lineage {
namespace livedisplay {

static constexpr const char* kDispFeaturePath = "/dev/mi_display/disp_feature";
static constexpr const char* kBrightnessPath = "/sys/class/backlight/panel0-backlight/brightness";
static constexpr const char* kScreenStatePath = "/sys/class/thermal/thermal_message/screen_state";

SunlightEnhancement::SunlightEnhancement() {
    mMonitorThread = std::thread(&SunlightEnhancement::monitorScreenState, this);
}

SunlightEnhancement::~SunlightEnhancement() {
    mStopThread = true;
    if (mMonitorThread.joinable()) {
        mMonitorThread.join();
    }
}

ndk::ScopedAStatus SunlightEnhancement::getEnabled(bool* _aidl_return) {
    *_aidl_return = mEnabled;
    return ndk::ScopedAStatus::ok();
}

uint32_t SunlightEnhancement::getBrightness() {
    int fd = open(kDispFeaturePath, O_RDWR);
    if (fd >= 0) {
        struct disp_brightness_req req;
        memset(&req, 0, sizeof(req));
        req.base.disp_id = MI_DISP_PRIMARY;

        if (ioctl(fd, MI_DISP_IOCTL_GET_BRIGHTNESS, &req) == 0 && req.brightness > 0) {
            close(fd);
            return req.brightness;
        }
        close(fd);
    }

    // Fallback to sysfs reading if IOCTL fails
    std::string buf;
    if (android::base::ReadFileToString(kBrightnessPath, &buf)) {
        return static_cast<uint32_t>(std::strtoul(android::base::Trim(buf).c_str(), nullptr, 10));
    }

    LOG(ERROR) << "Failed to read brightness via IOCTL and sysfs";
    return 0;
}

void SunlightEnhancement::setBrightness(uint32_t level) {
    if (level == 0) return;

    int fd = open(kDispFeaturePath, O_RDWR);
    if (fd >= 0) {
        struct disp_brightness_req req;
        memset(&req, 0, sizeof(req));
        req.base.disp_id = MI_DISP_PRIMARY;
        req.brightness = level;

        if (ioctl(fd, MI_DISP_IOCTL_SET_BRIGHTNESS, &req) == 0) {
            close(fd);
            return;
        }
        close(fd);
    }

    // Fallback to sysfs writing if IOCTL fails
    std::string levelStr = std::to_string(level) + "\n";
    if (!android::base::WriteStringToFile(levelStr, kBrightnessPath)) {
        LOG(ERROR) << "Failed to write target brightness to " << kBrightnessPath;
    }
}

void SunlightEnhancement::applyHbm(bool enabled) {
    int fd = open(kDispFeaturePath, O_RDWR);
    if (fd < 0) {
        LOG(ERROR) << "Failed to open " << kDispFeaturePath;
        return;
    }

    struct disp_feature_req req;
    
    // 1. Toggle Standard Global HBM using nested base struct layout
    memset(&req, 0, sizeof(req));
    req.base.flag = MI_DISP_FLAG_BLOCK;
    req.base.disp_id = MI_DISP_PRIMARY;
    req.feature_id = DISP_FEATURE_HBM;
    req.feature_val = enabled ? 1 : 0;
    req.tx_len = 0;
    req.tx_ptr = 0;
    req.rx_len = 0;
    req.rx_ptr = 0;

    if (ioctl(fd, MI_DISP_IOCTL_SET_FEATURE, &req) < 0) {
        LOG(ERROR) << "IOCTL MI_DISP_IOCTL_SET_FEATURE failed for standard HBM";
    }

    // 2. Toggle Stepped HBM (LCD_HBM) level for aggressive panel sunlight boost (Level 3)
    memset(&req, 0, sizeof(req));
    req.base.flag = MI_DISP_FLAG_BLOCK;
    req.base.disp_id = MI_DISP_PRIMARY;
    req.feature_id = DISP_FEATURE_LCD_HBM;
    req.feature_val = enabled ? LCD_HBM_L3_ON : LCD_HBM_OFF;
    req.tx_len = 0;
    req.tx_ptr = 0;
    req.rx_len = 0;
    req.rx_ptr = 0;

    if (ioctl(fd, MI_DISP_IOCTL_SET_FEATURE, &req) < 0) {
        LOG(DEBUG) << "IOCTL MI_DISP_IOCTL_SET_FEATURE for LCD_HBM failed or unhandled";
    }

    close(fd);
}

void SunlightEnhancement::monitorScreenState() {
    int lastState = -1;

    while (!mStopThread) {
        std::string buf;
        if (android::base::ReadFileToString(kScreenStatePath, &buf)) {
            buf = android::base::Trim(buf);
            int currentState = (buf == "1") ? 1 : 0;

            // Re-enforce HBM when screen transitions from OFF (0) to ON (1)
            if (lastState == 0 && currentState == 1 && mEnabled) {
                LOG(INFO) << "Screen turned on, re-enforcing stepped HBM";
                applyHbm(true);
            }
            lastState = currentState;
        }

        // Track live brightness changes while HBM is active
        if (mEnabled) {
            uint32_t currentBrightness = getBrightness();
            if (currentBrightness > 0) {
                if (!mHbmActive) {
                    // Sample HBM baseline level set by OS framework on trigger
                    mLastBrightness = currentBrightness;
                    mHbmActive = true;
                } else {
                    // Update user adjustment if slider moved during HBM
                    uint32_t last = mLastBrightness.load();
                    if (currentBrightness != last) {
                        mUserSetBrightness = currentBrightness;
                        mLastBrightness = currentBrightness;
                    }
                }
            }
        } else {
            mHbmActive = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

ndk::ScopedAStatus SunlightEnhancement::setEnabled(bool enabled) {
    if (enabled == mEnabled) {
        return ndk::ScopedAStatus::ok();
    }

    if (enabled) {
        mStoredBrightness = getBrightness();
        mUserSetBrightness = 0;
        mHbmActive = false;
    }

    mEnabled = enabled;

    // Apply Stepped HBM and global features via IOCTL
    applyHbm(enabled);

    if (!enabled && mStoredBrightness > 0) {
        // Final poll in case slider was adjusted immediately prior to disable call
        uint32_t currentBrightness = getBrightness();
        uint32_t last = mLastBrightness.load();
        if (mHbmActive && currentBrightness > 0 && currentBrightness != last) {
            mUserSetBrightness = currentBrightness;
        }

        uint32_t userSet = mUserSetBrightness.load();
        uint32_t stored = mStoredBrightness.load();
        uint32_t targetBrightness = (userSet > 0) ? userSet : stored;

        setBrightness(targetBrightness);

        // Reset tracked states
        mStoredBrightness = 0;
        mUserSetBrightness = 0;
        mHbmActive = false;
    }

    return ndk::ScopedAStatus::ok();
}

}  // namespace livedisplay
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
