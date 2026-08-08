/*
 * SPDX-FileCopyrightText: 2019-2025 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "SunlightEnhancementService"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

#include "SunlightEnhancement.h"

namespace aidl {
namespace vendor {
namespace lineage {
namespace livedisplay {

static constexpr const char* kDispCommandPath = 
        "/proc/mi_display/tx_cmd_set_prim";
static constexpr const char* kBrightnessPath = 
        "/sys/class/backlight/panel0-backlight/brightness";

ndk::ScopedAStatus SunlightEnhancement::getEnabled(bool* _aidl_return) {
    // Return the cached state instead of reading the file
    *_aidl_return = mEnabled;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus SunlightEnhancement::setEnabled(bool enabled) {
    // Prevent redundant writes if the state isn't changing
    if (enabled == mEnabled) {
        return ndk::ScopedAStatus::ok();
    }

    if (enabled) {
        // Read and store current brightness before turning on HBM
        std::string buf;
        if (android::base::ReadFileToString(kBrightnessPath, &buf)) {
            mStoredBrightness = android::base::Trim(buf);
        } else {
            LOG(ERROR) << "Failed to read current brightness from " << kBrightnessPath;
        }

        // Engage HBM
        if (!android::base::WriteStringToFile("28\n", kDispCommandPath)) {
            LOG(ERROR) << "Failed to write 28 to " << kDispCommandPath;
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }
    } else {
        // Disengage HBM
        if (!android::base::WriteStringToFile("34\n", kDispCommandPath)) {
            LOG(ERROR) << "Failed to write 34 to " << kDispCommandPath;
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }

        // Restore the previous brightness if we successfully stored one
        if (!mStoredBrightness.empty()) {
            if (!android::base::WriteStringToFile(mStoredBrightness + "\n", kBrightnessPath)) {
                LOG(ERROR) << "Failed to restore brightness to " << kBrightnessPath;
            }
            // Clear the cache after restoring
            mStoredBrightness.clear();
        }
    }

    // Update the cache if the write was successful
    mEnabled = enabled;
    return ndk::ScopedAStatus::ok();
}

}  // namespace livedisplay
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
