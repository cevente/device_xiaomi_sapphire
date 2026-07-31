#pragma once

#include "UdfpsHandler.h"
#include <android-base/unique_fd.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>

class FpcUdfpsHandler : public UdfpsHandler {
  public:
    FpcUdfpsHandler();
    ~FpcUdfpsHandler() override;

    void init(fingerprint_device_t* device) override;
    void onFingerDown(uint32_t x, uint32_t y, float minor, float major) override;
    void onFingerUp() override;
    void onError(int32_t error, int32_t vendorCode) override;
    void onAcquired(int32_t result, int32_t vendorCode) override;
    void onEnrollmentProgress(int32_t enrollmentId, int32_t remaining) override;
    void preEnroll() override;
    void enroll() override;
    void postEnroll() override;
    void cancel() override;

  private:
    fingerprint_device_t* mDevice;
    android::base::unique_fd touch_fd_;
    android::base::unique_fd disp_fd_;
    android::base::unique_fd drm_fd_;
    std::atomic<bool> enrolling{false};
    std::atomic<bool> isRunning{true};
    std::atomic<bool> mPendingCleanup{false};
    std::atomic<bool> mHbmStuck{false};
    std::atomic<bool> mIsFingerDown{false};
    std::atomic<bool> mAuthInProgress{false};
    std::atomic<bool> mIsScreenOnFod{false};
    std::atomic<int32_t> mSamplesRemaining{0};
    std::atomic<bool> mIsFinalEnrollment{false};
    std::atomic<bool> mHbmEnabled{false};
    std::atomic<bool> mFingerUpSent{false};
    std::atomic<int> mLastFodState{-1};

    std::mutex touch_mutex_;
    std::mutex disp_mutex_;
    std::mutex device_mutex_;
    std::mutex cleanup_mutex_;
    std::condition_variable cleanup_cv_;

    std::thread fodThread_;
    std::thread dispThread_;
    std::thread screenThread_;
    std::thread cleanupThread_;
    std::atomic<bool> cleanupThreadRunning{false};

    bool isFingerprintActive();
    void sendEarlyWakeupHint();
    void setDispFpStatus(int status);
    void enableHbm();
    void disableHbm();
    int getBrightness();
    bool isScreenOn();
    void scheduleHbmTimeout(bool isFinalEnrollment = false);
    void forceHbmCleanup(bool isFinalEnrollment = false);
    void directForceFodOff();
    void setFingerDown(bool pressed);
    void shutdownThreads();
    void screenStateMonitorThread();
    void setFodStatus(int value);
    void fodPressMonitorThread();
    void displayEventMonitorThread();
    const char* getFingerprintStatusName(int status);
};
