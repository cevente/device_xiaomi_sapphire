/*
 * Copyright (C) 2024 The LineageOS Project
 *
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
#define FOD_PRESS_STATUS_PATH "/sys/class/touch/touch_dev/fod_press_status"
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

}  // anonymous namespace

class XiaomiSm6225UdfpsHandler : public UdfpsHandler {
  public:
    XiaomiSm6225UdfpsHandler() : mDevice(nullptr), isFpcFod(false), mPendingCleanup(false), 
                                  mHbmStuck(false), mAuthInProgress(false), mIsScreenOnFod(false),
                                  mSamplesRemaining(0), mIsFinalEnrollment(false) {}

    ~XiaomiSm6225UdfpsHandler() {
        LOG(INFO) << "Destructor called, shutting down threads";
        shutdownThreads();
    }

    void init(fingerprint_device_t* device) {
        LOG(INFO) << "Initializing UDFPS handler";
        
        mDevice = device;
        
        // Open device nodes
        touch_fd_ = android::base::unique_fd(open(TOUCH_DEV_PATH, O_RDWR));
        if (touch_fd_.get() < 0) {
            LOG(ERROR) << "Failed to open touch device: " << strerror(errno);
        }

        disp_fd_ = android::base::unique_fd(open(DISP_FEATURE_PATH, O_RDWR));
        if (disp_fd_.get() < 0) {
            LOG(ERROR) << "Failed to open display device: " << strerror(errno);
        }

        // Determine fingerprint vendor
        std::string fpVendor = android::base::GetProperty("persist.vendor.sys.fp.vendor", "none");
        LOG(INFO) << "Fingerprint vendor: " << fpVendor;
        isFpcFod = (fpVendor == "fpc_fod");

        // Start monitoring threads
        fodThread_ = std::thread([this]() { fodPressMonitorThread(); });
        dispThread_ = std::thread([this]() { displayEventMonitorThread(); });
        
        if (isFpcFod) {
            screenThread_ = std::thread([this]() { screenStateMonitorThread(); });
        }

        LOG(INFO) << "UDFPS handler initialized";
    }

    void onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) {
        LOG(INFO) << __func__ << " - Framework pointer DOWN";
        
        // If we're in deferred cleanup mode, ignore finger DOWN
        if (mPendingCleanup.load()) {
            LOG(INFO) << "⏳ Deferred cleanup pending - ignoring finger DOWN";
            return;
        }
        
        // Clear HBM stuck flag on new touch
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
        
        // Just call setFingerDown(false) - it will handle the deferred cleanup case
        setFingerDown(false);
        
        // Normal cleanup
        mAuthInProgress = false;
        mIsScreenOnFod = false;
        mHbmStuck = false;
        mIsFinalEnrollment = false;
        
        if (!enrolling.load()) {
            setFodStatus(FOD_STATUS_OFF);
        }
    }

    void onAcquired(int32_t result, int32_t vendorCode) {
        LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;
        
        if (static_cast<AcquiredInfo>(result) == AcquiredInfo::GOOD) {
            LOG(INFO) << "✅ Acquisition GOOD";
            
            // If we're already in deferred cleanup mode, do NOTHING
            if (mPendingCleanup.load()) {
                LOG(INFO) << "⏳ Deferred cleanup already pending - ignoring GOOD acquisition";
                return;
            }
            
            // Check if this is the FINAL enrollment scan
            // mIsFinalEnrollment is set by onEnrollmentProgress when remaining == 0
            if (mIsFinalEnrollment.load() && enrolling.load() && mIsFingerDown.load()) {
                LOG(INFO) << "📝 FINAL enrollment scan completed with finger still down - deferring cleanup";
                mPendingCleanup = true;
                // Keep HBM alive until finger lifts
                enableHbm();
                return;
            }
            
            // Normal success path - not final enrollment
            mAuthInProgress = false;
            mIsScreenOnFod = false;
            setFingerDown(false);
            mPendingCleanup = false;
            mHbmStuck = false;
            mIsFinalEnrollment = false;
            
            // Force HBM off immediately on success
            forceHbmCleanup();
            
            if (!enrolling.load()) {
                setFodStatus(FOD_STATUS_OFF);
            }
            return;
        }

        if (!isFpcFod && vendorCode == 21) {
            setFodStatus(FOD_STATUS_ON);
        } else if (isFpcFod && vendorCode == 22) {
            setFodStatus(FOD_STATUS_ON);
        }
        
        // Detect if HBM is being killed while finger is still down
        if (vendorCode == 23 && mIsFingerDown) {
            // Don't re-enable HBM if we're in deferred cleanup mode
            if (!mPendingCleanup.load()) {
                LOG(INFO) << "⚠️ HBM killed while finger is still down - re-enabling";
                mHbmStuck = true;
                enableHbm();
                scheduleHbmCleanup();
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
            // CRITICAL: Don't set mPendingCleanup or enable HBM yet!
            // Wait for the final onAcquired(GOOD) to confirm the scan completed.
            // Just mark that we're in the final enrollment state.
            mIsFinalEnrollment = true;
            // Keep HBM as-is - don't re-enable it here
        } else {
            LOG(INFO) << "📝 Enrollment scan " << enrollmentId 
                      << " - " << remaining << " scans remaining";
            // Not the final scan - normal cleanup will happen on finger up
        }
    }

    void cancel() {
        LOG(INFO) << __func__;
        enrolling.store(false);
        mAuthInProgress = false;
        mIsScreenOnFod = false;
        mPendingCleanup = false;
        mSamplesRemaining = 0;
        mIsFinalEnrollment = false;
        forceCleanupIfPressed();
    }

    void preEnroll() {
        LOG(INFO) << __func__;
        mPendingCleanup = false;
        mHbmStuck = false;
        mSamplesRemaining = 0;
        mIsFinalEnrollment = false;
        enrolling.store(true);
    }

    void enroll() {
        LOG(INFO) << __func__;
        enrolling.store(true);
        mSamplesRemaining = 0;
        mIsFinalEnrollment = false;
    }

    void postEnroll() {
        LOG(INFO) << __func__;
        forceCleanupIfPressed();
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

    void enableHbm() {
        std::lock_guard<std::mutex> lock(disp_mutex_);
        if (disp_fd_.get() >= 0) {
            disp_local_hbm_req req;
            req.base.flag = 0;
            req.base.disp_id = MI_DISP_PRIMARY;
            req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT;
            if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) < 0) {
                LOG(ERROR) << "Failed to enable HBM: " << strerror(errno);
            } else {
                LOG(INFO) << "✅ HBM enabled";
            }
        }
    }

    void scheduleHbmCleanup() {
        std::lock_guard<std::mutex> lock(cleanup_mutex_);
        if (cleanupThread_.joinable()) {
            cleanupThread_.join();
        }
        cleanupThread_ = std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (mHbmStuck.load() && mIsFingerDown.load()) {
                LOG(INFO) << "💡 Force cleaning HBM stuck state";
                forceHbmCleanup();
                mHbmStuck = false;
            }
        });
    }

    void forceHbmCleanup() {
        LOG(INFO) << "Forcing HBM cleanup - complete state reset";
        
        {
            std::lock_guard<std::mutex> lock(disp_mutex_);
            if (disp_fd_.get() >= 0) {
                disp_local_hbm_req req;
                req.base.flag = 0;
                req.base.disp_id = MI_DISP_PRIMARY;
                req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
                if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) < 0) {
                    LOG(ERROR) << "Failed to force HBM off: " << strerror(errno);
                } else {
                    LOG(INFO) << "✅ HBM forced off";
                }
            }
        }
        
        {
            std::lock_guard<std::mutex> lock(touch_mutex_);
            if (touch_fd_.get() >= 0) {
                int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, 0};
                if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
                    LOG(ERROR) << "Failed to reset touch state: " << strerror(errno);
                } else {
                    LOG(INFO) << "✅ Touch state reset";
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
        
        if (!enrolling.load()) {
            setFodStatus(FOD_STATUS_OFF);
        }
        
        LOG(INFO) << "✅ Complete cleanup performed";
    }

    void forceCleanupIfPressed() {
        bool pressed = mIsFingerDown.load();

        if (pressed) {
            LOG(INFO) << "UDFPS: Finger held during enrollment finish. Deferred until pointer UP.";
            if (!mPendingCleanup.load()) {
                mPendingCleanup = true;
                mIsFinalEnrollment = true;
            }
            
            std::lock_guard<std::mutex> lock(cleanup_mutex_);
            if (cleanupThread_.joinable()) {
                cleanupThread_.join();
            }
            cleanupThread_ = std::thread([this]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(3000));
                if (mPendingCleanup.load()) {
                    LOG(INFO) << "⚠️ Finger still held after 3 seconds, forcing cleanup";
                    enrolling.store(false);
                    mIsFinalEnrollment = false;
                    forceHbmCleanup();
                    mPendingCleanup = false;
                    mHbmStuck = false;
                    mSamplesRemaining = 0;
                    setFodStatus(FOD_STATUS_OFF);
                }
            });
        } else {
            enrolling.store(false);
            mIsFinalEnrollment = false;
            mSamplesRemaining = 0;
            setFingerDown(false);
            mPendingCleanup = false;
            mHbmStuck = false;
            setFodStatus(FOD_STATUS_OFF);
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
        LOG(INFO) << "FOD press monitor thread started";
        
        int fd = open(FOD_PRESS_STATUS_PATH, O_RDONLY);
        if (fd < 0) {
            LOG(ERROR) << "Failed to open " << FOD_PRESS_STATUS_PATH 
                       << ", error: " << strerror(errno);
            return;
        }

        readBool(fd);

        struct pollfd fodPressStatusPoll = {
            .fd = fd,
            .events = POLLERR | POLLPRI,
            .revents = 0,
        };

        while (isRunning.load()) {
            int rc = poll(&fodPressStatusPoll, 1, 1000);
            
            if (rc < 0) {
                if (errno == EINTR) continue;
                LOG(ERROR) << "Poll failed: " << strerror(errno);
                break;
            }

            if (rc == 0) continue;

            if (!(fodPressStatusPoll.revents & (POLLERR | POLLPRI))) {
                if (fodPressStatusPoll.revents & (POLLHUP | POLLNVAL)) {
                    LOG(ERROR) << "Poll error event: " << fodPressStatusPoll.revents;
                    break;
                }
                fodPressStatusPoll.revents = 0;
                continue;
            }

            fodPressStatusPoll.revents = 0;

            const bool pressed = readBool(fd);
            mIsFingerDown = pressed;
            
            bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);
            if (!isScreenOffEnabled && getBrightness() == 0) {
                LOG(INFO) << "UDFPS: Touch ignored. Screen-Off disabled.";
                continue;
            }

            LOG(DEBUG) << "fod_press_status changed: " << (pressed ? "pressed" : "released");
            
            if (!pressed) {
                LOG(INFO) << "💡 Screen-off FOD lift detected";
                // Just call setFingerDown(false) - it will handle deferred cleanup
                setFingerDown(false);
                if (!enrolling.load()) {
                    setFodStatus(FOD_STATUS_OFF);
                }
            } else {
                setFingerDown(true);
            }
            
            readBool(fd);
        }

        close(fd);
        LOG(INFO) << "FOD press monitor thread stopped";
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
            LOG(DEBUG) << "Display event data: 0x" << std::hex << value;

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
        } else {
            LOG(DEBUG) << "Set FOD status to " << value;
        }
    }

    void setFingerDown(bool pressed) {
        // If we're in deferred cleanup mode:
        // - Ignore finger DOWN events (don't re-enable HBM)
        // - ALWAYS process finger UP events (turn off HBM)
        if (mPendingCleanup.load()) {
            if (pressed) {
                LOG(INFO) << "⏳ Deferred cleanup pending - ignoring finger DOWN";
                return;
            } else {
                LOG(INFO) << "💡 Deferred cleanup pending - processing finger UP to turn off HBM";
                // Don't return - process the finger UP
            }
        }
        
        bool screenOn = isScreenOn();
        
        if (screenOn && pressed) {
            mIsScreenOnFod = true;
            mAuthInProgress = true;
            LOG(INFO) << "Screen ON FOD: DOWN - enabling HBM";
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

        {
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
        
        mIsFingerDown = pressed;
        
        if (!pressed) {
            mAuthInProgress = false;
            mIsScreenOnFod = false;
            mIsFinalEnrollment = false;
            
            // If we were in deferred cleanup, clear it now
            if (mPendingCleanup.load()) {
                LOG(INFO) << "💡 Deferred cleanup completed on finger UP - clearing state";
                mPendingCleanup = false;
                enrolling.store(false);
                mSamplesRemaining = 0;
            }
            
            // Ensure HBM is off on finger up
            forceHbmCleanup();
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
