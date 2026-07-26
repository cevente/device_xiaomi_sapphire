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

#include <display/drm/mi_disp.h>

#include "UdfpsHandler.h"
#include "xiaomi_touch.h"

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

using ::aidl::android::hardware::biometrics::fingerprint::AcquiredInfo;

namespace {

static bool readBool(int fd) {
    char c;
    int rc;

    rc = pread(fd, &c, sizeof(char), 0);
    if (rc != 1) {
        LOG(ERROR) << "failed to read bool from fd, err: " << rc;
        return false;
    }

    return c != '0';
}

static disp_event_resp* parseDispEvent(int fd) {
    static char event_data[1024];
    ssize_t size;

    memset(event_data, 0, sizeof(event_data));
    size = read(fd, event_data, sizeof(event_data));
    if (size < 0) {
        LOG(ERROR) << "read fod event failed";
        return nullptr;
    }

    if (size < (ssize_t)sizeof(struct disp_event)) {
        LOG(ERROR) << "Invalid event size " << size << ", expect at least "
                   << sizeof(struct disp_event);
        return nullptr;
    }

    return reinterpret_cast<disp_event_resp*>(event_data);
}

static int open_ts_input() {
    int fd = -1;
    DIR *dir = opendir("/dev/input");

    if (dir != NULL) {
        struct dirent *ent;

        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_type == DT_CHR) {
                char absolute_path[PATH_MAX] = {0};
                char name[80] = {0};

                strcpy(absolute_path, "/dev/input/");
                strcat(absolute_path, ent->d_name);

                fd = open(absolute_path, O_RDWR);
                if (fd < 0) {
                    continue;
                }

                if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), &name) > 0) {
                    if (strcmp(name, "fts_ts") == 0 || strcmp(name, "fts") == 0 || 
                        strcmp(name, "goodix_ts") == 0 || strcmp(name, "NVTCapacitiveTouchScreen") == 0) {
                        LOG(INFO) << "Found touchscreen: " << name << " at " << absolute_path;
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
    XiaomiSm6225UdfpsHandler() : mDevice(nullptr), isFpcFod(false), mPendingCleanup(false), 
                                  mHbmStuck(false), mAuthInProgress(false), mIsScreenOnFod(false),
                                  mSamplesRemaining(0), mIsFinalEnrollment(false), mHbmEnabled(false) {}

    ~XiaomiSm6225UdfpsHandler() {
        LOG(INFO) << "Destructor called, shutting down threads";
        shutdownThreads();
    }

    void init(fingerprint_device_t* device) {
        LOG(INFO) << "Initializing UDFPS handler";
        
        mDevice = device;
        
        touch_fd_ = android::base::unique_fd(open(TOUCH_DEV_PATH, O_RDWR));
        if (touch_fd_.get() < 0) {
            LOG(ERROR) << "Failed to open touch device: " << strerror(errno);
        }

        disp_fd_ = android::base::unique_fd(open(DISP_FEATURE_PATH, O_RDWR));
        if (disp_fd_.get() < 0) {
            LOG(ERROR) << "Failed to open display device: " << strerror(errno);
        }

        std::string fpVendor = android::base::GetProperty("persist.vendor.sys.fp.vendor", "none");
        LOG(INFO) << "Fingerprint vendor: " << fpVendor;
        isFpcFod = (fpVendor == "fpc_fod");

        fodThread_ = std::thread([this]() { fodPressMonitorThread(); });
        dispThread_ = std::thread([this]() { displayEventMonitorThread(); });
        
        if (isFpcFod) {
            screenThread_ = std::thread([this]() { screenStateMonitorThread(); });
        }

        LOG(INFO) << "UDFPS handler initialized";
    }

    void onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) {
        LOG(INFO) << __func__ << " - Framework pointer DOWN";
        
        if (mPendingCleanup.load()) {
            LOG(INFO) << "⏳ Deferred cleanup pending - ignoring finger DOWN";
            return;
        }
        
        mHbmStuck = false;
        mAuthInProgress = true;
        mIsFinalEnrollment = false;
        
        mFbDownTimeMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

        if (isFpcFod) {
            setFodStatus(FOD_STATUS_ON);
        }

        setFingerDown(true);
    }

    void onFingerUp() {
        LOG(INFO) << __func__ << " - Framework pointer UP";
        
        if (mPendingCleanup.load()) {
            LOG(INFO) << "💡 Framework finger UP during deferred cleanup - forcing cleanup";
            bool wasFinalEnrollment = mIsFinalEnrollment.load();
            mIsFinalEnrollment = false;
            mPendingCleanup = false;
            enrolling.store(false);
            mSamplesRemaining = 0;
            mIsFingerDown = false;
            forceHbmCleanup(wasFinalEnrollment);
            setFodStatus(FOD_STATUS_OFF);
            resetTouchState();
            return;
        }
        
        setFingerDown(false);
        
        if (!mPendingCleanup.load()) {
            mAuthInProgress = false;
            mIsScreenOnFod = false;
            mHbmStuck = false;
            mIsFinalEnrollment = false;
            
            if (!enrolling.load()) {
                setFodStatus(FOD_STATUS_OFF);
                resetTouchState();
            }
        }
    }

    void onAcquired(int32_t result, int32_t vendorCode) {
        LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;
        
        if (static_cast<AcquiredInfo>(result) == AcquiredInfo::GOOD) {
            LOG(INFO) << "✅ Acquisition GOOD";
            
            if (enrolling.load() && !mPendingCleanup.load()) {
                LOG(INFO) << "📝 Enrollment capture GOOD - turning off HBM and disabling FOD";
                bool isFinal = (mSamplesRemaining.load() == 0);
                if (isFinal) {
                    LOG(INFO) << "📝 Final enrollment capture - will use 270ms timeout for cleanup";
                    if (mIsFingerDown.load()) {
                        scheduleHbmTimeout(true);
                    }
                    return;
                } else {
                    disableHbm();
                    forceHbmCleanup(false);
                    setFodStatus(FOD_STATUS_OFF);
                    resetTouchState();
                    setDispFpStatus(ENROLL_STOP);
                    mAuthInProgress = false;
                    mIsScreenOnFod = false;
                    return;
                }
            }
            
            if (mPendingCleanup.load()) {
                LOG(INFO) << "⏳ Deferred cleanup already pending - ignoring GOOD acquisition";
                return;
            }
            
            mAuthInProgress = false;
            mIsScreenOnFod = false;
            setFingerDown(false);
            mPendingCleanup = false;
            mHbmStuck = false;
            mIsFinalEnrollment = false;
            
            disableHbm();
            forceHbmCleanup(false);
            setFodStatus(FOD_STATUS_OFF);
            resetTouchState();
            
            if (!enrolling.load()) {
                setDispFpStatus(FINGERPRINT_NONE);
            }
            return;
        }

        if (!isFpcFod && vendorCode == 21) {
            setFodStatus(FOD_STATUS_ON);
        } else if (isFpcFod && vendorCode == 22) {
            setFodStatus(FOD_STATUS_ON);
        }
        
        if (vendorCode == 23 && mIsFingerDown) {
            if (!mPendingCleanup.load()) {
                LOG(INFO) << "⚠️ HBM killed while finger is still down - re-enabling";
                mHbmStuck = true;
            } else {
                LOG(INFO) << "⏳ Deferred cleanup pending - ignoring HBM kill event";
            }
        }
    }

    void onEnrollmentProgress(int32_t enrollmentId, int32_t remaining) override {
        LOG(INFO) << __func__ << " enrollmentId: " << enrollmentId 
                  << " remaining: " << remaining;
        
        mSamplesRemaining = remaining;
        
        if (remaining == 0) {
            LOG(INFO) << "📝 FINAL enrollment scan - samples_remaining = 0";
            mIsFinalEnrollment = true;
            if (mIsFingerDown.load()) {
                LOG(INFO) << "📝 Finger still down - will use 270ms timeout for cleanup";
            }
        } else {
            LOG(INFO) << "📝 Enrollment scan " << enrollmentId 
                      << " - " << remaining << " scans remaining";
        }
    }

    void preEnroll() {
        LOG(INFO) << __func__;
        mPendingCleanup = false;
        mHbmStuck = false;
        mSamplesRemaining = 0;
        mIsFinalEnrollment = false;
        enrolling.store(true);
        setDispFpStatus(ENROLL_START);
    }

    void enroll() {
        LOG(INFO) << __func__;
        enrolling.store(true);
        mSamplesRemaining = 0;
        mIsFinalEnrollment = false;
        setDispFpStatus(ENROLL_START);
    }

    void postEnroll() {
        LOG(INFO) << __func__;
        setDispFpStatus(ENROLL_STOP);
        enrolling.store(false);
        mPendingCleanup = false;
        mHbmStuck = false;
        mSamplesRemaining = 0;
        mIsFinalEnrollment = false;
        disableHbm();
        setFodStatus(FOD_STATUS_OFF);
        resetTouchState();
        setDispFpStatus(FINGERPRINT_NONE);
    }

    void cancel() {
        LOG(INFO) << __func__;
        enrolling.store(false);
        mAuthInProgress = false;
        mIsScreenOnFod = false;
        mPendingCleanup = false;
        mSamplesRemaining = 0;
        mIsFinalEnrollment = false;
        mIsFingerDown = false;
        mHbmStuck = false;
        setDispFpStatus(FINGERPRINT_NONE);
        disableHbm();
        setFodStatus(FOD_STATUS_OFF);
        resetTouchState();
    }

  private:
    fingerprint_device_t* mDevice;
    android::base::unique_fd touch_fd_;
    android::base::unique_fd disp_fd_;
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
    
    bool isFpcFod;
    
    std::atomic<uint64_t> mFbDownTimeMs{0};

    std::mutex touch_mutex_;
    std::mutex disp_mutex_;
    std::mutex device_mutex_;
    std::mutex cleanup_mutex_;

    std::thread fodThread_;
    std::thread dispThread_;
    std::thread screenThread_;
    std::thread cleanupThread_;

    bool isFingerprintActive() {
        return enrolling.load() || 
               mAuthInProgress.load() || 
               mPendingCleanup.load() ||
               mIsFingerDown.load();
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
            
            if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_FEATURE, &fp_req) < 0) {
                LOG(ERROR) << "Failed to set display FP status (" << status << "): " << strerror(errno);
            } else {
                LOG(INFO) << "✅ Display FP status synced: " << status;
            }
        }
    }

    void enableHbm() {
        if (mHbmEnabled.load()) {
            return;
        }
        
        std::lock_guard<std::mutex> lock(disp_mutex_);
        if (disp_fd_.get() >= 0) {
            disp_local_hbm_req req;
            req.base.flag = 0;
            req.base.disp_id = MI_DISP_PRIMARY;
            req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT;
            if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) < 0) {
                LOG(ERROR) << "Failed to enable HBM: " << strerror(errno);
            } else {
                mHbmEnabled = true;
                LOG(INFO) << "✅ HBM enabled";
            }
        }
    }

    void disableHbm() {
        if (!mHbmEnabled.load()) {
            return;
        }
        
        std::lock_guard<std::mutex> lock(disp_mutex_);
        if (disp_fd_.get() >= 0) {
            disp_local_hbm_req req;
            req.base.flag = 0;
            req.base.disp_id = MI_DISP_PRIMARY;
            req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
            if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) < 0) {
                LOG(ERROR) << "Failed to disable HBM: " << strerror(errno);
            } else {
                mHbmEnabled = false;
                LOG(INFO) << "✅ HBM disabled";
            }
        }
    }

    void resetTouchState() {
        std::lock_guard<std::mutex> lock(touch_mutex_);
        if (touch_fd_.get() >= 0) {
            int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, 0};
            if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
                LOG(ERROR) << "Failed to reset touch state: " << strerror(errno);
            } else {
                LOG(INFO) << "✅ Touch state reset (FOD disabled)";
            }
        }
    }

    void scheduleHbmTimeout(bool isFinalEnrollment = false) {
        std::lock_guard<std::mutex> lock(cleanup_mutex_);
        if (cleanupThread_.joinable()) {
            cleanupThread_.join();
        }
        cleanupThread_ = std::thread([this, isFinalEnrollment]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(270));
            
            if (isFinalEnrollment) {
                LOG(INFO) << "⏰ Final enrollment timeout (270ms) - turning off HBM and disabling FOD";
                mPendingCleanup = false;
                mIsFinalEnrollment = false;
                enrolling.store(false);
                mSamplesRemaining = 0;
                mIsFingerDown = false;
                mAuthInProgress = false;
                mIsScreenOnFod = false;
                
                disableHbm();
                forceHbmCleanup(true);
                setFodStatus(FOD_STATUS_OFF);
                resetTouchState();
                setDispFpStatus(FINGERPRINT_NONE);
                return;
            }
            
            if (!mPendingCleanup.load() && !enrolling.load()) {
                LOG(INFO) << "⏰ HBM timeout (270ms) - turning off HBM and disabling FOD";
                
                disableHbm();
                setFodStatus(FOD_STATUS_OFF);
                resetTouchState();
                mIsFingerDown = false;
                mAuthInProgress = false;
                mIsScreenOnFod = false;
                
            } else if (enrolling.load()) {
                LOG(INFO) << "⏰ HBM timeout - enrollment active, waiting for onAcquired(GOOD)";
            } else if (mPendingCleanup.load()) {
                LOG(INFO) << "⏰ HBM timeout - deferred cleanup pending, keeping HBM on";
            }
        });
    }

    void scheduleHbmCleanup() {
        std::lock_guard<std::mutex> lock(cleanup_mutex_);
        if (cleanupThread_.joinable()) {
            cleanupThread_.join();
        }
        cleanupThread_ = std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(270));
            if (mHbmStuck.load() && mIsFingerDown.load()) {
                LOG(INFO) << "💡 Force cleaning HBM stuck state";
                disableHbm();
                forceHbmCleanup(false);
                mHbmStuck = false;
            }
        });
    }

    void forceHbmCleanup(bool isFinalEnrollment = false) {
        LOG(INFO) << "Forcing HBM cleanup - complete state reset (final=" << isFinalEnrollment << ")";
        
        disableHbm();
        resetTouchState();
        setFodStatus(FOD_STATUS_OFF);
        
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
                
                if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_FEATURE, &fp_req) < 0) {
                    LOG(ERROR) << "Failed to reset FP status: " << strerror(errno);
                } else {
                    LOG(INFO) << "✅ FP display status reset to FINGERPRINT_NONE (final enrollment)";
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
        
        LOG(INFO) << "✅ Complete cleanup performed - FOD disabled";
    }

    void forceCleanupIfPressed() {
        bool pressed = mIsFingerDown.load();

        if (pressed) {
            LOG(INFO) << "UDFPS: Finger held during enrollment finish. Deferred until pointer UP.";
            if (!mPendingCleanup.load()) {
                mPendingCleanup = true;
                mIsFinalEnrollment = true;
            }
        } else {
            enrolling.store(false);
            bool wasFinalEnrollment = mIsFinalEnrollment.load();
            mIsFinalEnrollment = false;
            mSamplesRemaining = 0;
            setFingerDown(false);
            mPendingCleanup = false;
            mHbmStuck = false;
            setFodStatus(FOD_STATUS_OFF);
            resetTouchState();
        }
    }

    int getBrightness() {
        int fd = open(BRIGHTNESS_PATH, O_RDONLY);
        if (fd < 0) return -1;
        char buf[12];
        ssize_t len = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (len <= 0) return -1;
        buf[len] = '\0';
        return atoi(buf);
    }

    bool isScreenOn() {
        return getBrightness() > 0;
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
                        if (!enrolling.load() && !mPendingCleanup && !mAuthInProgress.load()) {
                            setFodStatus(FOD_STATUS_OFF);
                            resetTouchState();
                        }
                    }
                    lastState = currentState;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    void shutdownThreads() {
        isRunning.store(false);
        if (fodThread_.joinable()) {
            fodThread_.join();
        }
        if (dispThread_.joinable()) {
            dispThread_.join();
        }
        if (screenThread_.joinable()) {
            screenThread_.join();
        }
        if (cleanupThread_.joinable()) {
            cleanupThread_.join();
        }
    }

    void fodPressMonitorThread() {
        LOG(INFO) << "Native input event monitor thread started";
        
        int fd = -1;
        while (isRunning.load() && fd < 0) {
            fd = open_ts_input();
            if (fd < 0) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        if (fd < 0) {
            LOG(ERROR) << "Failed to find touchscreen input device";
            return;
        }

        struct pollfd tsPoll = {
            .fd = fd,
            .events = POLLIN,
            .revents = 0,
        };

        struct input_event ev;
        
        while (isRunning.load()) {
            int rc = poll(&tsPoll, 1, 1000);
            
            if (rc < 0) {
                if (errno == EINTR) continue;
                LOG(ERROR) << "Input poll failed: " << strerror(errno);
                break;
            }

            if (rc == 0) continue;

            if (tsPoll.revents & POLLIN) {
                ssize_t bytesRead = read(fd, &ev, sizeof(struct input_event));
                
                if (bytesRead < (ssize_t)sizeof(struct input_event)) {
                    continue;
                }

                bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);
                bool screenOn = isScreenOn();
                bool fpActive = isFingerprintActive();
                
                if (ev.type == EV_KEY && ev.code == 0x0152) {
                    bool pressed = (ev.value == 1);
                    
                    if (!isScreenOffEnabled && !screenOn) {
                        continue;
                    }
                    
                    if (!fpActive && !mPendingCleanup.load()) {
                        continue;
                    }
                    
                    mIsFingerDown = pressed;
                    
                    if (!pressed && mPendingCleanup.load()) {
                        LOG(INFO) << "💡 FORCED: Finger UP during deferred cleanup - cleaning up immediately";
                        bool wasFinalEnrollment = mIsFinalEnrollment.load();
                        mIsFinalEnrollment = false;
                        mPendingCleanup = false;
                        enrolling.store(false);
                        mSamplesRemaining = 0;
                        forceHbmCleanup(wasFinalEnrollment);
                        setFodStatus(FOD_STATUS_OFF);
                        resetTouchState();
                        continue;
                    }
                    
                    if (!screenOn && isScreenOffEnabled) {
                        if (pressed) {
                            LOG(INFO) << "📱 Screen-off FOD: DOWN detected - enabling HBM";
                            setFodStatus(FOD_STATUS_ON);
                            enableHbm();
                            if (!enrolling.load()) {
                                scheduleHbmTimeout(false);
                            } else {
                                LOG(INFO) << "📱 Screen-off FOD: Enrollment active - keeping HBM on";
                            }
                            std::lock_guard<std::mutex> lock(device_mutex_);
                            if (mDevice != nullptr) {
                                mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, PARAM_FOD_PRESSED);
                            }
                        } else {
                            LOG(INFO) << "📱 Screen-off FOD: UP detected - cleaning up";
                            setFodStatus(FOD_STATUS_OFF);
                            std::lock_guard<std::mutex> lock(device_mutex_);
                            if (mDevice != nullptr) {
                                mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, PARAM_FOD_RELEASED);
                            }
                            disableHbm();
                            forceHbmCleanup(false);
                            resetTouchState();
                        }
                        continue;
                    }
                    
                    if (screenOn) {
                        if (pressed) {
                            LOG(INFO) << "📱 Screen-on FOD: DOWN detected - enabling HBM";
                            setFingerDown(true);
                            if (!enrolling.load()) {
                                scheduleHbmTimeout(false);
                            }
                        } else {
                            LOG(INFO) << "📱 Screen-on FOD: UP detected - cleaning up";
                            setFingerDown(false);
                            if (!enrolling.load() && !mPendingCleanup.load()) {
                                setFodStatus(FOD_STATUS_OFF);
                                resetTouchState();
                            }
                        }
                    }
                }
                
                if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID && ev.value >= 0) {
                    bool screenOn = isScreenOn();
                    bool fpActive = isFingerprintActive();
                    
                    if (screenOn && fpActive && !mIsFingerDown.load() && !mPendingCleanup.load()) {
                        LOG(INFO) << "📱 Initial touch detected - enabling HBM";
                        setFingerDown(true);
                        if (!enrolling.load()) {
                            scheduleHbmTimeout(false);
                        }
                    }
                }
                
                if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID && ev.value == -1) {
                    if (mIsFingerDown.load()) {
                        mIsFingerDown = false;
                        
                        if (mPendingCleanup.load()) {
                            LOG(INFO) << "💡 Touch release during deferred cleanup - forcing cleanup";
                            bool wasFinalEnrollment = mIsFinalEnrollment.load();
                            mIsFinalEnrollment = false;
                            mPendingCleanup = false;
                            enrolling.store(false);
                            mSamplesRemaining = 0;
                            forceHbmCleanup(wasFinalEnrollment);
                            setFodStatus(FOD_STATUS_OFF);
                            resetTouchState();
                        } else {
                            setFingerDown(false);
                            if (!enrolling.load() && !mPendingCleanup.load()) {
                                setFodStatus(FOD_STATUS_OFF);
                                resetTouchState();
                            }
                        }
                    }
                }
            }
        }

        close(fd);
        LOG(INFO) << "Native input event monitor thread stopped";
    }

    void displayEventMonitorThread() {
        LOG(INFO) << "Display event monitor thread started";
        
        int fd = open(DISP_FEATURE_PATH, O_RDWR);
        if (fd < 0) {
            LOG(ERROR) << "Failed to open " << DISP_FEATURE_PATH 
                       << ", error: " << strerror(errno);
            return;
        }

        disp_event_req req;
        req.base.flag = 0;
        req.base.disp_id = MI_DISP_PRIMARY;
        req.type = MI_DISP_EVENT_FOD;
        if (ioctl(fd, MI_DISP_IOCTL_REGISTER_EVENT, &req) < 0) {
            LOG(ERROR) << "Failed to register for display events: " << strerror(errno);
            close(fd);
            return;
        }

        struct pollfd dispEventPoll = {
            .fd = fd,
            .events = POLLIN,
            .revents = 0,
        };

        while (isRunning.load()) {
            int rc = poll(&dispEventPoll, 1, 1000);
            
            if (rc < 0) {
                if (errno == EINTR) continue;
                LOG(ERROR) << "Display poll failed: " << strerror(errno);
                break;
            }

            if (rc == 0) continue;

            if (!(dispEventPoll.revents & POLLIN)) {
                if (dispEventPoll.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    LOG(ERROR) << "Display poll error: " << dispEventPoll.revents;
                    break;
                }
                dispEventPoll.revents = 0;
                continue;
            }

            dispEventPoll.revents = 0;

            disp_event_resp* response = parseDispEvent(fd);
            if (response == nullptr) {
                continue;
            }

            if (response->base.type != MI_DISP_EVENT_FOD) {
                LOG(WARNING) << "Unexpected display event: " << response->base.type;
                continue;
            }

            int value = response->data[0];

            bool localHbmUiReady = value & LOCAL_HBM_UI_READY;
            
            std::lock_guard<std::mutex> deviceLock(device_mutex_);
            if (mDevice != nullptr) {
                mDevice->extCmd(mDevice, COMMAND_NIT,
                              localHbmUiReady ? PARAM_NIT_FOD : PARAM_NIT_NONE);
            }
        }

        close(fd);
        LOG(INFO) << "Display event monitor thread stopped";
    }

    void setFodStatus(int value) {
        std::lock_guard<std::mutex> lock(touch_mutex_);
        
        if (touch_fd_.get() < 0) {
            LOG(ERROR) << "Touch device not opened";
            return;
        }

        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, value};
        if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
            LOG(ERROR) << "Failed to set FOD status: " << strerror(errno);
        }
    }

    void setFingerDown(bool pressed) {
        if (!isFingerprintActive() && !mPendingCleanup.load()) {
            return;
        }
        
        if (mPendingCleanup.load()) {
            if (pressed) {
                LOG(INFO) << "⏳ Deferred cleanup pending - ignoring finger DOWN";
                return;
            } else {
                LOG(INFO) << "💡 Deferred cleanup pending - processing finger UP";
                
                bool wasFinalEnrollment = mIsFinalEnrollment.load();
                mIsFingerDown = false;
                mAuthInProgress = false;
                mIsScreenOnFod = false;
                mIsFinalEnrollment = false;
                mPendingCleanup = false;
                enrolling.store(false);
                mSamplesRemaining = 0;
                mHbmStuck = false;
                
                forceHbmCleanup(wasFinalEnrollment);
                setFodStatus(FOD_STATUS_OFF);
                resetTouchState();
                setDispFpStatus(FINGERPRINT_NONE);
                return;
            }
        }
        
        bool screenOn = isScreenOn();
        
        if (screenOn && pressed) {
            mIsScreenOnFod = true;
            mAuthInProgress = true;
            LOG(INFO) << "Screen ON FOD: DOWN - enabling HBM";
            
            enableHbm();
            
            if (!enrolling.load()) {
                scheduleHbmTimeout(false);
            }
        }
        
        {
            std::lock_guard<std::mutex> lock(touch_mutex_);
            if (touch_fd_.get() >= 0) {
                int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, pressed ? 1 : 0};
                if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
                    LOG(ERROR) << "Failed to set finger down: " << strerror(errno);
                }
            }
        }

        if (!enrolling.load()) {
            std::lock_guard<std::mutex> lock(disp_mutex_);
            if (disp_fd_.get() >= 0) {
                disp_local_hbm_req req;
                req.base.flag = 0;
                req.base.disp_id = MI_DISP_PRIMARY;
                req.local_hbm_value = pressed ? LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT
                                              : LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
                if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) < 0) {
                    LOG(ERROR) << "Failed to set HBM: " << strerror(errno);
                } else if (!pressed) {
                    mHbmEnabled = false;
                    LOG(INFO) << "✅ HBM turned OFF on finger UP";
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
        
        if (!enrolling.load()) {
            setDispFpStatus(pressed ? AUTH_START : AUTH_STOP);
        }
        
        mIsFingerDown = pressed;
        
        if (!pressed) {
            mAuthInProgress = false;
            mIsScreenOnFod = false;
            
            if (mPendingCleanup.load()) {
                LOG(INFO) << "💡 Deferred cleanup completed on finger UP - clearing state";
                bool wasFinalEnrollment = mIsFinalEnrollment.load();
                mIsFinalEnrollment = false;
                mPendingCleanup = false;
                enrolling.store(false);
                mSamplesRemaining = 0;
                forceHbmCleanup(wasFinalEnrollment);
            } else {
                mIsFinalEnrollment = false;
                forceHbmCleanup(false);
            }
            
            setFodStatus(FOD_STATUS_OFF);
            resetTouchState();
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
