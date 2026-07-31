#pragma once

#include "UdfpsHandler.h"
#include <android-base/unique_fd.h>
#include <atomic>
#include <mutex>
#include <thread>

class GoodixUdfpsHandler : public UdfpsHandler {
  public:
    GoodixUdfpsHandler();
    ~GoodixUdfpsHandler() override;

    void init(fingerprint_device_t* device) override;
    void onFingerDown(uint32_t x, uint32_t y, float minor, float major) override;
    void onFingerUp() override;
    void onAcquired(int32_t result, int32_t vendorCode) override;
    void cancel() override;
    void preEnroll() override;
    void enroll() override;
    void postEnroll() override;

  private:
    fingerprint_device_t* mDevice;
    android::base::unique_fd touch_fd_;
    android::base::unique_fd disp_fd_;
    std::atomic<bool> enrolling{false};
    std::atomic<bool> isRunning{true};
    std::atomic<uint64_t> mFbDownTimeMs{0};

    std::mutex touch_mutex_;
    std::mutex disp_mutex_;
    std::mutex device_mutex_;

    std::thread fodThread_;
    std::thread dispThread_;

    int getBrightness();
    void shutdownThreads();
    void fodPressMonitorThread();
    void displayEventMonitorThread();
    void setFodStatus(int value);
    void setFingerDown(bool pressed);
};
