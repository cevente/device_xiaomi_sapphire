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
        dispThread_ = std::thread([this]() { displayEventMonitorThread(); });
        
        if (isFpcFod) {
            fodThread_ = std::thread([this]() { screenOffFodMonitorThread(); });
            screenThread_ = std::thread([this]() { screenStateMonitorThread(); });
        }

        LOG(INFO) << "UDFPS handler initialized";
    }

    void onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) {
        LOG(INFO) << __func__;
        
        mHbmStuck = false;
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
            if (mHbmStuck.load() && mIsFingerDown.load()) {
                LOG(INFO) << "💡 Force cleaning HBM stuck state";
                forceHbmCleanup();
                mHbmStuck = false;
            }
        });
    }

    void forceHbmCleanup() {
        LOG(INFO) << "Forcing HBM cleanup";
        
        std::lock_guard<std::mutex> lock(disp_mutex_);
        if (disp_fd_.get() >= 0) {
            disp_local_hbm_req req;
            req.base.flag = 1; 
            req.base.disp_id = MI_DISP_PRIMARY;
            req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
            if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) < 0) {
                LOG(ERROR) << "Failed to force HBM off: " << strerror(errno);
            }
        }
        
        std::lock_guard<std::mutex> touchLock(touch_mutex_);
        if (touch_fd_.get() >= 0) {
            int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, 0};
            if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
                LOG(ERROR) << "Failed to reset touch state: " << strerror(errno);
            }
        }
        
        mIsFingerDown = false;
        mPendingCleanup = false;
    }

    void forceCleanupIfPressed() {
        int screenState = getScreenState();
        bool pressed = false;

        // Only query fod_press_status if screen is OFF (Screen-Off FOD scenario)
        if (screenState == 0) {
            int fd = open(FOD_PRESS_STATUS_PATH, O_RDONLY);
            if (fd >= 0) {
                pressed = readBool(fd);
                close(fd);
            }
        }

        setFingerDown(false);
        
        if (pressed) {
            mPendingCleanup = true;
            LOG(INFO) << "UDFPS: Finger held during enrollment finish on screen-off. Cleanup deferred.";
            
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

    int getScreenState() {
        int fd = open(SCREEN_STATE_PATH, O_RDONLY);
        if (fd < 0) return -1;
        char buf[4];
        ssize_t len = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (len <= 0) return -1;
        buf[len] = '\0';
        return atoi(buf);
    }

    void screenStateMonitorThread() {
        int lastState = -1;
        while (isRunning.load()) {
            int currentState = getScreenState();
            if (currentState != -1) {
                bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);

                if (currentState != lastState) {
                    if (currentState == 0 && isFpcFod && isScreenOffEnabled) {
                        setFodStatus(FOD_STATUS_ON);
                    } else if (currentState == 1 && isFpcFod) {
                        if (!enrolling.load() && !mPendingCleanup) {
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

    // Dedicated solely to Screen-Off AOD touch monitoring using fod_press_status
    void screenOffFodMonitorThread() {
        LOG(INFO) << "Screen-off FOD press monitor thread started";
        
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
                break;
            }

            if (rc == 0) continue;

            if (!(fodPressStatusPoll.revents & (POLLERR | POLLPRI))) {
                fodPressStatusPoll.revents = 0;
                continue;
            }

            fodPressStatusPoll.revents = 0;

            const bool pressed = readBool(fd);
            
            // Safety guard: Only process this thread's events if screen state is OFF (0)
            if (getScreenState() != 0) {
                continue;
            }

            bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);
            if (!isScreenOffEnabled) {
                continue;
            }

            mIsFingerDown = pressed;
            LOG(DEBUG) << "Screen-off fod_press_status changed: " << (pressed ? "pressed" : "released");
            setFingerDown(pressed);
            
            if (!pressed) {
                if (mPendingCleanup || !enrolling.load()) {
                    setFodStatus(FOD_STATUS_OFF);
                    mPendingCleanup = false;
                    mHbmStuck = false;
                }
            }
        }

        close(fd);
        LOG(INFO) << "Screen-off FOD press monitor thread stopped";
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
                break;
            }

            if (rc == 0) continue;

            if (!(dispEventPoll.revents & POLLIN)) {
                dispEventPoll.revents = 0;
                continue;
            }

            dispEventPoll.revents = 0;

            disp_event_resp* response = parseDispEvent(fd);
            if (response == nullptr) {
                continue;
            }

            if (response->base.type != MI_DISP_EVENT_FOD) {
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
            return;
        }

        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, value};
        ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
    }

    void setFingerDown(bool pressed) {
        {
            std::lock_guard<std::mutex> lock(touch_mutex_);
            if (touch_fd_.get() >= 0) {
                int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, pressed ? 1 : 0};
                ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
            }
        }

        {
            std::lock_guard<std::mutex> lock(disp_mutex_);
            if (disp_fd_.get() >= 0) {
                disp_local_hbm_req req;
                req.base.flag = 1; 
                req.base.disp_id = MI_DISP_PRIMARY;
                req.local_hbm_value = pressed ? LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT
                                              : LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
                ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req);
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
