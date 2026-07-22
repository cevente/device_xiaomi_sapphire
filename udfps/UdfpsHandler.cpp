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
#define HBM_CTRL_NODE "/proc/mi_display/tx_cmd_set_prim"

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
    XiaomiSm6225UdfpsHandler() : mDevice(nullptr), isFpcFod(false), mCachedScreenState(-1) {
        mLastScreenStateCheck = std::chrono::steady_clock::now();
    }

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
        
        mFbDownTimeMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

        if (isFpcFod) {
            setFodStatus(FOD_STATUS_ON);
        }

        setFingerDown(true);
    }

    void onFingerUp() {
        LOG(INFO) << __func__;
        
        uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t elapsed = now - mFbDownTimeMs.load();
        
        if (elapsed < 250) {
            LOG(INFO) << "UDFPS: Ignorando falso UP del framework (pasaron " << elapsed << "ms)";
            return;
        }

        killHbm();
        setFingerDown(false);
        setFodStatus(FOD_STATUS_OFF);

        if (!enrolling.load()) {
            std::thread([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                LOG(INFO) << "UDFPS: Self-terminating to reset deadlocked HAL state";
                std::exit(0);
            }).detach();
        }
    }

    void onAcquired(int32_t result, int32_t vendorCode) {
        LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;
        
        if (static_cast<AcquiredInfo>(result) == AcquiredInfo::GOOD) {
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
            
            if (!enrolling.load()) {
                killHbm();
                setFodStatus(FOD_STATUS_OFF);
                setFingerDown(false);
            }
        }

        if (!isFpcFod && vendorCode == 21) {
            setFodStatus(FOD_STATUS_ON);
        } else if (isFpcFod && vendorCode == 22) {
            setFodStatus(FOD_STATUS_ON);
        }
    }

    void cancel() {
        LOG(INFO) << __func__;
        enrolling.store(false);
        
        killHbm();
        setFodStatus(FOD_STATUS_OFF);
        setFingerDown(false);
    }

    void preEnroll() {
        LOG(INFO) << __func__;
        enrolling.store(true);
    }

    void enroll() {
        LOG(INFO) << __func__;
        enrolling.store(true);
    }

    void postEnroll() {
        LOG(INFO) << __func__;
        enrolling.store(false);
        
        killHbm();
        setFodStatus(FOD_STATUS_OFF);
        setFingerDown(false);

        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            LOG(INFO) << "UDFPS: Self-terminating post-enrollment to clear HBM lockup";
            std::exit(0);
        }).detach();
    }

  private:
    fingerprint_device_t* mDevice;
    android::base::unique_fd touch_fd_;
    android::base::unique_fd disp_fd_;
    std::atomic<bool> enrolling{false};
    std::atomic<bool> isRunning{true};
    bool isFpcFod;
    
    std::atomic<uint64_t> mFbDownTimeMs{0};
    std::atomic<int> mCachedScreenState{-1};
    std::chrono::steady_clock::time_point mLastScreenStateCheck;
    std::mutex mScreenStateMutex;

    std::mutex touch_mutex_;
    std::mutex disp_mutex_;
    std::mutex device_mutex_;

    std::thread fodThread_;
    std::thread dispThread_;
    std::thread screenThread_;

    int getScreenState() {
        std::lock_guard<std::mutex> lock(mScreenStateMutex);
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - mLastScreenStateCheck).count();
        
        // Cache for 100ms to avoid excessive file reads
        if (elapsed < 100 && mCachedScreenState != -1) {
            return mCachedScreenState;
        }
        
        int fd = open(SCREEN_STATE_PATH, O_RDONLY);
        if (fd < 0) {
            LOG(ERROR) << "Failed to open screen state: " << strerror(errno);
            return -1;
        }
        char buf[2];
        ssize_t len = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (len <= 0) {
            LOG(ERROR) << "Failed to read screen state: " << strerror(errno);
            return -1;
        }
        buf[len] = '\0';
        mCachedScreenState = atoi(buf);
        mLastScreenStateCheck = now;
        return mCachedScreenState;
    }

    void killHbm() {
        int fd = open(HBM_CTRL_NODE, O_WRONLY);
        if (fd >= 0) {
            write(fd, "71\n", 3);
            close(fd);
            LOG(INFO) << "💡 HBM killed via proc node (ID 71)";
        } else {
            LOG(ERROR) << "❌ Failed to open HBM_CTRL_NODE";
        }
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
                        if (!enrolling.load()) {
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
            uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            
            if (pressed) {
                mFbDownTimeMs.store(now);
            } else {
                uint64_t elapsed = now - mFbDownTimeMs.load();
                if (elapsed < 250) {
                    LOG(INFO) << "UDFPS: Hardware marco UP muy rapido (" << elapsed << "ms). Esperando 100ms...";
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if (readBool(fd)) {
                        LOG(INFO) << "UDFPS: El dedo seguia ahi! Falso UP fisico ignorado.";
                        continue; 
                    }
                }
            }

            bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);
            if (!isScreenOffEnabled) {
                LOG(INFO) << "UDFPS: Toque ignorado. Screen-Off desactivado.";
                continue;
            }

            LOG(DEBUG) << "fod_press_status changed: " << (pressed ? "pressed" : "released");
            setFingerDown(pressed);
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

            struct disp_event_resp* response = parseDispEvent(fd);
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
        bool isScreenOn = (getScreenState() == 1);

        // Always update touch controller and fingerprint HAL regardless of screen state
        {
            std::lock_guard<std::mutex> lock(touch_mutex_);
            if (touch_fd_.get() >= 0) {
                int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, pressed ? 1 : 0};
                if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
                    LOG(ERROR) << "Failed to set finger down: " << strerror(errno);
                }
            }
        }

        // Handle HBM based on screen state
        if (isScreenOn) {
            // SCREEN ON: Use /proc node for local HBM
            int fd = open(HBM_CTRL_NODE, O_WRONLY);
            if (fd >= 0) {
                if (pressed) {
                    // ID 54: mi,mdss-dsi-local-hbm-normal-white-1000nit-command
                    write(fd, "54\n", 3);
                    LOG(INFO) << "💡 Screen ON: Local HBM enabled (ID 54)";
                } else {
                    // ID 71: mi,mdss-dsi-local-hbm-off-to-hbm-command
                    write(fd, "71\n", 3);
                    LOG(INFO) << "💡 Screen ON: Local HBM disabled (ID 71)";
                }
                close(fd);
            } else {
                LOG(ERROR) << "❌ Failed to open HBM_CTRL_NODE";
            }
        } else {
            // SCREEN OFF: Use display framework IOCTL
            std::lock_guard<std::mutex> lock(disp_mutex_);
            if (disp_fd_.get() >= 0) {
                disp_local_hbm_req req;
                req.base.flag = 0;
                req.base.disp_id = MI_DISP_PRIMARY;
                req.local_hbm_value = pressed ? LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT
                                              : LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
                if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) < 0) {
                    LOG(ERROR) << "Failed to set HBM: " << strerror(errno);
                }
            }
        }

        // Always notify fingerprint device
        {
            std::lock_guard<std::mutex> lock(device_mutex_);
            if (mDevice != nullptr) {
                mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS,
                              pressed ? PARAM_FOD_PRESSED : PARAM_FOD_RELEASED);
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
