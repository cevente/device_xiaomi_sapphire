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
        return -1;
    }
    char buf[4];
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0) return -1;
    buf[len] = '\0';
    return atoi(buf);
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
    XiaomiSm6225UdfpsHandler() : mDevice(nullptr), isFpcFod(false), mPendingCleanup(false), 
                                  mHbmStuck(false), mFodActive(false), mProcessingTouch(false),
                                  mHbmLocked(false) {}

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
        LOG(INFO) << __func__;
        
        mHbmStuck = false;
        mFodActive = true;
        
        mFbDownTimeMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

        if (isFpcFod) {
            setFodStatus(FOD_STATUS_ON);
        }

        setFingerDown(true);
    }

    void onFingerUp() {
        LOG(INFO) << __func__;
        mFodActive = false;
        setFingerDown(false);
        mPendingCleanup = false;
        mHbmStuck = false;
        
        if (!enrolling.load()) {
            setFodStatus(FOD_STATUS_OFF);
        }
    }

    void onAcquired(int32_t result, int32_t vendorCode) {
        LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;
        
        // CRITICAL: vendorCode 40 = ILLUMINATION_TOO_SLOW
        // This means HBM wasn't ready in time - force it on
        if (vendorCode == 40 && mIsFingerDown.load()) {
            LOG(WARNING) << "⚠️ vendorCode 40 - ILLUMINATION_TOO_SLOW, forcing HBM retry";
            
            // Force HBM enable again immediately
            enableHbm();
            
            // Tell fingerprint HAL to retry
            // The HAL will retry capture after receiving this
            return;
        }
        
        if (static_cast<AcquiredInfo>(result) == AcquiredInfo::GOOD) {
            mFodActive = false;
            mHbmLocked = false;
            setFingerDown(false);
            mPendingCleanup = false;
            mHbmStuck = false;
            mProcessingTouch = false;
            
            // Cancel any delayed cleanup
            std::lock_guard<std::mutex> lock(cleanup_mutex_);
            if (cleanupThread_.joinable()) {
                cleanupThread_.join();
            }
            
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
        
        if (vendorCode == 23 && mIsFingerDown) {
            LOG(INFO) << "⚠️ HBM killed while finger is still down";
            mHbmStuck = true;
            // Re-enable HBM if we're still in auth
            if (mHbmLocked.load() || mFodActive.load()) {
                LOG(INFO) << "HBM locked/rearming - re-enabling";
                enableHbm();
            } else {
                scheduleHbmCleanup();
            }
        }
    }

    void cancel() {
        LOG(INFO) << __func__;
        enrolling.store(false);
        mFodActive = false;
        mHbmLocked = false;
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
    std::atomic<bool> mFodActive{false};
    std::atomic<bool> mProcessingTouch{false};
    std::atomic<bool> mHbmLocked{false};
    bool isFpcFod;
    
    std::atomic<uint64_t> mFbDownTimeMs{0};

    std::mutex touch_mutex_;
    std::mutex disp_mutex_;
    std::mutex device_mutex_;
    std::mutex cleanup_mutex_;
    std::mutex state_mutex_;

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
                mHbmLocked = true;
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
            if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) < 0) {
                LOG(ERROR) << "Failed to disable HBM: " << strerror(errno);
            } else {
                LOG(INFO) << "✅ HBM disabled";
                mHbmLocked = false;
            }
        }
    }

    void scheduleHbmCleanup() {
        std::lock_guard<std::mutex> lock(cleanup_mutex_);
        if (cleanupThread_.joinable()) {
            cleanupThread_.join();
        }
        cleanupThread_ = std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            
            if (mHbmStuck.load() && mIsFingerDown.load()) {
                LOG(INFO) << "💡 Force cleaning HBM stuck state";
                
                // Check if finger is still physically pressed
                int fd = open(FOD_PRESS_STATUS_PATH, O_RDONLY);
                bool stillPressed = false;
                if (fd >= 0) {
                    stillPressed = readBool(fd);
                    close(fd);
                }
                
                if (stillPressed) {
                    LOG(WARNING) << "Finger still pressed, waiting...";
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    
                    fd = open(FOD_PRESS_STATUS_PATH, O_RDONLY);
                    if (fd >= 0) {
                        stillPressed = readBool(fd);
                        close(fd);
                    }
                }
                
                if (!stillPressed || mFodActive.load() == false) {
                    forceHbmCleanup();
                    mHbmStuck = false;
                }
            }
        });
    }

    void forceHbmCleanup() {
        LOG(INFO) << "Forcing HBM cleanup";
        
        disableHbm();
        mIsFingerDown = false;
        mPendingCleanup = false;
        mFodActive = false;
        mHbmLocked = false;
    }

    void forceCleanupIfPressed() {
        int fd = open(FOD_PRESS_STATUS_PATH, O_RDONLY);
        bool pressed = false;
        if (fd >= 0) {
            pressed = readBool(fd);
            close(fd);
        }

        setFingerDown(false);
        mFodActive = false;
        
        if (pressed) {
            mPendingCleanup = true;
            LOG(INFO) << "UDFPS: Finger held during enrollment finish. Cleanup deferred.";
            
            std::lock_guard<std::mutex> lock(cleanup_mutex_);
            if (cleanupThread_.joinable()) {
                cleanupThread_.join();
            }
            cleanupThread_ = std::thread([this]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                if (mPendingCleanup.load()) {
                    LOG(INFO) << "⚠️ Forcing cleanup after timeout";
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
                bool isScreenOffEnabled = android::base::GetBoolProperty(
                    "persist.vendor.sys.fp.screen_off", true);

                if (currentState == 0 && isFpcFod && isScreenOffEnabled) {
                    LOG(INFO) << "Screen OFF, enabling FOD";
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    if (!mProcessingTouch.load() && !mIsFingerDown.load()) {
                        setFodStatus(FOD_STATUS_ON);
                    }
                } else if (currentState == 1 && isFpcFod) {
                    LOG(INFO) << "Screen ON, disabling FOD";
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    
                    // Don't disable FOD if authentication is in progress
                    if (mProcessingTouch.load() || mIsFingerDown.load()) {
                        LOG(INFO) << "Authentication in progress, delaying FOD disable";
                        
                        // Schedule delayed cleanup
                        std::lock_guard<std::mutex> cleanupLock(cleanup_mutex_);
                        if (cleanupThread_.joinable()) {
                            cleanupThread_.join();
                        }
                        cleanupThread_ = std::thread([this]() {
                            int waitCount = 0;
                            while (waitCount < 10 && (mProcessingTouch.load() || mIsFingerDown.load())) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                                waitCount++;
                            }
                            LOG(INFO) << "Delayed FOD cleanup after auth";
                            forceScreenOffFodReset();
                        });
                        return;
                    }
                    
                    setFodStatus(FOD_STATUS_OFF);
                    mPendingCleanup = false;
                    mHbmStuck = false;
                    mFodActive = false;
                    mIsFingerDown = false;
                    mProcessingTouch = false;
                    mHbmLocked = false;
                    
                    // Wait for driver to settle
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    
                    // Reset touch state using mode 10 (not 1001)
                    {
                        std::lock_guard<std::mutex> touchLock(touch_mutex_);
                        if (touch_fd_.get() >= 0) {
                            int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, FOD_STATUS_OFF};
                            ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
                        }
                    }
                    
                    // Force HBM off
                    disableHbm();
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
            
            // Check screen state before processing
            if (isScreenOn()) {
                LOG(WARNING) << "⚠️ Screen ON but got fod_press_status event!";
                // Just clean up and ignore
                setFodStatus(FOD_STATUS_OFF);
                mPendingCleanup = false;
                mHbmStuck = false;
                mFodActive = false;
                mProcessingTouch = false;
                mIsFingerDown = false;
                mHbmLocked = false;
                disableHbm();
                continue;
            }

            bool isScreenOffEnabled = android::base::GetBoolProperty(
                "persist.vendor.sys.fp.screen_off", true);
            if (!isScreenOffEnabled) {
                continue;
            }

            LOG(DEBUG) << "fod_press_status: " << (pressed ? "pressed" : "released");
            
            if (pressed) {
                mProcessingTouch = true;
                mFodActive = true;
            }
            
            setFingerDown(pressed);
            
            if (!pressed) {
                mProcessingTouch = false;
                mFodActive = false;
                if (mPendingCleanup || !enrolling.load()) {
                    // Wait for driver to settle
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    setFodStatus(FOD_STATUS_OFF);
                    mPendingCleanup = false;
                    mHbmStuck = false;
                    mHbmLocked = false;
                    LOG(INFO) << "💡 FOD touch disabled on release";
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

        // Only allow valid FOD status values
        if (value != FOD_STATUS_ON && value != FOD_STATUS_OFF) {
            LOG(WARNING) << "Invalid FOD value: " << value;
            return;
        }

        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, value};
        if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
            LOG(ERROR) << "Failed to set FOD status to " << value 
                       << ": " << strerror(errno);
        } else {
            LOG(DEBUG) << "Set FOD status to " << value;
        }
    }

    void setFingerDown(bool pressed) {
        // SCREEN-ON FOD: Handle separately
        if (isScreenOn()) {
            if (pressed) {
                LOG(INFO) << "Screen ON FOD touch: DOWN";
                // Enable HBM for screen-on FOD
                enableHbm();
                mFodActive = true;
                mIsFingerDown = true;
                mHbmLocked = true;
                
                // Send touch command
                {
                    std::lock_guard<std::mutex> lock(touch_mutex_);
                    if (touch_fd_.get() >= 0) {
                        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, 1};
                        ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
                    }
                }
                
                // Send fingerprint command
                {
                    std::lock_guard<std::mutex> lock(device_mutex_);
                    if (mDevice != nullptr) {
                        mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, PARAM_FOD_PRESSED);
                    }
                }
            } else {
                LOG(INFO) << "Screen ON FOD touch: UP";
                disableHbm();
                mFodActive = false;
                mIsFingerDown = false;
                mHbmLocked = false;
                
                // Send touch command
                {
                    std::lock_guard<std::mutex> lock(touch_mutex_);
                    if (touch_fd_.get() >= 0) {
                        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, 0};
                        ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
                    }
                }
                
                // Send fingerprint command
                {
                    std::lock_guard<std::mutex> lock(device_mutex_);
                    if (mDevice != nullptr) {
                        mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, PARAM_FOD_RELEASED);
                    }
                }
            }
            return;
        }
        
        // SCREEN-OFF FOD: Handle normally
        LOG(INFO) << "FOD touch: " << (pressed ? "DOWN" : "UP") << " screen=OFF";
        
        if (!pressed) {
            // For UP events, reset FOD status first
            setFodStatus(FOD_STATUS_OFF);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        // For DOWN events, ENABLE HBM FIRST before anything else
        if (pressed) {
            LOG(INFO) << "Screen OFF FOD: enabling HBM first";
            enableHbm();
            mHbmLocked = true;
            
            // Give display driver time to actually enable HBM
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        
        // 1. Send THP_FOD_DOWNUP_CTL (mode 1001)
        {
            std::lock_guard<std::mutex> lock(touch_mutex_);
            if (touch_fd_.get() >= 0) {
                int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, pressed ? 1 : 0};
                if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
                    LOG(ERROR) << "Failed to set finger " << (pressed ? "down" : "up") 
                               << ": " << strerror(errno);
                    if (pressed) {
                        disableHbm();
                    }
                    return;
                }
            }
        }
        
        // 2. Send fingerprint command
        {
            std::lock_guard<std::mutex> lock(device_mutex_);
            if (mDevice != nullptr) {
                mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS,
                              pressed ? PARAM_FOD_PRESSED : PARAM_FOD_RELEASED);
            }
        }
        
        // 3. Handle state updates
        if (pressed) {
            mFodActive = true;
            mIsFingerDown = true;
            mProcessingTouch = true;
        } else {
            mFodActive = false;
            mIsFingerDown = false;
            mProcessingTouch = false;
            mHbmLocked = false;
            
            // Wait for driver to settle
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            
            // Re-arm FOD if screen is still off
            if (isScreenOff()) {
                if (isFpcFod && android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true)) {
                    LOG(INFO) << "Re-arming FOD after release";
                    setFodStatus(FOD_STATUS_OFF);
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    setFodStatus(FOD_STATUS_ON);
                }
            } else {
                setFodStatus(FOD_STATUS_OFF);
            }
        }
    }

    void forceScreenOffFodReset() {
        LOG(INFO) << "🔄 Force resetting screen-off FOD state";
        
        // Unlock HBM first
        mHbmLocked = false;
        disableHbm();
        
        // 1. Disable FOD
        setFodStatus(FOD_STATUS_OFF);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        
        // 2. Reset touch state
        {
            std::lock_guard<std::mutex> lock(touch_mutex_);
            if (touch_fd_.get() >= 0) {
                int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, FOD_STATUS_OFF};
                ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
            }
        }
        
        // 3. Clear state flags
        mIsFingerDown = false;
        mPendingCleanup = false;
        mHbmStuck = false;
        mFodActive = false;
        mProcessingTouch = false;
        mHbmLocked = false;
        
        // 4. Re-enable FOD if screen is off
        if (isScreenOff() && isFpcFod) {
            bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);
            if (isScreenOffEnabled) {
                LOG(INFO) << "Screen off, re-enabling FOD after reset";
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                setFodStatus(FOD_STATUS_ON);
            }
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
