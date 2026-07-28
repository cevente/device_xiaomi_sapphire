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
#include <sys/time.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <sstream>
#include <iomanip>

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

// Helper function to get current timestamp in milliseconds
static uint64_t getTimestampMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

// Helper function to format timestamp for logging
static std::string formatTimestamp(uint64_t ts) {
    time_t sec = ts / 1000;
    uint64_t ms = ts % 1000;
    struct tm* tm = localtime(&sec);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", tm);
    std::ostringstream oss;
    oss << buf << "." << std::setw(3) << std::setfill('0') << ms;
    return oss.str();
}

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
                        LOG(INFO) << "Found touch device: " << name << " at " << absolute_path;
                        break;
                    }
                }

                close(fd);
                fd = -1;
            }
        }
        closedir(dir);
    }
    if (fd < 0) {
        LOG(ERROR) << "Failed to find touch input device";
    }
    return fd;
}

}  // anonymous namespace

class XiaomiSm6225UdfpsHandler : public UdfpsHandler {
  public:
    XiaomiSm6225UdfpsHandler() : mDevice(nullptr), mPendingCleanup(false), 
                                  mHbmStuck(false), mAuthInProgress(false), mIsScreenOnFod(false),
                                  mSamplesRemaining(0), mIsFinalEnrollment(false), mHbmEnabled(false),
                                  isFpcFod(false), mFingerUpSent(false), mLastFodState(-1) {}

    ~XiaomiSm6225UdfpsHandler() override {
        LOG(INFO) << "Destructor called, shutting down threads";
        shutdownThreads();
    }

    void init(fingerprint_device_t* device) override {
        LOG(INFO) << "Initializing UdfpsHandler for xiaomi_sm6225";
        mDevice = device;
        
        touch_fd_ = android::base::unique_fd(open(TOUCH_DEV_PATH, O_RDWR));
        if (touch_fd_.get() >= 0) {
            LOG(INFO) << "Opened touch device: " << TOUCH_DEV_PATH;
        } else {
            LOG(ERROR) << "Failed to open touch device: " << TOUCH_DEV_PATH << " - " << strerror(errno);
        }
        
        disp_fd_ = android::base::unique_fd(open(DISP_FEATURE_PATH, O_RDWR));
        if (disp_fd_.get() >= 0) {
            LOG(INFO) << "Opened display feature device: " << DISP_FEATURE_PATH;
        } else {
            LOG(ERROR) << "Failed to open display feature device: " << DISP_FEATURE_PATH << " - " << strerror(errno);
        }
        
        drm_fd_ = android::base::unique_fd(open(DRM_DEV_PATH, O_RDWR));
        if (drm_fd_.get() >= 0) {
            LOG(INFO) << "Opened DRM device: " << DRM_DEV_PATH;
        } else {
            LOG(ERROR) << "Failed to open DRM device: " << DRM_DEV_PATH << " - " << strerror(errno);
        }

        std::string fpVendor = android::base::GetProperty("persist.vendor.sys.fp.vendor", "none");
        isFpcFod = (fpVendor == "fpc_fod");
        LOG(INFO) << "Fingerprint vendor: " << fpVendor << ", isFpcFod: " << (isFpcFod ? "true" : "false");

        fodThread_ = std::thread([this]() { fodPressMonitorThread(); });
        dispThread_ = std::thread([this]() { displayEventMonitorThread(); });
        
        if (isFpcFod) {
            screenThread_ = std::thread([this]() { screenStateMonitorThread(); });
            LOG(INFO) << "Screen state monitor thread started for FPC FOD";
        }
        
        LOG(INFO) << "UdfpsHandler initialization complete";
    }

    void onFingerDown(uint32_t x, uint32_t y, float minor, float major) override {
        uint64_t ts = getTimestampMs();
        LOG(INFO) << "[" << formatTimestamp(ts) << "] onFingerDown: x=" << x << ", y=" << y 
                  << ", minor=" << minor << ", major=" << major
                  << ", pendingCleanup=" << mPendingCleanup.load();
        
        if (mPendingCleanup.load()) {
            LOG(WARNING) << "[" << formatTimestamp(ts) << "] onFingerDown ignored - cleanup pending";
            return;
        }

        sendEarlyWakeupHint();
        
        mHbmStuck = false;
        mAuthInProgress = true;
        mIsFinalEnrollment = false;
        mFingerUpSent = false;
        
        if (isFpcFod) {
            LOG(INFO) << "[" << formatTimestamp(ts) << "] FPC FOD: setting FOD_STATUS_ON";
            setFodStatus(FOD_STATUS_ON);
        }

        setFingerDown(true);
    }

    void onFingerUp() override {
        uint64_t ts = getTimestampMs();
        LOG(INFO) << "[" << formatTimestamp(ts) << "] onFingerUp: pendingCleanup=" << mPendingCleanup.load()
                  << ", isFinalEnrollment=" << mIsFinalEnrollment.load();
        
        if (mPendingCleanup.load()) {
            bool wasFinalEnrollment = mIsFinalEnrollment.load();
            LOG(INFO) << "[" << formatTimestamp(ts) << "] onFingerUp: cleanup pending, wasFinalEnrollment=" << wasFinalEnrollment;
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
        uint64_t ts = getTimestampMs();
        LOG(WARNING) << "[" << formatTimestamp(ts) << "] onError: error=" << error << ", vendorCode=" << vendorCode;
        
        if (error == 2 || error == 3 || error == 5) {
            LOG(INFO) << "[" << formatTimestamp(ts) << "] onError: cleaning up due to error";
            mAuthInProgress = false;
            mIsFingerDown = false;
            forceHbmCleanup(false);
            setFodStatus(FOD_STATUS_OFF);
        }
        
        setDispFpStatus(FINGERPRINT_NONE);
    }

    void onAcquired(int32_t result, int32_t vendorCode) override {
        uint64_t ts = getTimestampMs();
        auto acquired = static_cast<AcquiredInfo>(result);
        LOG(INFO) << "[" << formatTimestamp(ts) << "] onAcquired: result=" << result 
                  << " (" << static_cast<int>(acquired) << "), vendorCode=" << vendorCode
                  << ", enrolling=" << enrolling.load() << ", pendingCleanup=" << mPendingCleanup.load();
        
        if (acquired == AcquiredInfo::GOOD) {
            if (enrolling.load() && !mPendingCleanup.load()) {
                bool isFinal = (mSamplesRemaining.load() == 0);
                LOG(INFO) << "[" << formatTimestamp(ts) << "] onAcquired GOOD: isFinal=" << isFinal;
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
            
            if (mPendingCleanup.load()) {
                LOG(INFO) << "[" << formatTimestamp(ts) << "] onAcquired GOOD: cleanup pending, ignoring";
                return;
            }
            
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
            
            LOG(WARNING) << "[" << formatTimestamp(ts) << "] onAcquired: bad scan, cleaning up";
            mAuthInProgress = false;
            mIsFingerDown = false;
            forceHbmCleanup(false);
            setDispFpStatus(FINGERPRINT_NONE);
            setFodStatus(FOD_STATUS_OFF);
            
            return;
        }

        if (!isFpcFod && vendorCode == 21) {
            LOG(INFO) << "[" << formatTimestamp(ts) << "] onAcquired: vendorCode 21 - setting FOD_ON";
            setFodStatus(FOD_STATUS_ON);
        } else if (isFpcFod && vendorCode == 22) {
            LOG(INFO) << "[" << formatTimestamp(ts) << "] onAcquired: vendorCode 22 - setting FOD_ON";
            setFodStatus(FOD_STATUS_ON);
        }
        
        if (vendorCode == 23 && mIsFingerDown) {
            LOG(WARNING) << "[" << formatTimestamp(ts) << "] onAcquired: vendorCode 23 - HBM stuck detected";
            if (!mPendingCleanup.load()) {
                mHbmStuck = true;
                forceHbmCleanup(false);
            }
        }
    }

    void onEnrollmentProgress(int32_t enrollmentId, int32_t remaining) override {
        uint64_t ts = getTimestampMs();
        LOG(INFO) << "[" << formatTimestamp(ts) << "] onEnrollmentProgress: enrollmentId=" << enrollmentId 
                  << ", remaining=" << remaining;
        mSamplesRemaining = remaining;
        if (remaining == 0) {
            LOG(INFO) << "[" << formatTimestamp(ts) << "] onEnrollmentProgress: final enrollment step";
            mIsFinalEnrollment = true;
        }
    }

    void preEnroll() override {
        uint64_t ts = getTimestampMs();
        LOG(INFO) << "[" << formatTimestamp(ts) << "] preEnroll called";
        mPendingCleanup = false;
        mHbmStuck = false;
        mSamplesRemaining = 0;
        mIsFinalEnrollment = false;
        enrolling.store(true);
        setDispFpStatus(ENROLL_START);
        LOG(INFO) << "[" << formatTimestamp(ts) << "] preEnroll complete, enrolling=true";
    }

    void enroll() override {
        uint64_t ts = getTimestampMs();
        LOG(INFO) << "[" << formatTimestamp(ts) << "] enroll called";
        enrolling.store(true);
        mSamplesRemaining = 0;
        mIsFinalEnrollment = false;
        setDispFpStatus(ENROLL_START);
        LOG(INFO) << "[" << formatTimestamp(ts) << "] enroll complete, enrolling=true";
    }

    void postEnroll() override {
        uint64_t ts = getTimestampMs();
        LOG(INFO) << "[" << formatTimestamp(ts) << "] postEnroll called";
        setDispFpStatus(ENROLL_STOP);
        enrolling.store(false);
        forceHbmCleanup(false);
        setDispFpStatus(FINGERPRINT_NONE);
        LOG(INFO) << "[" << formatTimestamp(ts) << "] postEnroll complete";
    }

    void cancel() override {
        uint64_t ts = getTimestampMs();
        LOG(INFO) << "[" << formatTimestamp(ts) << "] cancel called";
        enrolling.store(false);
        setDispFpStatus(FINGERPRINT_NONE);
        forceHbmCleanup(false);
        LOG(INFO) << "[" << formatTimestamp(ts) << "] cancel complete";
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
    std::atomic<bool> mFingerUpSent{false};
    std::atomic<int> mLastFodState{-1};
    
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
        bool active = enrolling.load() || mAuthInProgress.load() || mPendingCleanup.load() || mIsFingerDown.load();
        return active;
    }

    void sendEarlyWakeupHint() {
        if (drm_fd_.get() >= 0) {
            drm_msm_display_hint hint = {};
            hint.hint_flags = DRM_MSM_DISPLAY_EARLY_WAKEUP_HINT;
            if (ioctl(drm_fd_.get(), DRM_IOCTL_MSM_DISPLAY_HINT, &hint) == 0) {
                LOG(INFO) << "sendEarlyWakeupHint: success";
            } else {
                LOG(WARNING) << "sendEarlyWakeupHint: failed - " << strerror(errno);
            }
        }
    }

    void setDispFpStatus(int status) {
        uint64_t ts = getTimestampMs();
        LOG(INFO) << "[" << formatTimestamp(ts) << "] setDispFpStatus: status=" << status 
                  << " (" << getFingerprintStatusName(status) << ")";
        
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
                LOG(INFO) << "[" << formatTimestamp(ts) << "] setDispFpStatus: ioctl success";
            } else {
                LOG(WARNING) << "[" << formatTimestamp(ts) << "] setDispFpStatus: ioctl failed - " << strerror(errno);
            }
        } else {
            LOG(WARNING) << "[" << formatTimestamp(ts) << "] setDispFpStatus: disp_fd_ invalid";
        }
    }

    void enableHbm() {
        uint64_t ts = getTimestampMs();
        if (mHbmEnabled.load()) {
            LOG(INFO) << "[" << formatTimestamp(ts) << "] enableHbm: already enabled, skipping";
            return;
        }

        LOG(INFO) << "[" << formatTimestamp(ts) << "] enableHbm: enabling HBM";
        std::lock_guard<std::mutex> lock(disp_mutex_);
        if (disp_fd_.get() >= 0) {
            disp_local_hbm_req req;
            req.base.flag = 0;
            req.base.disp_id = MI_DISP_PRIMARY;
            req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT;
            if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) == 0) {
                mHbmEnabled = true;
                LOG(INFO) << "[" << formatTimestamp(ts) << "] enableHbm: success";
            } else {
                LOG(WARNING) << "[" << formatTimestamp(ts) << "] enableHbm: ioctl failed - " << strerror(errno);
            }
        }
    }

    void disableHbm() {
        uint64_t ts = getTimestampMs();
        if (!mHbmEnabled.load()) {
            LOG(INFO) << "[" << formatTimestamp(ts) << "] disableHbm: already disabled, skipping";
            return;
        }

        LOG(INFO) << "[" << formatTimestamp(ts) << "] disableHbm: disabling HBM";
        std::lock_guard<std::mutex> lock(disp_mutex_);
        if (disp_fd_.get() >= 0) {
            disp_local_hbm_req req;
            req.base.flag = 0;
            req.base.disp_id = MI_DISP_PRIMARY;
            req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
            if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) == 0) {
                mHbmEnabled = false;
                LOG(INFO) << "[" << formatTimestamp(ts) << "] disableHbm: success";
            } else {
                LOG(WARNING) << "[" << formatTimestamp(ts) << "] disableHbm: ioctl failed - " << strerror(errno);
            }
        }
    }

    int getBrightness() {
        android::base::unique_fd fd(open(BRIGHTNESS_PATH, O_RDONLY));
        if (fd.get() < 0) {
            LOG(WARNING) << "getBrightness: failed to open " << BRIGHTNESS_PATH << " - " << strerror(errno);
            return -1;
        }
        char buf[12];
        ssize_t len = read(fd.get(), buf, sizeof(buf) - 1);
        if (len <= 0) {
            LOG(WARNING) << "getBrightness: read failed - " << strerror(errno);
            return -1;
        }
        buf[len] = '\0';
        int brightness = atoi(buf);
        return brightness;
    }

    bool isScreenOn() {
        int brightness = getBrightness();
        bool on = brightness > 0;
        return on;
    }

    void scheduleHbmTimeout(bool isFinalEnrollment = false) {
        uint64_t ts = getTimestampMs();
        LOG(INFO) << "[" << formatTimestamp(ts) << "] scheduleHbmTimeout: isFinalEnrollment=" << isFinalEnrollment;
        
        std::thread oldThread;
        
        {
            std::lock_guard<std::mutex> lock(cleanup_mutex_);
            if (cleanupThread_.joinable()) {
                LOG(INFO) << "[" << formatTimestamp(ts) << "] scheduleHbmTimeout: stopping existing cleanup thread";
                cleanupThreadRunning = false;
                cleanup_cv_.notify_all();
                oldThread = std::move(cleanupThread_);
            }
        }
        
        if (oldThread.joinable()) {
            LOG(INFO) << "[" << formatTimestamp(ts) << "] scheduleHbmTimeout: joining old thread";
            oldThread.join();
        }

        {
            std::lock_guard<std::mutex> lock(cleanup_mutex_);
            cleanupThreadRunning = true;
            
            cleanupThread_ = std::thread([this, isFinalEnrollment]() {
                uint64_t startTs = getTimestampMs();
                LOG(INFO) << "[" << formatTimestamp(startTs) << "] cleanupThread: starting, isFinalEnrollment=" << isFinalEnrollment;
                
                std::unique_lock<std::mutex> threadLock(cleanup_mutex_);
                cleanup_cv_.wait_for(threadLock, std::chrono::milliseconds(300));
                if (!cleanupThreadRunning) {
                    LOG(INFO) << "[" << formatTimestamp(getTimestampMs()) << "] cleanupThread: cancelled";
                    return;
                }
                
                uint64_t wakeTs = getTimestampMs();
                LOG(INFO) << "[" << formatTimestamp(wakeTs) << "] cleanupThread: woke after delay";
                
                if (isFinalEnrollment) {
                    LOG(INFO) << "[" << formatTimestamp(wakeTs) << "] cleanupThread: final enrollment cleanup";
                    mPendingCleanup = false;
                    mIsFinalEnrollment = false;
                    enrolling.store(false);
                    mSamplesRemaining = 0;
                    forceHbmCleanup(true);
                    setDispFpStatus(FINGERPRINT_NONE);
                    return;
                }
                
                if (!mPendingCleanup.load() && !enrolling.load()) {
                    LOG(INFO) << "[" << formatTimestamp(wakeTs) << "] cleanupThread: executing cleanup";
                    forceHbmCleanup(false);
                } else {
                    LOG(INFO) << "[" << formatTimestamp(wakeTs) << "] cleanupThread: skipping cleanup, pendingCleanup=" 
                              << mPendingCleanup.load() << ", enrolling=" << enrolling.load();
                }
            });
        }
    }

    void forceHbmCleanup(bool isFinalEnrollment = false) {
        uint64_t ts = getTimestampMs();
        LOG(INFO) << "[" << formatTimestamp(ts) << "] forceHbmCleanup: isFinalEnrollment=" << isFinalEnrollment
                  << ", mHbmEnabled=" << mHbmEnabled.load() << ", mIsFingerDown=" << mIsFingerDown.load();
        
        // Use the yield-and-kick approach to avoid I2C bus collisions
        directForceFodOff();
        disableHbm();
        
        if (isFinalEnrollment) {
            LOG(INFO) << "[" << formatTimestamp(ts) << "] forceHbmCleanup: final enrollment - resetting FP status";
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
        LOG(INFO) << "[" << formatTimestamp(ts) << "] forceHbmCleanup: resetting state flags";
        mIsFingerDown = false;
        mPendingCleanup = false;
        mHbmStuck = false;
        mAuthInProgress = false;
        mIsScreenOnFod = false;
        mSamplesRemaining = 0;
        mIsFinalEnrollment = false;
        mFingerUpSent = false;
        mLastFodState = -1;
        
        LOG(INFO) << "[" << formatTimestamp(ts) << "] forceHbmCleanup: complete";
    }

    /**
     * Direct Force FOD Off - Yield and Kick Architecture
     * 
     * The fts_ts kernel driver automatically attempts to write reg:1 (FOD OFF) 
     * during the hardware IRQ interrupt when the finger is lifted.
     * Spamming ioctls at the exact same time corrupts the I2C bus transaction.
     * 
     * Step 1: Yield (wait 30ms) to let the kernel IRQ thread finish its hardware write.
     * Step 2: Ensure the state is clean without aggressive loops.
     * Step 3: KICK the IC into Touch_Active_MODE to force the physical matrix switch.
     */
    void directForceFodOff() {
        uint64_t ts = getTimestampMs();
        if (touch_fd_.get() < 0) {
            LOG(ERROR) << "[" << formatTimestamp(ts) << "] directForceFodOff: touch_fd_ invalid";
            return;
        }
        
        LOG(WARNING) << "[" << formatTimestamp(ts) << "] ========== directForceFodOff: START (Yield & Kick) ==========";

        // Step 1: Yield to the kernel's IRQ thread to avoid I2C collisions
        LOG(INFO) << "[" << formatTimestamp(ts) << "] Step 1: Yielding 30ms to kernel IRQ thread...";
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        // Step 2: Ensure THP_FOD_DOWNUP_CTL is released
        {
            std::lock_guard<std::mutex> lock(touch_mutex_);
            int bufUp[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, 0};
            if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &bufUp) == 0) {
                mFingerUpSent = true;
                LOG(INFO) << "[" << formatTimestamp(getTimestampMs()) << "] Step 2: THP_FOD_DOWNUP_CTL released (success)";
            } else {
                LOG(WARNING) << "[" << formatTimestamp(getTimestampMs()) << "] Step 2: THP_FOD_DOWNUP_CTL failed - " << strerror(errno);
            }
        }

        // Step 3: Ensure FOD is OFF (Gentle single command, no loops)
        {
            std::lock_guard<std::mutex> lock(touch_mutex_);
            int bufOff[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, FOD_STATUS_OFF};
            if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &bufOff) == 0) {
                LOG(INFO) << "[" << formatTimestamp(getTimestampMs()) << "] Step 3: FOD OFF confirmed (success)";
            } else {
                LOG(WARNING) << "[" << formatTimestamp(getTimestampMs()) << "] Step 3: FOD OFF failed - " << strerror(errno);
            }
        }
        
        // Step 4: The Kick.
        // Force the IC to switch from the high-res fingerprint scanning matrix 
        // back to the standard UI capacitive matrix.
        {
            std::lock_guard<std::mutex> lock(touch_mutex_);
            int bufActive[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Active_MODE, 1};
            if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &bufActive) == 0) {
                LOG(INFO) << "[" << formatTimestamp(getTimestampMs()) << "] Step 4: Touch_Active_MODE=1 (KICK) success";
            } else {
                LOG(WARNING) << "[" << formatTimestamp(getTimestampMs()) << "] Step 4: Touch_Active_MODE failed - " << strerror(errno);
            }
        }
        
        LOG(INFO) << "[" << formatTimestamp(getTimestampMs()) << "] ========== directForceFodOff: COMPLETE ==========";
    }

    void setFingerDown(bool pressed) {
        uint64_t ts = getTimestampMs();
        LOG(INFO) << "[" << formatTimestamp(ts) << "] setFingerDown: pressed=" << pressed
                  << ", pendingCleanup=" << mPendingCleanup.load()
                  << ", mIsFingerDown=" << mIsFingerDown.load();
        
        if (mPendingCleanup.load()) {
            LOG(WARNING) << "[" << formatTimestamp(ts) << "] setFingerDown: cleanup pending, ignoring";
            return;
        }
        
        bool screenOn = isScreenOn();
        LOG(INFO) << "[" << formatTimestamp(ts) << "] setFingerDown: screenOn=" << screenOn;

        // 1. Notify touch firmware of the physical finger state
        // This is critical - if the firmware thinks the finger is still down,
        // it will keep the gesture scanner active
        {
            std::lock_guard<std::mutex> lock(touch_mutex_);
            if (touch_fd_.get() >= 0) {
                int bufDownUp[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, pressed ? 1 : 0};
                if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &bufDownUp) == 0) {
                    mFingerUpSent = !pressed;
                    LOG(INFO) << "[" << formatTimestamp(getTimestampMs()) << "] setFingerDown: THP_FOD_DOWNUP_CTL=" << (pressed ? "DOWN" : "UP") << " success";
                } else {
                    LOG(WARNING) << "[" << formatTimestamp(getTimestampMs()) << "] setFingerDown: THP_FOD_DOWNUP_CTL failed - " << strerror(errno);
                }
            }
        }
        
        if (screenOn && pressed) {
            LOG(INFO) << "[" << formatTimestamp(ts) << "] setFingerDown: screen on + pressed - enabling HBM and auth";
            mIsScreenOnFod = true;
            mAuthInProgress = true;
            enableHbm();
            if (!enrolling.load()) {
                LOG(INFO) << "[" << formatTimestamp(ts) << "] setFingerDown: not enrolling - scheduling HBM timeout";
                scheduleHbmTimeout(false);
            }
        }
        
        if (pressed) {
            LOG(INFO) << "[" << formatTimestamp(ts) << "] setFingerDown: setting FOD_STATUS_ON";
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
                if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) == 0) {
                    LOG(INFO) << "[" << formatTimestamp(getTimestampMs()) << "] setFingerDown: local HBM set to " 
                              << (pressed ? "WHITE_1000NIT" : "OFF_FINGER_UP");
                    if (!pressed) mHbmEnabled = false;
                } else {
                    LOG(WARNING) << "[" << formatTimestamp(getTimestampMs()) << "] setFingerDown: local HBM ioctl failed - " << strerror(errno);
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(device_mutex_);
            if (mDevice != nullptr) {
                int cmdResult = mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS,
                              pressed ? PARAM_FOD_PRESSED : PARAM_FOD_RELEASED);
                LOG(INFO) << "[" << formatTimestamp(getTimestampMs()) << "] setFingerDown: extCmd result=" << cmdResult;
            } else {
                LOG(WARNING) << "[" << formatTimestamp(getTimestampMs()) << "] setFingerDown: mDevice is null";
            }
        }
        
        if (!enrolling.load()) {
            setDispFpStatus(pressed ? AUTH_START : AUTH_STOP);
        }
        
        mIsFingerDown = pressed;
        LOG(INFO) << "[" << formatTimestamp(getTimestampMs()) << "] setFingerDown: mIsFingerDown=" << mIsFingerDown.load();
        
        if (!pressed) {
            LOG(INFO) << "[" << formatTimestamp(getTimestampMs()) << "] setFingerDown: finger up - cleaning up";
            mAuthInProgress = false;
            mIsScreenOnFod = false;
            mIsFinalEnrollment = false;
            
            if (mPendingCleanup.load()) {
                LOG(INFO) << "[" << formatTimestamp(getTimestampMs()) << "] setFingerDown: cleanup pending during finger up";
                mPendingCleanup = false;
                enrolling.store(false);
                mSamplesRemaining = 0;
            }
            
            forceHbmCleanup(false);
        }
    }

    void shutdownThreads() {
        LOG(INFO) << "shutdownThreads: starting";
        isRunning.store(false);
        
        {
            std::lock_guard<std::mutex> lock(cleanup_mutex_);
            cleanupThreadRunning = false;
            cleanup_cv_.notify_all();
        }
        
        if (cleanupThread_.joinable()) {
            LOG(INFO) << "shutdownThreads: joining cleanupThread";
            cleanupThread_.join();
        }
        if (fodThread_.joinable()) {
            LOG(INFO) << "shutdownThreads: joining fodThread";
            fodThread_.join();
        }
        if (dispThread_.joinable()) {
            LOG(INFO) << "shutdownThreads: joining dispThread";
            dispThread_.join();
        }
        if (screenThread_.joinable()) {
            LOG(INFO) << "shutdownThreads: joining screenThread";
            screenThread_.join();
        }
        LOG(INFO) << "shutdownThreads: complete";
    }

    void screenStateMonitorThread() {
        LOG(INFO) << "screenStateMonitorThread: started";
        int lastState = -1;
        while (isRunning.load()) {
            int brightness = getBrightness();
            if (brightness != -1) {
                int currentState = (brightness == 0) ? 0 : 1;
                bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);

                if (currentState != lastState) {
                    uint64_t ts = getTimestampMs();
                    LOG(INFO) << "[" << formatTimestamp(ts) << "] screenStateMonitor: state changed " << lastState << " -> " << currentState
                              << ", isScreenOffEnabled=" << isScreenOffEnabled;
                    
                    if (currentState == 0 && isFpcFod && isScreenOffEnabled) {
                        LOG(INFO) << "[" << formatTimestamp(ts) << "] screenStateMonitor: screen off - setting FOD_ON";
                        setFodStatus(FOD_STATUS_ON);
                    } else if (currentState == 1 && isFpcFod) {
                        LOG(INFO) << "[" << formatTimestamp(ts) << "] screenStateMonitor: screen on - checking cleanup";
                        // Small delay to let authentication complete if in progress
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        if (!enrolling.load() && !mAuthInProgress.load() && !mPendingCleanup.load()) {
                            LOG(INFO) << "[" << formatTimestamp(getTimestampMs()) << "] screenStateMonitor: forcing cleanup";
                            forceHbmCleanup(false);
                        } else {
                            LOG(INFO) << "[" << formatTimestamp(getTimestampMs()) << "] screenStateMonitor: skipping cleanup - enrolling=" 
                                      << enrolling.load() << ", auth=" << mAuthInProgress.load() 
                                      << ", pending=" << mPendingCleanup.load();
                        }
                    }
                    lastState = currentState;
                }
            } else {
                LOG(WARNING) << "screenStateMonitorThread: failed to get brightness";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        LOG(INFO) << "screenStateMonitorThread: exiting";
    }

    void setFodStatus(int value) {
        uint64_t ts = getTimestampMs();
        if (value == mLastFodState.load()) {
            LOG(INFO) << "[" << formatTimestamp(ts) << "] setFodStatus(" << value << "): state unchanged, skipping";
            return;
        }
        
        if (value == FOD_STATUS_ON && isScreenOn()) {
            LOG(INFO) << "[" << formatTimestamp(ts) << "] setFodStatus(ON): screen is on, skipping";
            return;
        }

        LOG(INFO) << "[" << formatTimestamp(ts) << "] setFodStatus: setting to " << (value ? "ON" : "OFF");
        
        std::lock_guard<std::mutex> lock(touch_mutex_);
        if (touch_fd_.get() < 0) {
            LOG(ERROR) << "[" << formatTimestamp(ts) << "] setFodStatus: touch_fd_ invalid";
            return;
        }

        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, value};
        
        // Simple single attempt - let the yield-and-kick handle the rest
        if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) == 0) {
            mLastFodState = value;
            LOG(INFO) << "[" << formatTimestamp(getTimestampMs()) << "] setFodStatus: ioctl success, state=" << (value ? "ON" : "OFF");
        } else {
            LOG(WARNING) << "[" << formatTimestamp(getTimestampMs()) << "] setFodStatus: ioctl failed - " << strerror(errno);
        }
    }

    void fodPressMonitorThread() {
        LOG(INFO) << "fodPressMonitorThread: started";
        int fd = -1;
        while (isRunning.load() && fd < 0) {
            fd = open_ts_input();
            if (fd < 0) {
                LOG(WARNING) << "fodPressMonitorThread: waiting for touch device...";
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        if (fd < 0) {
            LOG(ERROR) << "fodPressMonitorThread: failed to find touch device, exiting";
            return;
        }

        android::base::unique_fd touchFd(fd);
        struct pollfd tsPoll = { .fd = touchFd.get(), .events = POLLIN, .revents = 0 };
        struct input_event ev;
        int consecutiveErrors = 0;
        
        while (isRunning.load()) {
            int rc = poll(&tsPoll, 1, 1000);
            if (rc < 0) {
                consecutiveErrors++;
                if (consecutiveErrors > 10) {
                    LOG(ERROR) << "fodPressMonitorThread: poll failed " << consecutiveErrors << " times, exiting";
                    break;
                }
                continue;
            }
            consecutiveErrors = 0;
            
            if (rc == 0) continue; // timeout

            if (tsPoll.revents & POLLIN) {
                ssize_t bytesRead = read(touchFd.get(), &ev, sizeof(struct input_event));
                if (bytesRead < (ssize_t)sizeof(struct input_event)) {
                    if (bytesRead < 0) {
                        LOG(WARNING) << "fodPressMonitorThread: read failed - " << strerror(errno);
                    }
                    continue;
                }

                uint64_t ts = getTimestampMs();
                bool screenOn = isScreenOn();
                bool fpActive = isFingerprintActive();
                
                // Log interesting events at INFO level
                if (ev.type == EV_KEY && ev.code == BTN_INFO) {
                    LOG(INFO) << "[" << formatTimestamp(ts) << "] fodPressMonitor: BTN_INFO event, value=" << ev.value
                              << ", screenOn=" << screenOn << ", fpActive=" << fpActive
                              << ", mIsFingerDown=" << mIsFingerDown.load();
                } else if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID) {
                    LOG(INFO) << "[" << formatTimestamp(ts) << "] fodPressMonitor: ABS_MT_TRACKING_ID event, value=" << ev.value
                              << ", screenOn=" << screenOn << ", fpActive=" << fpActive
                              << ", mIsFingerDown=" << mIsFingerDown.load();
                }
                
                if (ev.type == EV_KEY && ev.code == BTN_INFO) {
                    bool pressed = (ev.value == 1);
                    bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);
                    
                    // Always ensure FOD is disabled and finger-up state is sent on finger up
                    if (!pressed) {
                        // Send finger up to firmware if not already sent
                        if (!mFingerUpSent.load()) {
                            LOG(INFO) << "[" << formatTimestamp(ts) << "] fodPressMonitor: sending THP_FOD_DOWNUP_CTL=0 (finger up)";
                            std::lock_guard<std::mutex> lock(touch_mutex_);
                            int bufUp[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, 0};
                            ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &bufUp);
                            mFingerUpSent = true;
                        }
                        setFodStatus(FOD_STATUS_OFF);
                    }
                    
                    if (!screenOn && !isScreenOffEnabled) {
                        LOG(INFO) << "[" << formatTimestamp(ts) << "] fodPressMonitor: screen off and screen_off disabled, ignoring";
                        continue;
                    }
                    
                    if (screenOn && pressed && !fpActive && !mPendingCleanup.load()) {
                        LOG(INFO) << "[" << formatTimestamp(ts) << "] fodPressMonitor: screen on, pressed, but no FP active - ignoring";
                        continue;
                    }
                    
                    mIsFingerDown = pressed;
                    
                    if (!pressed && mPendingCleanup.load()) {
                        LOG(INFO) << "[" << formatTimestamp(ts) << "] fodPressMonitor: finger up with cleanup pending";
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
                            LOG(INFO) << "[" << formatTimestamp(ts) << "] fodPressMonitor: screen off FOD press";
                            setFodStatus(FOD_STATUS_ON);
                            enableHbm();
                            std::lock_guard<std::mutex> lock(device_mutex_);
                            if (mDevice != nullptr) mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, PARAM_FOD_PRESSED);
                        } else {
                            LOG(INFO) << "[" << formatTimestamp(ts) << "] fodPressMonitor: screen off FOD release";
                            std::lock_guard<std::mutex> lock(device_mutex_);
                            if (mDevice != nullptr) mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, PARAM_FOD_RELEASED);
                            forceHbmCleanup(false);
                        }
                        continue;
                    }
                    
                    if (screenOn) {
                        if (pressed) {
                            LOG(INFO) << "[" << formatTimestamp(ts) << "] fodPressMonitor: screen on FOD press";
                            setFingerDown(true);
                            if (!enrolling.load()) scheduleHbmTimeout(false);
                        } else {
                            LOG(INFO) << "[" << formatTimestamp(ts) << "] fodPressMonitor: screen on FOD release";
                            setFingerDown(false);
                            if (!enrolling.load() && !mPendingCleanup.load()) {
                                forceHbmCleanup(false);
                            }
                        }
                    }
                }
                
                if (screenOn && ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID && ev.value >= 0) {
                    if (fpActive && !mIsFingerDown.load() && !mPendingCleanup.load()) {
                        LOG(INFO) << "[" << formatTimestamp(ts) << "] fodPressMonitor: ABS_MT_TRACKING_ID touch start (finger down)";
                        setFingerDown(true);
                        if (!enrolling.load()) scheduleHbmTimeout(false);
                    }
                }
                
                if (screenOn && ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID && ev.value == -1) {
                    if (mIsFingerDown.load()) {
                        LOG(INFO) << "[" << formatTimestamp(ts) << "] fodPressMonitor: ABS_MT_TRACKING_ID touch end (finger up)";
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
        LOG(INFO) << "fodPressMonitorThread: exiting";
    }

    void displayEventMonitorThread() {
        LOG(INFO) << "displayEventMonitorThread: started";
        if (disp_fd_.get() < 0) {
            LOG(ERROR) << "displayEventMonitorThread: disp_fd_ invalid, exiting";
            return;
        }

        disp_event_req req;
        req.base.flag = 0;
        req.base.disp_id = MI_DISP_PRIMARY;
        req.type = MI_DISP_EVENT_FOD;
        if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_REGISTER_EVENT, &req) == 0) {
            LOG(INFO) << "displayEventMonitorThread: registered for FOD events";
        } else {
            LOG(WARNING) << "displayEventMonitorThread: failed to register FOD events - " << strerror(errno);
        }

        struct pollfd dispEventPoll = { .fd = disp_fd_.get(), .events = POLLIN, .revents = 0 };

        while (isRunning.load()) {
            int rc = poll(&dispEventPoll, 1, 1000);
            if (rc < 0) {
                LOG(WARNING) << "displayEventMonitorThread: poll failed - " << strerror(errno);
                continue;
            }
            if (rc == 0) continue;

            if (dispEventPoll.revents & POLLIN) {
                dispEventPoll.revents = 0;
                disp_event_resp* response = parseDispEvent(disp_fd_.get());
                if (response == nullptr) {
                    LOG(WARNING) << "displayEventMonitorThread: failed to parse display event";
                    continue;
                }

                if (response->base.type == MI_DISP_EVENT_FOD) {
                    int value = response->data[0];
                    bool localHbmUiReady = value & LOCAL_HBM_UI_READY;
                    uint64_t ts = getTimestampMs();
                    LOG(INFO) << "[" << formatTimestamp(ts) << "] displayEventMonitor: FOD event, value=" << value 
                              << ", localHbmUiReady=" << localHbmUiReady;
                    
                    std::lock_guard<std::mutex> deviceLock(device_mutex_);
                    if (mDevice != nullptr) {
                        int cmdResult = mDevice->extCmd(mDevice, COMMAND_NIT,
                                      localHbmUiReady ? PARAM_NIT_FOD : PARAM_NIT_NONE);
                        LOG(INFO) << "[" << formatTimestamp(getTimestampMs()) << "] displayEventMonitor: extCmd NIT result=" << cmdResult;
                    }
                }
            }
        }
        LOG(INFO) << "displayEventMonitorThread: exiting";
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
    LOG(INFO) << "UdfpsHandlerFactory: create called";
    return new XiaomiSm6225UdfpsHandler();
}

static void destroy(UdfpsHandler* handler) {
    LOG(INFO) << "UdfpsHandlerFactory: destroy called";
    delete handler;
}

extern "C" UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
    .create = create,
    .destroy = destroy,
};
