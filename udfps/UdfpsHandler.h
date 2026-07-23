/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <hardware/fingerprint.h>

class UdfpsHandler {
  public:
    virtual ~UdfpsHandler() = default;

    virtual void init(fingerprint_device_t* device) = 0;
    virtual void onFingerDown(uint32_t x, uint32_t y, float minor, float major) = 0;
    virtual void onFingerUp() = 0;
    virtual void onAcquired(int32_t result, int32_t vendorCode) = 0;
    virtual void cancel() = 0;
    virtual void preEnroll() = 0;
    virtual void enroll() = 0;
    virtual void postEnroll() = 0;

    // NEW: Called when enrollment progress is reported
    virtual void onEnrollmentProgress(int32_t enrollmentId, int32_t remaining) = 0;

    // Optional callbacks
    virtual void onAuthenticationSucceeded() {}
    virtual void onAuthenticationFailed() {}
};

struct UdfpsHandlerFactory {
    UdfpsHandler* (*create)();
    void (*destroy)(UdfpsHandler*);
};
