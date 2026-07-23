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
#define SCREEN_STATE_PATH "/sys/class/thermal/thermal_message/screen_state"

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

static int getScreenState() {
    int fd = open(SCREEN_STATE_PATH, O_RDONLY);
    if (fd < 0) {
        LOG(ERROR) << "Failed to open screen_state: " << strerror(errno);
        return -1;
    }
    char buf[4];
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0) return -1;
    buf[len] = '\0';
    return atoi(buf);  // Returns 1 for screen ON, 0 for OFF
}

static bool isScreenOn() {
    int state = getScreenState();
    return state == 1;
}

static bool isScreenOff() {
    int state = getScreenState();
    return state == 0;
}

}  // anonymous namespace

class XiaomiSm6225UdfpsHandler : public UdfpsHandler {
  public:
    XiaomiSm6225UdfpsHandler() : mDevice(nullptr), isFpcFod(false), mPendingCleanup(false), mHbmStuck(false) {}

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
        LOG(INFO) << __func__;
        
        // Clear HBM stuck flag on new touch
        mHbmStuck = false;
        
        mFbDownTimeMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

        if (isFpcFod) {
            setFodStatus(FOD_STATUS_ON);
        }

        setFingerDown(true);
    }

    void onFingerUp() {
        LOG(INFO) << __func__;
        setFingerDown(false);
        mPendingCleanup = false;
        mHbmStuck = false;
        
        if (!enrolling.load()) {
            setFodStatus(FOD_STATUS_OFF);
        }
    }

    void onAcquired(int32_t result, int32_t vendorCode) {
        LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;
        
        if (static_cast<AcquiredInfo>(result) == AcquiredInfo::GOOD) {
            setFingerDown(false);
            mPendingCleanup = false;
            mHbmStuck = false;
            
            if (!enrolling.load()) {
                setFodStatus(FOD_STATUS_OFF);
            }
        }

        if (!isFpcFod && vendorCode == 21) {
            setFodStatus(FOD_STATUS_ON);
        } else if (isFpcFod && vendorCode == 22) {
            setFodStatus(FOD_STATUS_ON);
        }
        
        // Detect if HBM is being killed while finger is still down
        if (vendorCode == 23 && mIsFingerDown) {
            LOG(INFO) << "⚠️ HBM killed while finger is still down - potential stuck state detected";
            mHbmStuck = true;
            scheduleHbmCleanup();
        }
    }

    void cancel() {
        LOG(INFO) << __func__;
        enrolling.store(false);
        forceCleanupIfPressed();
    }

    void preEnroll() {
        LOG(INFO) << __func__;
        mPendingCleanup = false;
        mHbmStuck = false;
        enrolling.store(true);
    }

    void enroll() {
        LOG(INFO) << __func__;
        enrolling.store(true);
    }

    void postEnroll() {
        LOG(INFO) << __func__;
        enrolling.store(false);
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

    void scheduleHbmCleanup() {
        std::lock_guard<std::mutex> lock(cleanup_mutex_);
        if (cleanupThread_.joinable()) {
            cleanupThread_.join();
        }
        cleanupThread_ = std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            // CRITICAL FIX: Check conditions more carefully
            if (mHbmStuck.load() && mIsFingerDown.load()) {
                LOG(INFO) << "💡 Force cleaning HBM stuck state";
                
                // Don't force cleanup if finger is actually still pressed
                int fd = open(FOD_PRESS_STATUS_PATH, O_RDONLY);
                if (fd >= 0) {
                    bool stillPressed = readBool(fd);
                    close(fd);
                    if (stillPressed) {
                        LOG(WARNING) << "Finger still physically pressed, postponing cleanup";
                        // Reschedule
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                        if (mHbmStuck.load()) {
                            // Check again
                            fd = open(FOD_PRESS_STATUS_PATH, O_RDONLY);
                            if (fd >= 0) {
                                stillPressed = readBool(fd);
                                close(fd);
                                if (!stillPressed) {
                                    forceHbmCleanup();
                                } else {
                                    LOG(WARNING) << "Finger still pressed after 1.5s, forcing anyway";
                                    forceHbmCleanup();
                                }
                            }
                        }
                        return;
                    }
                }
                
                forceHbmCleanup();
                mHbmStuck = false;
            }
        });
    }

    void forceHbmCleanup() {
        LOG(INFO) << "Forcing HBM cleanup";
        
        // Don't touch HBM if finger is still down
        if (mIsFingerDown.load()) {
            LOG(WARNING) << "Finger still down, skipping HBM cleanup";
            return;
        }
        
        // Force HBM off via display ioctl
        std::lock_guard<std::mutex> lock(disp_mutex_);
        if (disp_fd_.get() >= 0) {
            disp_local_hbm_req req;
            req.base.flag = 0;
            req.base.disp_id = MI_DISP_PRIMARY;
            req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
            if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) < 0) {
                LOG(ERROR) << "Failed to force HBM off: " << strerror(errno);
            }
        }
        
        // Reset touch state - BUT DON'T USE THP_FOD_DOWNUP_CTL (1001)
        // This mode is not supported by the touch driver and causes crashes
        std::lock_guard<std::mutex> touchLock(touch_mutex_);
        if (touch_fd_.get() >= 0) {
            // Use Touch_Fod_Enable (10) instead, which IS supported
            int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, FOD_STATUS_OFF};
            if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
                LOG(ERROR) << "Failed to reset touch state: " << strerror(errno);
            }
        }
        
        mIsFingerDown = false;
        mPendingCleanup = false;
    }

    void forceScreenOffFodReset() {
        LOG(INFO) << "🔄 Force resetting screen-off FOD state";
        
        // 1. Disable FOD
        setFodStatus(FOD_STATUS_OFF);
        
        // 2. Reset touch state - use mode 10 not mode 1001
        {
            std::lock_guard<std::mutex> lock(touch_mutex_);
            if (touch_fd_.get() >= 0) {
                int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, FOD_STATUS_OFF};
                ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
            }
        }
        
        // 3. Clear HBM
        {
            std::lock_guard<std::mutex> lock(disp_mutex_);
            if (disp_fd_.get() >= 0) {
                disp_local_hbm_req req;
                req.base.flag = 0;
                req.base.disp_id = MI_DISP_PRIMARY;
                req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
                ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req);
            }
        }
        
        // 4. Clear state flags
        mIsFingerDown = false;
        mPendingCleanup = false;
        mHbmStuck = false;
        
        // 5. Re-enable FOD if screen is off
        if (isScreenOff() && isFpcFod) {
            bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);
            if (isScreenOffEnabled) {
                LOG(INFO) << "Screen off, re-enabling FOD after reset";
                setFodStatus(FOD_STATUS_ON);
            }
        }
    }

    void recoverTouchDriver() {
        LOG(WARNING) << "⚠️ Touch driver in error state, attempting recovery";
        
        // 1. Reset FOD mode using mode 10 (supported)
        {
            int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, FOD_STATUS_OFF};
            ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
        }
        
        // 2. Re-initialize touch HAL using mode 1004 (supported)
        {
            int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_HAL_INIT_READY, 1};
            ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
        }
        
        // 3. Small delay for driver to settle
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        // 4. Restore FOD mode if needed
        if (!enrolling.load() && mIsFingerDown.load()) {
            int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, FOD_STATUS_ON};
            ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
        }
        
        LOG(INFO) << "✅ Touch driver recovery attempted";
    }

    void forceCleanupIfPressed() {
        int fd = open(FOD_PRESS_STATUS_PATH, O_RDONLY);
        bool pressed = false;
        if (fd >= 0) {
            pressed = readBool(fd);
            close(fd);
        }

        setFingerDown(false);
        
        if (pressed) {
            mPendingCleanup = true;
            LOG(INFO) << "UDFPS: Finger held during enrollment finish. Cleanup deferred until lift.";
            
            std::lock_guard<std::mutex> lock(cleanup_mutex_);
            if (cleanupThread_.joinable()) {
                cleanupThread_.join();
            }
            cleanupThread_ = std::thread([this]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(3000));
                if (mPendingCleanup.load()) {
                    LOG(INFO) << "⚠️ Finger still held after 3 seconds, forcing cleanup";
                    forceHbmCleanup();
                    mPendingCleanup = false;
                    mHbmStuck = false;
                    if (!enrolling.load()) {
                        setFodStatus(FOD_STATUS_OFF);
                    }
                }
            });
        } else {
            mPendingCleanup = false;
            mHbmStuck = false;
            setFodStatus(FOD_STATUS_OFF);
        }
    }

    void screenStateMonitorThread() {
        int lastState = -1;
        while (isRunning.load()) {
            int currentState = getScreenState();
            
            if (currentState < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            if (currentState != lastState) {
                bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);

                if (currentState == 0 && isFpcFod && isScreenOffEnabled) {
                    // Screen OFF: Enable FOD
                    LOG(INFO) << "Screen OFF, enabling FOD";
                    setFodStatus(FOD_STATUS_ON);
                } else if (currentState == 1 && isFpcFod) {
                    // Screen ON: FOD must be disabled
                    LOG(INFO) << "Screen ON, disabling FOD";
                    
                    // CRITICAL: Force FOD off regardless of enroll state
                    setFodStatus(FOD_STATUS_OFF);
                    
                    // Clear any pending state from screen-off FOD
                    mPendingCleanup = false;
                    mHbmStuck = false;
                    
                    // Ensure touch driver is in normal mode using mode 10
                    {
                        std::lock_guard<std::mutex> lock(touch_mutex_);
                        if (touch_fd_.get() >= 0) {
                            int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, FOD_STATUS_OFF};
                            ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
                        }
                    }
                    
                    // Force HBM off
                    {
                        std::lock_guard<std::mutex> lock(disp_mutex_);
                        if (disp_fd_.get() >= 0) {
                            disp_local_hbm_req req;
                            req.base.flag = 0;
                            req.base.disp_id = MI_DISP_PRIMARY;
                            req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
                            ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req);
                        }
                    }
                }
                lastState = currentState;
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
            
            // CRITICAL: Check screen state before processing
            if (isScreenOn()) {
                LOG(WARNING) << "⚠️ Screen is ON but got fod_press_status event!";
                LOG(WARNING) << "This means screen-off FOD wasn't properly disabled";
                
                // Force cleanup
                forceScreenOffFodReset();
                continue;
            }

            bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);
            if (!isScreenOffEnabled) {
                LOG(INFO) << "UDFPS: Screen-off FOD disabled, ignoring touch";
                continue;
            }

            LOG(DEBUG) << "fod_press_status changed: " << (pressed ? "pressed" : "released");
            setFingerDown(pressed);
            
            if (!pressed) {
                if (mPendingCleanup || !enrolling.load()) {
                    setFodStatus(FOD_STATUS_OFF);
                    mPendingCleanup = false;
                    mHbmStuck = false;
                    LOG(INFO) << "💡 FOD touch successfully disabled on physical finger release";
                }
            }
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

        // Block unsupported values - only allow FOD_STATUS_ON (1) and FOD_STATUS_OFF (0)
        // Mode 1001 (THP_FOD_DOWNUP_CTL) is NOT supported by the touch driver
        if (value != FOD_STATUS_ON && value != FOD_STATUS_OFF) {
            LOG(WARNING) << "Blocking unsupported FOD value: " << value;
            return;
        }

        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, value};
        if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
            LOG(ERROR) << "Failed to set FOD status to " << value 
                       << ": " << strerror(errno);
            
            // Attempt recovery if this fails
            if (isScreenOff()) {
                LOG(WARNING) << "Attempting FOD recovery...";
                // Try one more time
                if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
                    LOG(ERROR) << "FOD recovery failed, disabling FOD";
                    // Disable FOD to prevent further issues
                    int disableBuf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, FOD_STATUS_OFF};
                    ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &disableBuf);
                }
            }
        } else {
            LOG(DEBUG) << "Set FOD status to " << value;
        }
    }

    void setFingerDown(bool pressed) {
        bool touchSuccess = false;
        bool hbmSuccess = false;
        
        // Check screen state using thermal node
        if (isScreenOn()) {
            LOG(WARNING) << "⚠️ Screen is ON but got FOD touch event!";
            LOG(WARNING) << "This means screen-off FOD wasn't properly disabled";
            
            // Force cleanup immediately
            forceScreenOffFodReset();
            return;
        }
        
        // Screen is off, process FOD touch normally
        LOG(INFO) << "Screen OFF, processing FOD touch: " << (pressed ? "DOWN" : "UP");
        
        // IMPORTANT: DO NOT send THP_FOD_DOWNUP_CTL (mode 1001) to touch driver
        // The fts_ts driver does not support this mode and will enter error state
        // Use Touch_Fod_Enable (mode 10) instead which IS supported
        
        // 1. Set FOD status using mode 10 (supported)
        {
            std::lock_guard<std::mutex> lock(touch_mutex_);
            if (touch_fd_.get() >= 0) {
                int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, pressed ? FOD_STATUS_ON : FOD_STATUS_OFF};
                if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) == 0) {
                    touchSuccess = true;
                } else {
                    LOG(ERROR) << "Failed to set finger " << (pressed ? "down" : "up") 
                               << ": " << strerror(errno);
                    // If screen-off FOD touch fails, disable FOD
                    setFodStatus(FOD_STATUS_OFF);
                    return;
                }
            }
        }
        
        // 2. Set HBM only if touch succeeded or we're releasing
        if (touchSuccess || !pressed) {
            std::lock_guard<std::mutex> lock(disp_mutex_);
            if (disp_fd_.get() >= 0) {
                disp_local_hbm_req req;
                req.base.flag = 0;
                req.base.disp_id = MI_DISP_PRIMARY;
                req.local_hbm_value = pressed ? LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT
                                              : LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
                if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) == 0) {
                    hbmSuccess = true;
                } else {
                    LOG(ERROR) << "Failed to set HBM: " << strerror(errno);
                }
            }
        }
        
        // 3. Send fingerprint command (last, optional)
        {
            std::lock_guard<std::mutex> lock(device_mutex_);
            if (mDevice != nullptr) {
                mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS,
                              pressed ? PARAM_FOD_PRESSED : PARAM_FOD_RELEASED);
            }
        }
        
        // 4. If finger released, re-arm FOD if screen is still off
        if (!pressed && isScreenOff()) {
            if (isFpcFod && android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true)) {
                LOG(INFO) << "Screen still off, re-arming FOD";
                setFodStatus(FOD_STATUS_ON);
            }
        }
        
        // 5. If HBM failed but touch succeeded, schedule retry
        if (pressed && touchSuccess && !hbmSuccess) {
            scheduleHbmCleanup();
        }
        
        mIsFingerDown = pressed;
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
