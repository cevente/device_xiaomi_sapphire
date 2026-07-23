/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// Include all headers needed for bindgen
#include "xiaomi_touch.h"
#include <display/drm/mi_disp.h>
#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>

// Define missing types if needed
#ifndef LOCAL_HBM_UI_READY
#define LOCAL_HBM_UI_READY 1
#endif
