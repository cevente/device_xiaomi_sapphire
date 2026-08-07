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

ndk::ScopedAStatus SunlightEnhancement::getEnabled(bool* _aidl_return) {
    // Return the cached state instead of reading the file
    *_aidl_return = mEnabled;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus SunlightEnhancement::setEnabled(bool enabled) {
    // Note the added \n
    if (!android::base::WriteStringToFile((enabled ? "28\n" : "34\n"), kDispCommandPath)) {
        LOG(ERROR) << "Failed to write " << kDispCommandPath;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    // Update the cache if the write was successful
    mEnabled = enabled;
    return ndk::ScopedAStatus::ok();
}

}  // namespace livedisplay
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
