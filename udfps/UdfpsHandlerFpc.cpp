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

#include <poll.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <dirent.h>
#include <limits.h>
#include <sys/time.h>
#include <sstream>
#include <iomanip>

#include "display/drm/mi_disp.h"
#include "display/drm/sde_drm.h"
#include "display/drm/msm_drm_pp.h"
#include "xiaomi_touch.h"
#include "FpcUdfpsHandler.h"

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
        LOG(ERROR) << "Failed to read display event, errno: " << errno;
        return nullptr;
    }

    if (size < (ssize_t)sizeof(struct disp_event)) {
        LOG(ERROR) << "Invalid event size " << size << ", expected at least " << sizeof(struct disp_event);
        return nullptr;
    }

    LOG(VERBOSE) << "Display event read successfully, size: " << size;
    return reinterpret_cast<disp_event_resp*>(event_data);
}

static int open_ts_input() {
    LOG(INFO) << "Searching for touchscreen input device...";
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
                    LOG(DEBUG) << "Found input device: " << name << " at " << absolute_path;
                    if (strcmp(name, "fts_ts") == 0 || strcmp(name, "fts") == 0 || 
                        strcmp(name, "goodix_ts") == 0 || strcmp(name, "NVTCapacitiveTouchScreen") == 0) {
                        LOG(INFO) << "Found touchscreen device: " << name;
                        break;
                    }
                }

                close(fd);
                fd = -1;
            }
        }
        closedir(dir);
    } else {
        LOG(ERROR) << "Failed to open /dev/input directory";
    }
    
    if (fd < 0) {
        LOG(ERROR) << "No touchscreen device found";
    } else {
        LOG(INFO) << "Touchscreen device opened, fd: " << fd;
    }
    return fd;
}

}  // anonymous namespace

FpcUdfpsHandler::FpcUdfpsHandler() : mDevice(nullptr) {
    LOG(INFO) << "FpcUdfpsHandler constructor called";
}

FpcUdfpsHandler::~FpcUdfpsHandler() {
    LOG(INFO) << "FpcUdfpsHandler destructor called";
    shutdownThreads();
}

void FpcUdfpsHandler::init(fingerprint_device_t* device) {
    LOG(INFO) << "Initializing FPC UDFPS handler";
    
    mDevice = device;
    
    touch_fd_ = android::base::unique_fd(open(TOUCH_DEV_PATH, O_RDWR));
    if (touch_fd_.get() < 0) {
        LOG(ERROR) << "Failed to open touch device " << TOUCH_DEV_PATH << ": " << strerror(errno);
    } else {
        LOG(INFO) << "Touch device opened successfully, fd: " << touch_fd_.get();
    }
    
    disp_fd_ = android::base::unique_fd(open(DISP_FEATURE_PATH, O_RDWR));
    if (disp_fd_.get() < 0) {
        LOG(ERROR) << "Failed to open display device " << DISP_FEATURE_PATH << ": " << strerror(errno);
    } else {
        LOG(INFO) << "Display device opened successfully, fd: " << disp_fd_.get();
    }
    
    drm_fd_ = android::base::unique_fd(open(DRM_DEV_PATH, O_RDWR));
    if (drm_fd_.get() < 0) {
        LOG(ERROR) << "Failed to open DRM device " << DRM_DEV_PATH << ": " << strerror(errno);
    } else {
        LOG(INFO) << "DRM device opened successfully, fd: " << drm_fd_.get();
    }

    fodThread_ = std::thread([this]() { fodPressMonitorThread(); });
    dispThread_ = std::thread([this]() { displayEventMonitorThread(); });
    screenThread_ = std::thread([this]() { screenStateMonitorThread(); });
    
    LOG(INFO) << "FPC UDFPS handler initialized successfully";
}

void FpcUdfpsHandler::onFingerDown(uint32_t x, uint32_t y, float minor, float major) {
    LOG(INFO) << __func__ << " - Finger down at (" << x << ", " << y << "), minor=" << minor << ", major=" << major;
    
    if (mPendingCleanup.load()) {
        LOG(INFO) << "Pending cleanup active, ignoring finger down";
        return;
    }

    sendEarlyWakeupHint();
    
    mHbmStuck = false;
    mAuthInProgress = true;
    mIsFinalEnrollment = false;
    mFingerUpSent = false;
    
    LOG(DEBUG) << "Setting FOD status ON and finger down";
    setFodStatus(FOD_STATUS_ON);
    setFingerDown(true);
}

void FpcUdfpsHandler::onFingerUp() {
    LOG(INFO) << __func__ << " - Finger up event";
    
    if (mPendingCleanup.load()) {
        bool wasFinalEnrollment = mIsFinalEnrollment.load();
        LOG(INFO) << "Pending cleanup active, final enrollment: " << wasFinalEnrollment;
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

void FpcUdfpsHandler::onError(int32_t error, int32_t vendorCode) {
    LOG(ERROR) << __func__ << " - Error: " << error << ", vendorCode: " << vendorCode;
    
    if (error == 2 || error == 3 || error == 5) {
        LOG(INFO) << "Critical error detected, cleaning up";
        mAuthInProgress = false;
        mIsFingerDown = false;
        forceHbmCleanup(false);
        setFodStatus(FOD_STATUS_OFF);
    }
    
    setDispFpStatus(FINGERPRINT_NONE);
}

void FpcUdfpsHandler::onAcquired(int32_t result, int32_t vendorCode) {
    auto acquired = static_cast<AcquiredInfo>(result);
    LOG(INFO) << __func__ << " - result: " << result << " (" << static_cast<int>(acquired) 
              << "), vendorCode: " << vendorCode;
    
    if (acquired == AcquiredInfo::GOOD) {
        LOG(INFO) << "Acquired GOOD - Successful fingerprint capture";
        if (enrolling.load() && !mPendingCleanup.load()) {
            bool isFinal = (mSamplesRemaining.load() == 0);
            LOG(INFO) << "Enrollment in progress, isFinal: " << isFinal;
            if (isFinal) {
                if (mIsFingerDown.load()) {
                    LOG(INFO) << "Final enrollment sample, scheduling HBM timeout";
                    scheduleHbmTimeout(true);
                }
                return;
            } else {
                LOG(INFO) << "Intermediate enrollment sample, stopping";
                setDispFpStatus(ENROLL_STOP);
                forceHbmCleanup(false);
                return;
            }
        }
        
        if (mPendingCleanup.load()) {
            LOG(INFO) << "Pending cleanup active, ignoring GOOD acquire";
            return;
        }
        
        forceHbmCleanup(false);
        
        if (!enrolling.load()) {
            LOG(DEBUG) << "Not enrolling, setting fingerprint status to NONE";
            setDispFpStatus(FINGERPRINT_NONE);
        }
        return;
    }
    
    if (acquired == AcquiredInfo::INSUFFICIENT || 
        acquired == AcquiredInfo::TOO_SLOW ||
        acquired == AcquiredInfo::TOO_FAST ||
        acquired == AcquiredInfo::PARTIAL) {
        
        LOG(INFO) << "Poor quality acquire, cleaning up";
        mAuthInProgress = false;
        mIsFingerDown = false;
        forceHbmCleanup(false);
        setDispFpStatus(FINGERPRINT_NONE);
        setFodStatus(FOD_STATUS_OFF);
        
        return;
    }

    if (vendorCode == 22) {
        LOG(INFO) << "Vendor code 22 - Setting FOD status ON";
        setFodStatus(FOD_STATUS_ON);
    }
    
    if (vendorCode == 23 && mIsFingerDown) {
        LOG(INFO) << "Vendor code 23 - HBM stuck detected";
        if (!mPendingCleanup.load()) {
            mHbmStuck = true;
            forceHbmCleanup(false);
        }
    }
}

void FpcUdfpsHandler::onEnrollmentProgress(int32_t enrollmentId, int32_t remaining) {
    LOG(INFO) << __func__ << " - enrollmentId: " << enrollmentId << ", remaining: " << remaining;
    mSamplesRemaining = remaining;
    if (remaining == 0) {
        LOG(INFO) << "Enrollment complete, setting final flag";
        mIsFinalEnrollment = true;
    }
}

void FpcUdfpsHandler::preEnroll() {
    LOG(INFO) << __func__ << " - Pre-enroll started";
    mPendingCleanup = false;
    mHbmStuck = false;
    mSamplesRemaining = 0;
    mIsFinalEnrollment = false;
    enrolling.store(true);
    setDispFpStatus(ENROLL_START);
}

void FpcUdfpsHandler::enroll() {
    LOG(INFO) << __func__ << " - Enrollment started";
    enrolling.store(true);
    mSamplesRemaining = 0;
    mIsFinalEnrollment = false;
    setDispFpStatus(ENROLL_START);
}

void FpcUdfpsHandler::postEnroll() {
    LOG(INFO) << __func__ << " - Post-enroll completed";
    setDispFpStatus(ENROLL_STOP);
    enrolling.store(false);
    forceHbmCleanup(false);
    setDispFpStatus(FINGERPRINT_NONE);
}

void FpcUdfpsHandler::cancel() {
    LOG(INFO) << __func__ << " - Canceling fingerprint operation";
    enrolling.store(false);
    setDispFpStatus(FINGERPRINT_NONE);
    forceHbmCleanup(false);
}

bool FpcUdfpsHandler::isFingerprintActive() {
    bool active = enrolling.load() || mAuthInProgress.load() || mPendingCleanup.load() || mIsFingerDown.load();
    LOG(VERBOSE) << "Fingerprint active: " << active 
                 << " (enrolling=" << enrolling.load() 
                 << ", auth=" << mAuthInProgress.load() 
                 << ", cleanup=" << mPendingCleanup.load() 
                 << ", fingerDown=" << mIsFingerDown.load() << ")";
    return active;
}

void FpcUdfpsHandler::sendEarlyWakeupHint() {
    LOG(INFO) << "Sending early wakeup hint to DRM";
    if (drm_fd_.get() >= 0) {
        drm_msm_display_hint hint = {};
        hint.hint_flags = DRM_MSM_DISPLAY_EARLY_WAKEUP_HINT;
        if (ioctl(drm_fd_.get(), DRM_IOCTL_MSM_DISPLAY_HINT, &hint) == 0) {
            LOG(DEBUG) << "Early wakeup hint sent successfully";
        } else {
            LOG(ERROR) << "Failed to send early wakeup hint: " << strerror(errno);
        }
    } else {
        LOG(ERROR) << "DRM device not opened, cannot send hint";
    }
}

void FpcUdfpsHandler::setDispFpStatus(int status) {
    LOG(INFO) << __func__ << " - Setting display FP status to: " << getFingerprintStatusName(status);
    
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
        if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_FEATURE, &fp_req) == 0) {
            LOG(DEBUG) << "Display FP status set successfully";
        } else {
            LOG(ERROR) << "Failed to set display FP status: " << strerror(errno);
        }
    } else {
        LOG(ERROR) << "Display device not opened, cannot set FP status";
    }
}

void FpcUdfpsHandler::enableHbm() {
    if (mHbmEnabled.load()) {
        LOG(VERBOSE) << "HBM already enabled, skipping";
        return;
    }

    LOG(INFO) << "Enabling HBM";
    std::lock_guard<std::mutex> lock(disp_mutex_);
    if (disp_fd_.get() >= 0) {
        disp_local_hbm_req req;
        req.base.flag = 0;
        req.base.disp_id = MI_DISP_PRIMARY;
        req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT;
        if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) == 0) {
            mHbmEnabled = true;
            LOG(INFO) << "HBM enabled successfully";
        } else {
            LOG(ERROR) << "Failed to enable HBM: " << strerror(errno);
        }
    } else {
        LOG(ERROR) << "Display device not opened, cannot enable HBM";
    }
}

void FpcUdfpsHandler::disableHbm() {
    if (!mHbmEnabled.load()) {
        LOG(VERBOSE) << "HBM already disabled, skipping";
        return;
    }

    LOG(INFO) << "Disabling HBM";
    std::lock_guard<std::mutex> lock(disp_mutex_);
    if (disp_fd_.get() >= 0) {
        disp_local_hbm_req req;
        req.base.flag = 0;
        req.base.disp_id = MI_DISP_PRIMARY;
        req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
        if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) == 0) {
            mHbmEnabled = false;
            LOG(INFO) << "HBM disabled successfully";
        } else {
            LOG(ERROR) << "Failed to disable HBM: " << strerror(errno);
        }
    } else {
        LOG(ERROR) << "Display device not opened, cannot disable HBM";
    }
}

int FpcUdfpsHandler::getBrightness() {
    android::base::unique_fd fd(open(BRIGHTNESS_PATH, O_RDONLY));
    if (fd.get() < 0) {
        LOG(ERROR) << "Failed to open brightness path: " << strerror(errno);
        return -1;
    }
    char buf[12];
    ssize_t len = read(fd.get(), buf, sizeof(buf) - 1);
    if (len <= 0) {
        LOG(ERROR) << "Failed to read brightness value";
        return -1;
    }
    buf[len] = '\0';
    int brightness = atoi(buf);
    LOG(VERBOSE) << "Current brightness: " << brightness;
    return brightness;
}

bool FpcUdfpsHandler::isScreenOn() {
    bool on = getBrightness() > 0;
    LOG(VERBOSE) << "Screen is " << (on ? "ON" : "OFF");
    return on;
}

void FpcUdfpsHandler::scheduleHbmTimeout(bool isFinalEnrollment) {
    LOG(INFO) << __func__ << " - Scheduling HBM timeout, isFinalEnrollment: " << isFinalEnrollment;
    
    std::thread oldThread;
    
    {
        std::lock_guard<std::mutex> lock(cleanup_mutex_);
        if (cleanupThread_.joinable()) {
            LOG(INFO) << "Stopping existing cleanup thread";
            cleanupThreadRunning = false;
            cleanup_cv_.notify_all();
            oldThread = std::move(cleanupThread_);
        }
    }
    
    if (oldThread.joinable()) {
        LOG(INFO) << "Joining old cleanup thread";
        oldThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(cleanup_mutex_);
        cleanupThreadRunning = true;
        LOG(INFO) << "Starting new cleanup thread";
        
        cleanupThread_ = std::thread([this, isFinalEnrollment]() {
            LOG(INFO) << "Cleanup thread started";
            std::unique_lock<std::mutex> threadLock(cleanup_mutex_);
            cleanup_cv_.wait_for(threadLock, std::chrono::milliseconds(300));
            if (!cleanupThreadRunning) {
                LOG(INFO) << "Cleanup thread was stopped early";
                return;
            }
            
            LOG(INFO) << "HBM timeout triggered, isFinalEnrollment: " << isFinalEnrollment;
            if (isFinalEnrollment) {
                LOG(INFO) << "Final enrollment cleanup";
                mPendingCleanup = false;
                mIsFinalEnrollment = false;
                enrolling.store(false);
                mSamplesRemaining = 0;
                forceHbmCleanup(true);
                setDispFpStatus(FINGERPRINT_NONE);
                return;
            }
            
            if (!mPendingCleanup.load() && !enrolling.load()) {
                LOG(INFO) << "Non-enrollment cleanup";
                forceHbmCleanup(false);
            } else {
                LOG(INFO) << "Skipping cleanup - pending: " << mPendingCleanup.load() 
                          << ", enrolling: " << enrolling.load();
            }
        });
    }
}

void FpcUdfpsHandler::forceHbmCleanup(bool isFinalEnrollment) {
    LOG(INFO) << __func__ << " - Forcing HBM cleanup, isFinalEnrollment: " << isFinalEnrollment;
    
    directForceFodOff();
    disableHbm();
    
    if (isFinalEnrollment) {
        LOG(INFO) << "Final enrollment cleanup - resetting display FP status";
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
            if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_FEATURE, &fp_req) == 0) {
                LOG(DEBUG) << "Display FP status reset successfully";
            } else {
                LOG(ERROR) << "Failed to reset display FP status";
            }
        }
    }
    
    mIsFingerDown = false;
    mPendingCleanup = false;
    mHbmStuck = false;
    mAuthInProgress = false;
    mIsScreenOnFod = false;
    mSamplesRemaining = 0;
    mIsFinalEnrollment = false;
    mFingerUpSent = false;
    mLastFodState = -1;
    
    LOG(INFO) << "Cleanup completed, all states reset";
}

void FpcUdfpsHandler::directForceFodOff() {
    LOG(INFO) << __func__ << " - Force turning FOD off";
    if (touch_fd_.get() < 0) {
        LOG(ERROR) << "Touch device not opened, cannot force FOD off";
        return;
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    LOG(VERBOSE) << "Waited 30ms before FOD off sequence";

    {
        std::lock_guard<std::mutex> lock(touch_mutex_);
        int bufUp[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, 0};
        if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &bufUp) == 0) {
            mFingerUpSent = true;
            LOG(DEBUG) << "Finger up sent successfully";
        } else {
            LOG(ERROR) << "Failed to send finger up: " << strerror(errno);
        }
    }

    {
        std::lock_guard<std::mutex> lock(touch_mutex_);
        int bufOff[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, FOD_STATUS_OFF};
        if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &bufOff) == 0) {
            LOG(DEBUG) << "FOD disabled successfully";
        } else {
            LOG(ERROR) << "Failed to disable FOD: " << strerror(errno);
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(touch_mutex_);
        int bufActive[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Active_MODE, 1};
        if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &bufActive) == 0) {
            LOG(DEBUG) << "Active mode set successfully";
        } else {
            LOG(ERROR) << "Failed to set active mode: " << strerror(errno);
        }
    }

    // INJECTED D2TW RESTORE: Must happen after Touch_Active_MODE is set
    LOG(INFO) << "Restoring Double-Tap to Wake state (D2TW)";
    {
        std::lock_guard<std::mutex> lock(touch_mutex_);
        int d2twBuf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Doubletap_Mode, 1};
        if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &d2twBuf) == 0) {
            LOG(INFO) << "D2TW restored successfully in directForceFodOff";
        } else {
            LOG(ERROR) << "Failed to restore D2TW state: " << strerror(errno);
        }
    }
}

void FpcUdfpsHandler::setFingerDown(bool pressed) {
    LOG(INFO) << __func__ << " - Finger " << (pressed ? "DOWN" : "UP");
    
    if (mPendingCleanup.load()) {
        LOG(INFO) << "Pending cleanup active, ignoring finger " << (pressed ? "down" : "up");
        return;
    }
    
    bool screenOn = isScreenOn();
    LOG(DEBUG) << "Screen is " << (screenOn ? "ON" : "OFF");

    {
        std::lock_guard<std::mutex> lock(touch_mutex_);
        if (touch_fd_.get() >= 0) {
            int bufDownUp[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, pressed ? 1 : 0};
            if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &bufDownUp) == 0) {
                mFingerUpSent = !pressed;
                LOG(DEBUG) << "Finger " << (pressed ? "down" : "up") << " sent successfully";
            } else {
                LOG(ERROR) << "Failed to send finger " << (pressed ? "down" : "up") << ": " << strerror(errno);
            }
        } else {
            LOG(ERROR) << "Touch device not opened, cannot set finger state";
        }
    }
    
    if (screenOn && pressed) {
        LOG(INFO) << "Screen on, finger down - enabling HBM";
        mIsScreenOnFod = true;
        mAuthInProgress = true;
        enableHbm();
        if (!enrolling.load()) {
            LOG(DEBUG) << "Not enrolling, scheduling HBM timeout";
            scheduleHbmTimeout(false);
        }
    }
    
    if (pressed) {
        LOG(DEBUG) << "Setting FOD status ON";
        setFodStatus(FOD_STATUS_ON);
    }

    if (!enrolling.load()) {
        LOG(DEBUG) << "Not enrolling, controlling HBM via display";
        std::lock_guard<std::mutex> lock(disp_mutex_);
        if (disp_fd_.get() >= 0) {
            disp_local_hbm_req req;
            req.base.flag = 0;
            req.base.disp_id = MI_DISP_PRIMARY;
            req.local_hbm_value = pressed ? LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT
                                          : LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
            if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) == 0 && !pressed) {
                mHbmEnabled = false;
                LOG(DEBUG) << "HBM disabled via display ioctl";
            } else {
                LOG(ERROR) << "Failed to control HBM via display";
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(device_mutex_);
        if (mDevice != nullptr) {
            int cmd = pressed ? PARAM_FOD_PRESSED : PARAM_FOD_RELEASED;
            LOG(DEBUG) << "Sending FOD press command: " << cmd;
            mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, cmd);
        } else {
            LOG(ERROR) << "Device is null, cannot send FOD press command";
        }
    }
    
    if (!enrolling.load()) {
        int status = pressed ? AUTH_START : AUTH_STOP;
        LOG(DEBUG) << "Setting display FP status to: " << getFingerprintStatusName(status);
        setDispFpStatus(status);
    }
    
    mIsFingerDown = pressed;
    LOG(DEBUG) << "mIsFingerDown set to " << pressed;
    
    if (!pressed) {
        LOG(INFO) << "Finger up - cleaning up";
        mAuthInProgress = false;
        mIsScreenOnFod = false;
        mIsFinalEnrollment = false;
        
        if (mPendingCleanup.load()) {
            LOG(INFO) << "Pending cleanup active during finger up, resetting";
            mPendingCleanup = false;
            enrolling.store(false);
            mSamplesRemaining = 0;
        }
        
        forceHbmCleanup(false);
    }
}

void FpcUdfpsHandler::shutdownThreads() {
    LOG(INFO) << "Shutting down all threads";
    isRunning.store(false);
    
    {
        std::lock_guard<std::mutex> lock(cleanup_mutex_);
        cleanupThreadRunning = false;
        cleanup_cv_.notify_all();
    }
    
    if (cleanupThread_.joinable()) {
        LOG(INFO) << "Joining cleanup thread";
        cleanupThread_.join();
    }
    if (fodThread_.joinable()) {
        LOG(INFO) << "Joining FOD press monitor thread";
        fodThread_.join();
    }
    if (dispThread_.joinable()) {
        LOG(INFO) << "Joining display event monitor thread";
        dispThread_.join();
    }
    if (screenThread_.joinable()) {
        LOG(INFO) << "Joining screen state monitor thread";
        screenThread_.join();
    }
    LOG(INFO) << "All threads shut down successfully";
}

void FpcUdfpsHandler::screenStateMonitorThread() {
    LOG(INFO) << "Screen state monitor thread started";
    int lastState = -1;
    while (isRunning.load()) {
        int brightness = getBrightness();
        if (brightness != -1) {
            int currentState = (brightness == 0) ? 0 : 1;
            bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);

            if (currentState != lastState) {
                LOG(INFO) << "Screen state changed: " << (currentState ? "ON" : "OFF") 
                          << ", brightness: " << brightness
                          << ", screen-off enabled: " << isScreenOffEnabled;
                
                if (currentState == 0 && isScreenOffEnabled) {
                    LOG(INFO) << "Screen off with FOD enabled, setting FOD status ON";
                    setFodStatus(FOD_STATUS_ON);
                } else if (currentState == 1) {
                    LOG(INFO) << "Screen on, waiting 50ms before cleanup check";
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    if (!enrolling.load() && !mAuthInProgress.load() && !mPendingCleanup.load()) {
                        LOG(INFO) << "Screen on and no fingerprint active, forcing cleanup";
                        forceHbmCleanup(false);
                    } else {
                        LOG(DEBUG) << "Screen on but fingerprint active, skipping cleanup";
                    }
                }
                lastState = currentState;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    LOG(INFO) << "Screen state monitor thread stopped";
}

void FpcUdfpsHandler::setFodStatus(int value) {
    int currentState = mLastFodState.load();
    if (value == currentState) {
        LOG(VERBOSE) << "FOD status already " << (value ? "ON" : "OFF") << ", skipping";
        return;
    }
    
    if (value == FOD_STATUS_ON && isScreenOn()) {
        LOG(VERBOSE) << "Screen is on, ignoring FOD ON request";
        return;
    }

    LOG(INFO) << __func__ << " - Setting FOD status from " << (currentState ? "ON" : "OFF") 
              << " to " << (value ? "ON" : "OFF");

    std::lock_guard<std::mutex> lock(touch_mutex_);
    if (touch_fd_.get() < 0) {
        LOG(ERROR) << "Touch device not opened, cannot set FOD status";
        return;
    }

    // Original FOD command
    int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, value};
    if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) == 0) {
        mLastFodState = value;
        LOG(INFO) << "FOD status set to " << (value ? "ON" : "OFF") << " successfully";
    } else {
        LOG(ERROR) << "Failed to set FOD status: " << strerror(errno);
    }

    // INJECTED D2TW RESTORE
    LOG(INFO) << "Restoring Double-Tap to Wake state (D2TW)";
    int d2twBuf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Doubletap_Mode, 1};
    if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &d2twBuf) == 0) {
        LOG(INFO) << "D2TW restored successfully in setFodStatus";
    } else {
        LOG(ERROR) << "Failed to restore D2TW state: " << strerror(errno);
    }
}

void FpcUdfpsHandler::fodPressMonitorThread() {
    LOG(INFO) << "FOD press monitor thread started (PID: " << getpid() << ")";
    
    int fd = -1;
    while (isRunning.load() && fd < 0) {
        fd = open_ts_input();
        if (fd < 0) {
            LOG(INFO) << "Waiting 1 second before retrying touch device...";
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    if (fd < 0) {
        LOG(ERROR) << "Failed to open touch input device, thread exiting";
        return;
    }

    android::base::unique_fd touchFd(fd);
    struct pollfd tsPoll = { .fd = touchFd.get(), .events = POLLIN, .revents = 0 };
    struct input_event ev;
    int consecutiveErrors = 0;
    LOG(INFO) << "Touch input device opened, starting event loop";
    
    while (isRunning.load()) {
        int rc = poll(&tsPoll, 1, 1000);
        if (rc < 0) {
            consecutiveErrors++;
            LOG(ERROR) << "Poll error: " << strerror(errno) << " (consecutive: " << consecutiveErrors << ")";
            if (consecutiveErrors > 10) {
                LOG(ERROR) << "Too many consecutive poll errors, exiting thread";
                break;
            }
            continue;
        }
        consecutiveErrors = 0;
        
        if (rc == 0) {
            LOG(VERBOSE) << "Touch input poll timeout (no event)";
            continue;
        }

        if (tsPoll.revents & POLLIN) {
            ssize_t bytesRead = read(touchFd.get(), &ev, sizeof(struct input_event));
            if (bytesRead < (ssize_t)sizeof(struct input_event)) {
                LOG(ERROR) << "Incomplete read: " << bytesRead << " bytes";
                continue;
            }

            bool screenOn = isScreenOn();
            bool fpActive = isFingerprintActive();
            bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);
            
            if (ev.type == EV_KEY && ev.code == BTN_INFO && !screenOn && !isScreenOffEnabled) {
                LOG(VERBOSE) << "Ignoring touch event - screen off and screen-off disabled";
                continue;
            }
            
            if (ev.type == EV_KEY && ev.code == BTN_INFO) {
                bool pressed = (ev.value == 1);
                LOG(INFO) << "BTN_INFO event: " << (pressed ? "PRESSED" : "RELEASED")
                          << ", screenOn=" << screenOn 
                          << ", fpActive=" << fpActive
                          << ", screenOffEnabled=" << isScreenOffEnabled;
                
                if (!pressed) {
                    if (!mFingerUpSent.load()) {
                        LOG(INFO) << "Finger up not sent yet, sending now";
                        std::lock_guard<std::mutex> lock(touch_mutex_);
                        int bufUp[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, 0};
                        ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &bufUp);
                        mFingerUpSent = true;
                    }
                    setFodStatus(FOD_STATUS_OFF);
                }
                
                if (!screenOn && !isScreenOffEnabled) {
                    LOG(VERBOSE) << "Ignoring event - screen off and screen-off disabled";
                    continue;
                }
                
                if (screenOn && pressed && !fpActive && !mPendingCleanup.load()) {
                    LOG(VERBOSE) << "Ignoring press - screen on but fingerprint not active";
                    continue;
                }
                
                mIsFingerDown = pressed;
                
                if (!pressed && mPendingCleanup.load()) {
                    bool wasFinalEnrollment = mIsFinalEnrollment.load();
                    LOG(INFO) << "Press released with pending cleanup, final enrollment: " << wasFinalEnrollment;
                    mIsFinalEnrollment = false;
                    mPendingCleanup = false;
                    enrolling.store(false);
                    mSamplesRemaining = 0;
                    forceHbmCleanup(wasFinalEnrollment);
                    continue;
                }
                
                if (!screenOn && isScreenOffEnabled) {
                    if (pressed) {
                        LOG(INFO) << "Screen off with FOD enabled - finger down";
                        setFodStatus(FOD_STATUS_ON);
                        enableHbm();
                        std::lock_guard<std::mutex> lock(device_mutex_);
                        if (mDevice != nullptr) {
                            mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, PARAM_FOD_PRESSED);
                        }
                    } else {
                        LOG(INFO) << "Screen off with FOD enabled - finger up";
                        std::lock_guard<std::mutex> lock(device_mutex_);
                        if (mDevice != nullptr) {
                            mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, PARAM_FOD_RELEASED);
                        }
                        forceHbmCleanup(false);
                    }
                    continue;
                }
                
                if (screenOn) {
                    if (pressed) {
                        LOG(INFO) << "Screen on - finger down";
                        setFingerDown(true);
                        if (!enrolling.load()) {
                            scheduleHbmTimeout(false);
                        }
                    } else {
                        LOG(INFO) << "Screen on - finger up";
                        setFingerDown(false);
                        if (!enrolling.load() && !mPendingCleanup.load()) {
                            forceHbmCleanup(false);
                        }
                    }
                }
            }
            
            if (screenOn && ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID && ev.value >= 0) {
                LOG(VERBOSE) << "ABS_MT_TRACKING_ID event - touch started";
                if (fpActive && !mIsFingerDown.load() && !mPendingCleanup.load()) {
                    LOG(INFO) << "Touch detected during fingerprint activity - setting finger down";
                    setFingerDown(true);
                    if (!enrolling.load()) {
                        scheduleHbmTimeout(false);
                    }
                }
            }
            
            if (screenOn && ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID && ev.value == -1) {
                LOG(VERBOSE) << "ABS_MT_TRACKING_ID event - touch ended";
                if (mIsFingerDown.load()) {
                    LOG(INFO) << "Touch ended during fingerprint activity - setting finger up";
                    mIsFingerDown = false;
                    if (mPendingCleanup.load()) {
                        bool wasFinalEnrollment = mIsFinalEnrollment.load();
                        LOG(INFO) << "Pending cleanup during touch end, final enrollment: " << wasFinalEnrollment;
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
    LOG(INFO) << "FOD press monitor thread stopped";
}

void FpcUdfpsHandler::displayEventMonitorThread() {
    LOG(INFO) << "Display event monitor thread started (PID: " << getpid() << ")";
    
    if (disp_fd_.get() < 0) {
        LOG(ERROR) << "Display device not opened, thread exiting";
        return;
    }

    disp_event_req req;
    req.base.flag = 0;
    req.base.disp_id = MI_DISP_PRIMARY;
    req.type = MI_DISP_EVENT_FOD;
    if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_REGISTER_EVENT, &req) == 0) {
        LOG(INFO) << "Registered for display events successfully";
    } else {
        LOG(ERROR) << "Failed to register for display events: " << strerror(errno);
        return;
    }

    struct pollfd dispEventPoll = { .fd = disp_fd_.get(), .events = POLLIN, .revents = 0 };

    while (isRunning.load()) {
        int rc = poll(&dispEventPoll, 1, 1000);
        if (rc <= 0) {
            if (rc < 0) {
                LOG(ERROR) << "Display poll error: " << strerror(errno);
            }
            continue;
        }

        if (dispEventPoll.revents & POLLIN) {
            dispEventPoll.revents = 0;
            disp_event_resp* response = parseDispEvent(disp_fd_.get());
            if (response == nullptr) {
                LOG(ERROR) << "Failed to parse display event response";
                continue;
            }

            if (response->base.type == MI_DISP_EVENT_FOD) {
                int value = response->data[0];
                bool localHbmUiReady = value & LOCAL_HBM_UI_READY;
                LOG(INFO) << "Display event received: value=0x" << std::hex << value << std::dec
                          << ", localHbmUiReady=" << (localHbmUiReady ? "YES" : "NO");
                
                std::lock_guard<std::mutex> deviceLock(device_mutex_);
                if (mDevice != nullptr) {
                    int cmd = localHbmUiReady ? PARAM_NIT_FOD : PARAM_NIT_NONE;
                    LOG(DEBUG) << "Sending NIT command: " << cmd;
                    mDevice->extCmd(mDevice, COMMAND_NIT, cmd);
                } else {
                    LOG(ERROR) << "Device is null, cannot send NIT command";
                }
            } else {
                LOG(WARNING) << "Unexpected display event type: " << response->base.type;
            }
        }
    }
    LOG(INFO) << "Display event monitor thread stopped";
}

const char* FpcUdfpsHandler::getFingerprintStatusName(int status) {
    switch(status) {
        case FINGERPRINT_NONE: return "NONE";
        case AUTH_START: return "AUTH_START";
        case AUTH_STOP: return "AUTH_STOP";
        case ENROLL_START: return "ENROLL_START";
        case ENROLL_STOP: return "ENROLL_STOP";
        default: return "UNKNOWN";
    }
}
