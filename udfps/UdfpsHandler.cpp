/*
 * Copyright (C) 2024 Cedric Loste
 *
 * Based on original LineageOS UdfpsHandler
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "UdfpsHandler.xiaomi_sm6225"

#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>

#include <poll.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <dirent.h>
#include <limits.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <condition_variable>

// Display and DRM Headers
#include "display/drm/mi_disp.h"
#include "display/drm/sde_drm.h"
#include "display/drm/msm_drm_pp.h"
#include "xiaomi_touch.h"
#include "UdfpsHandler.h"

#define COMMAND_NIT 10
#define PARAM_NIT_FOD 1
#define PARAM_NIT_NONE 0

#define COMMAND_FOD_PRESS_STATUS 1
#define PARAM_FOD_PRESSED 1
#define PARAM_FOD_RELEASED 0

#define FOD_STATUS_OFF 0
#define FOD_STATUS_ON 1

#define TOUCH_DEV_PATH "/dev/xiaomi-touch"
#define TOUCH_MAGIC 'T'
#define TOUCH_IOC_SET_CUR_VALUE _IO(TOUCH_MAGIC, SET_CUR_VALUE)
#define TOUCH_IOC_GET_CUR_VALUE _IO(TOUCH_MAGIC, GET_CUR_VALUE)

#define DISP_FEATURE_PATH "/dev/mi_display/disp_feature"
#define BRIGHTNESS_PATH "/sys/class/backlight/panel0-backlight/brightness"
#define DRM_DEV_PATH "/dev/dri/card0"

using ::aidl::android::hardware::biometrics::fingerprint::AcquiredInfo;

namespace {

static disp_event_resp* parseDispEvent(int fd) {
    thread_local char event_data[1024];
    ssize_t size;

    memset(event_data, 0, sizeof(event_data));
    size = read(fd, event_data, sizeof(event_data));
    if (size < 0) {
        return nullptr;
    }

    if (size < (ssize_t)sizeof(struct disp_event)) {
        return nullptr;
    }

    return reinterpret_cast<disp_event_resp*>(event_data);
}

static int open_ts_input() {
    int fd = -1;
    DIR *dir = opendir("/dev/input");

    if (dir != nullptr) {
        struct dirent *ent;

        while ((ent = readdir(dir)) != nullptr) {
            if (ent->d_type == DT_CHR) {
                std::string absolute_path = std::string("/dev/input/") + ent->d_name;
                char name[80] = {0};

                fd = open(absolute_path.c_str(), O_RDWR);
                if (fd < 0) {
                    continue;
                }

                if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), &name) > 0) {
                    if (strcmp(name, "fts_ts") == 0 || strcmp(name, "fts") == 0 || 
                        strcmp(name, "goodix_ts") == 0 || strcmp(name, "NVTCapacitiveTouchScreen") == 0) {
                        break;
                    }
                }

                close(fd);
                fd = -1;
            }
        }
        closedir(dir);
    }
    return fd;
}

}  // anonymous namespace

class XiaomiSm6225UdfpsHandler : public UdfpsHandler {
  public:
    XiaomiSm6225UdfpsHandler() : mDevice(nullptr), mPendingCleanup(false), 
                                  mHbmStuck(false), mAuthInProgress(false), mIsScreenOnFod(false),
                                  mSamplesRemaining(0), mIsFinalEnrollment(false), mHbmEnabled(false),
                                  isFpcFod(false), mFodDisabledAttempts(0) {}

    ~XiaomiSm6225UdfpsHandler() override {
        shutdownThreads();
    }

    void init(fingerprint_device_t* device) override {
        mDevice = device;
        
        touch_fd_ = android::base::unique_fd(open(TOUCH_DEV_PATH, O_RDWR));
        disp_fd_ = android::base::unique_fd(open(DISP_FEATURE_PATH, O_RDWR));
        drm_fd_ = android::base::unique_fd(open(DRM_DEV_PATH, O_RDWR));

        std::string fpVendor = android::base::GetProperty("persist.vendor.sys.fp.vendor", "none");
        isFpcFod = (fpVendor == "fpc_fod");

        fodThread_ = std::thread([this]() { fodPressMonitorThread(); });
        dispThread_ = std::thread([this]() { displayEventMonitorThread(); });
        
        if (isFpcFod) {
            screenThread_ = std::thread([this]() { screenStateMonitorThread(); });
        }
    }

    void onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) override {
        if (mPendingCleanup.load()) {
            return;
        }

        sendEarlyWakeupHint();
        
        mHbmStuck = false;
        mAuthInProgress = true;
        mIsFinalEnrollment = false;
        mFodDisabledAttempts = 0;
        
        if (isFpcFod) {
            setFodStatus(FOD_STATUS_ON);
        }

        setFingerDown(true);
    }

    void onFingerUp() override {
        if (mPendingCleanup.load()) {
            bool wasFinalEnrollment = mIsFinalEnrollment.load();
            mIsFinalEnrollment = false;
            mPendingCleanup = false;
            enrolling.store(false);
            mSamplesRemaining = 0;
            mIsFingerDown = false;
            forceHbmCleanup(wasFinalEnrollment);
            return;
        }
        
        setFingerDown(false);
    }

    void onError(int32_t error, int32_t vendorCode) override {
        if (error == 2 || error == 3 || error == 5) {
            mAuthInProgress = false;
            mIsFingerDown = false;
            forceHbmCleanup(false);
            setFodStatus(FOD_STATUS_OFF);
        }
        
        setDispFpStatus(FINGERPRINT_NONE);
    }

    void onAcquired(int32_t result, int32_t vendorCode) override {
        auto acquired = static_cast<AcquiredInfo>(result);
        
        if (acquired == AcquiredInfo::GOOD) {
            if (enrolling.load() && !mPendingCleanup.load()) {
                bool isFinal = (mSamplesRemaining.load() == 0);
                if (isFinal) {
                    if (mIsFingerDown.load()) {
                        scheduleHbmTimeout(true);
                    }
                    return;
                } else {
                    setDispFpStatus(ENROLL_STOP);
                    forceHbmCleanup(false);
                    return;
                }
            }
            
            if (mPendingCleanup.load()) return;
            
            forceHbmCleanup(false);
            
            if (!enrolling.load()) {
                setDispFpStatus(FINGERPRINT_NONE);
            }
            return;
        }
        
        if (acquired == AcquiredInfo::INSUFFICIENT || 
            acquired == AcquiredInfo::TOO_SLOW ||
            acquired == AcquiredInfo::TOO_FAST ||
            acquired == AcquiredInfo::PARTIAL) {
            
            mAuthInProgress = false;
            mIsFingerDown = false;
            forceHbmCleanup(false);
            setDispFpStatus(FINGERPRINT_NONE);
            setFodStatus(FOD_STATUS_OFF);
            
            return;
        }

        if (!isFpcFod && vendorCode == 21) {
            setFodStatus(FOD_STATUS_ON);
        } else if (isFpcFod && vendorCode == 22) {
            setFodStatus(FOD_STATUS_ON);
        }
        
        if (vendorCode == 23 && mIsFingerDown) {
            if (!mPendingCleanup.load()) {
                mHbmStuck = true;
                forceHbmCleanup(false);
            }
        }
    }

    void onEnrollmentProgress(int32_t /*enrollmentId*/, int32_t remaining) override {
        mSamplesRemaining = remaining;
        if (remaining == 0) {
            mIsFinalEnrollment = true;
        }
    }

    void preEnroll() override {
        mPendingCleanup = false;
        mHbmStuck = false;
        mSamplesRemaining = 0;
        mIsFinalEnrollment = false;
        enrolling.store(true);
        setDispFpStatus(ENROLL_START);
    }

    void enroll() override {
        enrolling.store(true);
        mSamplesRemaining = 0;
        mIsFinalEnrollment = false;
        setDispFpStatus(ENROLL_START);
    }

    void postEnroll() override {
        setDispFpStatus(ENROLL_STOP);
        enrolling.store(false);
        forceHbmCleanup(false);
        setDispFpStatus(FINGERPRINT_NONE);
    }

    void cancel() override {
        enrolling.store(false);
        setDispFpStatus(FINGERPRINT_NONE);
        forceHbmCleanup(false);
    }

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
    std::atomic<int> mFodDisabledAttempts{0};
    
    bool isFpcFod;
    
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

    bool isFingerprintActive() {
        return enrolling.load() || 
               mAuthInProgress.load() || 
               mPendingCleanup.load() ||
               mIsFingerDown.load();
    }

    void sendEarlyWakeupHint() {
        if (drm_fd_.get() >= 0) {
            drm_msm_display_hint hint = {};
            hint.hint_flags = DRM_MSM_DISPLAY_EARLY_WAKEUP_HINT;
            ioctl(drm_fd_.get(), DRM_IOCTL_MSM_DISPLAY_HINT, &hint);
        }
    }

    void setDispFpStatus(int status) {
        std::lock_guard<std::mutex> lock(disp_mutex_);
        if (disp_fd_.get() >= 0) {
            disp_feature_req fp_req;
            fp_req.base.flag = 0;
            fp_req.base.disp_id = MI_DISP_PRIMARY;
            fp_req.feature_id = DISP_FEATURE_FP_STATUS;
            fp_req.feature_val = status;
            fp_req.tx_len = 0;
            fp_req.tx_ptr = 0;
            fp_req.rx_len = 0;
            fp_req.rx_ptr = 0;
            ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_FEATURE, &fp_req);
        }
    }

    void enableHbm() {
        if (mHbmEnabled.load()) return;

        std::lock_guard<std::mutex> lock(disp_mutex_);
        if (disp_fd_.get() >= 0) {
            disp_local_hbm_req req;
            req.base.flag = 0;
            req.base.disp_id = MI_DISP_PRIMARY;
            req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT;
            if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) == 0) {
                mHbmEnabled = true;
            }
        }
    }

    void disableHbm() {
        std::lock_guard<std::mutex> lock(disp_mutex_);
        if (disp_fd_.get() >= 0) {
            disp_local_hbm_req req;
            req.base.flag = 0;
            req.base.disp_id = MI_DISP_PRIMARY;
            req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
            if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) == 0) {
                mHbmEnabled = false;
            }
        }
    }

    int getBrightness() {
        android::base::unique_fd fd(open(BRIGHTNESS_PATH, O_RDONLY));
        if (fd.get() < 0) return -1;
        char buf[12];
        ssize_t len = read(fd.get(), buf, sizeof(buf) - 1);
        if (len <= 0) return -1;
        buf[len] = '\0';
        return atoi(buf);
    }

    bool isScreenOn() {
        return getBrightness() > 0;
    }

    void scheduleHbmTimeout(bool isFinalEnrollment = false) {
        std::thread oldThread;
        
        {
            std::lock_guard<std::mutex> lock(cleanup_mutex_);
            if (cleanupThread_.joinable()) {
                cleanupThreadRunning = false;
                cleanup_cv_.notify_all();
                oldThread = std::move(cleanupThread_);
            }
        }
        
        if (oldThread.joinable()) {
            oldThread.join();
        }

        {
            std::lock_guard<std::mutex> lock(cleanup_mutex_);
            cleanupThreadRunning = true;
            
            cleanupThread_ = std::thread([this, isFinalEnrollment]() {
                std::unique_lock<std::mutex> threadLock(cleanup_mutex_);
                cleanup_cv_.wait_for(threadLock, std::chrono::milliseconds(300));
                if (!cleanupThreadRunning) return;
                
                if (isFinalEnrollment) {
                    mPendingCleanup = false;
                    mIsFinalEnrollment = false;
                    enrolling.store(false);
                    mSamplesRemaining = 0;
                    forceHbmCleanup(true);
                    setDispFpStatus(FINGERPRINT_NONE);
                    return;
                }
                
                if (!mPendingCleanup.load() && !enrolling.load()) {
                    forceHbmCleanup(false);
                }
            });
        }
    }

    void forceHbmCleanup(bool isFinalEnrollment = false) {
        // First, aggressively disable FOD mode with retries
        bool fodDisabled = false;
        for (int attempt = 0; attempt < 10 && !fodDisabled; attempt++) {
            fodDisabled = setFodStatusWithRetry(FOD_STATUS_OFF);
            if (!fodDisabled && attempt < 9) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        
        // Even if setFodStatus failed, try direct ioctl to force reset
        if (!fodDisabled) {
            LOG(WARNING) << "Force HBM cleanup: direct FOD disable failed after retries, attempting force reset";
            directForceFodOff();
        }
        
        disableHbm();
        
        if (isFinalEnrollment) {
            std::lock_guard<std::mutex> lock(disp_mutex_);
            if (disp_fd_.get() >= 0) {
                disp_feature_req fp_req;
                fp_req.base.flag = 0;
                fp_req.base.disp_id = MI_DISP_PRIMARY;
                fp_req.feature_id = DISP_FEATURE_FP_STATUS;
                fp_req.feature_val = FINGERPRINT_NONE;
                fp_req.tx_len = 0;
                fp_req.tx_ptr = 0;
                fp_req.rx_len = 0;
                fp_req.rx_ptr = 0;
                ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_FEATURE, &fp_req);
            }
        }
        
        // Reset all state flags
        mIsFingerDown = false;
        mPendingCleanup = false;
        mHbmStuck = false;
        mAuthInProgress = false;
        mIsScreenOnFod = false;
        mSamplesRemaining = 0;
        mIsFinalEnrollment = false;
        mFodDisabledAttempts = 0;
    }

    void directForceFodOff() {
        // Direct write to touch device to force FOD off
        if (touch_fd_.get() < 0) return;
        
        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, FOD_STATUS_OFF};
        
        // Try multiple times with increasing delays
        for (int attempt = 0; attempt < 15; attempt++) {
            if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) == 0) {
                LOG(INFO) << "Direct force FOD off succeeded on attempt " << attempt;
                return;
            }
            int err = errno;
            LOG(WARNING) << "Direct force FOD off attempt " << attempt << " failed: " << strerror(err);
            if (err != EBUSY && err != EAGAIN && err != EINTR) {
                break;
            }
            // Increasing backoff delay
            std::this_thread::sleep_for(std::chrono::milliseconds(5 * (attempt + 1)));
        }
    }

    void setFingerDown(bool pressed) {
        if (mPendingCleanup.load()) return;
        
        bool screenOn = isScreenOn();
        
        if (screenOn && pressed) {
            mIsScreenOnFod = true;
            mAuthInProgress = true;
            enableHbm();
            if (!enrolling.load()) scheduleHbmTimeout(false);
        }
        
        if (pressed) {
            setFodStatus(FOD_STATUS_ON);
        }

        if (!enrolling.load()) {
            std::lock_guard<std::mutex> lock(disp_mutex_);
            if (disp_fd_.get() >= 0) {
                disp_local_hbm_req req;
                req.base.flag = 0;
                req.base.disp_id = MI_DISP_PRIMARY;
                req.local_hbm_value = pressed ? LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT
                                              : LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
                if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) == 0 && !pressed) {
                    mHbmEnabled = false;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(device_mutex_);
            if (mDevice != nullptr) {
                mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS,
                              pressed ? PARAM_FOD_PRESSED : PARAM_FOD_RELEASED);
            }
        }
        
        if (!enrolling.load()) setDispFpStatus(pressed ? AUTH_START : AUTH_STOP);
        
        mIsFingerDown = pressed;
        
        if (!pressed) {
            mAuthInProgress = false;
            mIsScreenOnFod = false;
            mIsFinalEnrollment = false;
            
            if (mPendingCleanup.load()) {
                mPendingCleanup = false;
                enrolling.store(false);
                mSamplesRemaining = 0;
            }
            
            forceHbmCleanup(false);
        }
    }

    void shutdownThreads() {
        isRunning.store(false);
        
        {
            std::lock_guard<std::mutex> lock(cleanup_mutex_);
            cleanupThreadRunning = false;
            cleanup_cv_.notify_all();
        }
        
        if (cleanupThread_.joinable()) cleanupThread_.join();
        if (fodThread_.joinable()) fodThread_.join();
        if (dispThread_.joinable()) dispThread_.join();
        if (screenThread_.joinable()) screenThread_.join();
    }

    void screenStateMonitorThread() {
        int lastState = -1;
        while (isRunning.load()) {
            int brightness = getBrightness();
            if (brightness != -1) {
                int currentState = (brightness == 0) ? 0 : 1;
                bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);

                if (currentState != lastState) {
                    if (currentState == 0 && isFpcFod && isScreenOffEnabled) {
                        setFodStatus(FOD_STATUS_ON);
                    } else if (currentState == 1 && isFpcFod) {
                        // If screen turns on, ensure FOD is properly disabled
                        // even if authentication is in progress (it might have completed)
                        if (!enrolling.load()) {
                            // Small delay to allow authentication to complete if in progress
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            if (!mAuthInProgress.load()) {
                                forceHbmCleanup(false);
                            }
                        }
                    }
                    lastState = currentState;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    bool setFodStatusWithRetry(int value) {
        if (value == FOD_STATUS_ON && isScreenOn()) {
            return true;
        }

        std::lock_guard<std::mutex> lock(touch_mutex_);
        if (touch_fd_.get() < 0) return false;

        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, value};
        
        // Improved retry mechanism with increasing delays
        for (int attempt = 0; attempt < 5; attempt++) {
            if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) == 0) {
                mFodDisabledAttempts = 0;
                return true;
            }
            int err = errno;
            LOG(WARNING) << "setFodStatus(" << value << ") attempt " << attempt 
                        << " failed: " << strerror(err);
            
            if (err != EBUSY && err != EAGAIN && err != EINTR) {
                mFodDisabledAttempts++;
                return false;
            }
            
            // Exponential backoff: 5ms, 10ms, 20ms, 40ms, 80ms
            int delay_ms = 5 * (1 << attempt);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        
        mFodDisabledAttempts++;
        return false;
    }

    void setFodStatus(int value) {
        if (value == FOD_STATUS_ON && isScreenOn()) {
            return;
        }

        setFodStatusWithRetry(value);
    }

    void fodPressMonitorThread() {
        int fd = -1;
        while (isRunning.load() && fd < 0) {
            fd = open_ts_input();
            if (fd < 0) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (fd < 0) return;

        android::base::unique_fd touchFd(fd);
        struct pollfd tsPoll = { .fd = touchFd.get(), .events = POLLIN, .revents = 0 };
        struct input_event ev;
        
        while (isRunning.load()) {
            int rc = poll(&tsPoll, 1, 1000);
            if (rc <= 0) continue;

            if (tsPoll.revents & POLLIN) {
                ssize_t bytesRead = read(touchFd.get(), &ev, sizeof(struct input_event));
                if (bytesRead < (ssize_t)sizeof(struct input_event)) continue;

                bool screenOn = isScreenOn();
                bool fpActive = isFingerprintActive();
                
                if (ev.type == EV_KEY && ev.code == BTN_INFO) {
                    bool pressed = (ev.value == 1);
                    bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);
                    
                    // Always ensure FOD is disabled on finger up, even if we're in a cleanup state
                    if (!pressed) {
                        // Directly disable FOD, don't rely on state flags
                        setFodStatus(FOD_STATUS_OFF);
                    }
                    
                    if (!screenOn && !isScreenOffEnabled) continue;
                    
                    if (screenOn && pressed && !fpActive && !mPendingCleanup.load()) {
                        continue;
                    }
                    
                    mIsFingerDown = pressed;
                    
                    if (!pressed && mPendingCleanup.load()) {
                        bool wasFinalEnrollment = mIsFinalEnrollment.load();
                        mIsFinalEnrollment = false;
                        mPendingCleanup = false;
                        enrolling.store(false);
                        mSamplesRemaining = 0;
                        forceHbmCleanup(wasFinalEnrollment);
                        continue;
                    }
                    
                    if (!screenOn && isScreenOffEnabled) {
                        if (pressed) {
                            setFodStatus(FOD_STATUS_ON);
                            enableHbm();
                            std::lock_guard<std::mutex> lock(device_mutex_);
                            if (mDevice != nullptr) mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, PARAM_FOD_PRESSED);
                        } else {
                            std::lock_guard<std::mutex> lock(device_mutex_);
                            if (mDevice != nullptr) mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, PARAM_FOD_RELEASED);
                            forceHbmCleanup(false);
                        }
                        continue;
                    }
                    
                    if (screenOn) {
                        if (pressed) {
                            setFingerDown(true);
                            if (!enrolling.load()) scheduleHbmTimeout(false);
                        } else {
                            setFingerDown(false);
                            if (!enrolling.load() && !mPendingCleanup.load()) {
                                forceHbmCleanup(false);
                            }
                        }
                    }
                }
                
                if (screenOn && ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID && ev.value >= 0) {
                    if (fpActive && !mIsFingerDown.load() && !mPendingCleanup.load()) {
                        setFingerDown(true);
                        if (!enrolling.load()) scheduleHbmTimeout(false);
                    }
                }
                
                if (screenOn && ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID && ev.value == -1) {
                    if (mIsFingerDown.load()) {
                        mIsFingerDown = false;
                        if (mPendingCleanup.load()) {
                            bool wasFinalEnrollment = mIsFinalEnrollment.load();
                            mIsFinalEnrollment = false;
                            mPendingCleanup = false;
                            enrolling.store(false);
                            mSamplesRemaining = 0;
                            forceHbmCleanup(wasFinalEnrollment);
                        } else {
                            setFingerDown(false);
                        }
                    }
                }
            }
        }
    }

    void displayEventMonitorThread() {
        if (disp_fd_.get() < 0) return;

        disp_event_req req;
        req.base.flag = 0;
        req.base.disp_id = MI_DISP_PRIMARY;
        req.type = MI_DISP_EVENT_FOD;
        ioctl(disp_fd_.get(), MI_DISP_IOCTL_REGISTER_EVENT, &req);

        struct pollfd dispEventPoll = { .fd = disp_fd_.get(), .events = POLLIN, .revents = 0 };

        while (isRunning.load()) {
            int rc = poll(&dispEventPoll, 1, 1000);
            if (rc <= 0) continue;

            if (dispEventPoll.revents & POLLIN) {
                dispEventPoll.revents = 0;
                disp_event_resp* response = parseDispEvent(disp_fd_.get());
                if (response == nullptr) continue;

                if (response->base.type == MI_DISP_EVENT_FOD) {
                    int value = response->data[0];
                    bool localHbmUiReady = value & LOCAL_HBM_UI_READY;
                    
                    std::lock_guard<std::mutex> deviceLock(device_mutex_);
                    if (mDevice != nullptr) {
                        mDevice->extCmd(mDevice, COMMAND_NIT,
                                      localHbmUiReady ? PARAM_NIT_FOD : PARAM_NIT_NONE);
                    }
                }
            }
        }
    }

    const char* getFingerprintStatusName(int status) {
        switch(status) {
            case FINGERPRINT_NONE: return "NONE";
            case AUTH_START: return "AUTH_START";
            case AUTH_STOP: return "AUTH_STOP";
            case ENROLL_START: return "ENROLL_START";
            case ENROLL_STOP: return "ENROLL_STOP";
            default: return "UNKNOWN";
        }
    }
};

static UdfpsHandler* create() {
    return new XiaomiSm6225UdfpsHandler();
}

static void destroy(UdfpsHandler* handler) {
    delete handler;
}

extern "C" UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
    .create = create,
    .destroy = destroy,
};
