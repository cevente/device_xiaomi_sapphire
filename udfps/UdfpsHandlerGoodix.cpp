/*
 * Copyright (C) 2024 The LineageOS Project
 *
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

#include <chrono>
#include <cstring>

#include <display/drm/mi_disp.h>

#include "GoodixUdfpsHandler.h"
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

GoodixUdfpsHandler::GoodixUdfpsHandler() : mDevice(nullptr) {
    LOG(INFO) << "GoodixUdfpsHandler constructor called";
}

GoodixUdfpsHandler::~GoodixUdfpsHandler() {
    LOG(INFO) << "GoodixUdfpsHandler destructor called, shutting down threads";
    shutdownThreads();
}

void GoodixUdfpsHandler::init(fingerprint_device_t* device) {
    LOG(INFO) << "Initializing Goodix UDFPS handler";
    
    mDevice = device;
    
    touch_fd_ = android::base::unique_fd(open(TOUCH_DEV_PATH, O_RDWR));
    if (touch_fd_.get() < 0) {
        LOG(ERROR) << "Failed to open touch device: " << strerror(errno);
    } else {
        LOG(INFO) << "Touch device opened successfully, fd: " << touch_fd_.get();
    }

    disp_fd_ = android::base::unique_fd(open(DISP_FEATURE_PATH, O_RDWR));
    if (disp_fd_.get() < 0) {
        LOG(ERROR) << "Failed to open display device: " << strerror(errno);
    } else {
        LOG(INFO) << "Display device opened successfully, fd: " << disp_fd_.get();
    }

    fodThread_ = std::thread([this]() { fodPressMonitorThread(); });
    dispThread_ = std::thread([this]() { displayEventMonitorThread(); });
    
    LOG(INFO) << "Goodix UDFPS handler initialized successfully";
}

void GoodixUdfpsHandler::onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) {
    LOG(INFO) << __func__ << " - Finger down event from framework";
    
    mFbDownTimeMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    setFingerDown(true);
}

void GoodixUdfpsHandler::onFingerUp() {
    LOG(INFO) << __func__ << " - Finger up event from framework";
    
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t elapsed = now - mFbDownTimeMs.load();
    
    if (elapsed < 250) {
        LOG(INFO) << "UDFPS: Ignoring false UP from framework (" << elapsed << "ms elapsed)";
        return;
    }

    setFingerDown(false);
}

void GoodixUdfpsHandler::onAcquired(int32_t result, int32_t vendorCode) {
    LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;
    
    if (static_cast<AcquiredInfo>(result) == AcquiredInfo::GOOD) {
        LOG(INFO) << "Acquired GOOD - Successful fingerprint capture";
        {
            std::lock_guard<std::mutex> lock(disp_mutex_);
            if (disp_fd_.get() >= 0) {
                disp_local_hbm_req req;
                req.base.flag = 0;
                req.base.disp_id = MI_DISP_PRIMARY;
                req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
                if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) == 0) {
                    LOG(DEBUG) << "HBM disabled after successful capture";
                } else {
                    LOG(ERROR) << "Failed to disable HBM";
                }
            }
        }
        
        if (!enrolling.load()) {
            LOG(DEBUG) << "Not enrolling, turning FOD off";
            setFodStatus(FOD_STATUS_OFF);
        }
    }

    if (vendorCode == 21) {
        LOG(INFO) << "Vendor code 21 - FOD status ON";
        setFodStatus(FOD_STATUS_ON);
    }
}

void GoodixUdfpsHandler::cancel() {
    LOG(INFO) << __func__ << " - Canceling fingerprint operation";
    enrolling.store(false);
    setFodStatus(FOD_STATUS_OFF);
}

void GoodixUdfpsHandler::preEnroll() {
    LOG(INFO) << __func__ << " - Pre-enroll started";
    enrolling.store(true);
}

void GoodixUdfpsHandler::enroll() {
    LOG(INFO) << __func__ << " - Enrollment started";
    enrolling.store(true);
}

void GoodixUdfpsHandler::postEnroll() {
    LOG(INFO) << __func__ << " - Post-enroll completed";
    enrolling.store(false);
    setFodStatus(FOD_STATUS_OFF);
}

int GoodixUdfpsHandler::getBrightness() {
    int fd = open(BRIGHTNESS_PATH, O_RDONLY);
    if (fd < 0) {
        LOG(ERROR) << "Failed to open brightness path: " << strerror(errno);
        return -1;
    }
    char buf[12];
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0) {
        LOG(ERROR) << "Failed to read brightness value";
        return -1;
    }
    buf[len] = '\0';
    int brightness = atoi(buf);
    LOG(VERBOSE) << "Current brightness: " << brightness;
    return brightness;
}

void GoodixUdfpsHandler::shutdownThreads() {
    LOG(INFO) << "Shutting down threads";
    isRunning.store(false);
    if (fodThread_.joinable()) {
        LOG(INFO) << "Joining FOD press monitor thread";
        fodThread_.join();
    }
    if (dispThread_.joinable()) {
        LOG(INFO) << "Joining display event monitor thread";
        dispThread_.join();
    }
    LOG(INFO) << "All threads shut down";
}

void GoodixUdfpsHandler::fodPressMonitorThread() {
    LOG(INFO) << "FOD press monitor thread started (PID: " << getpid() << ")";
    
    int fd = open(FOD_PRESS_STATUS_PATH, O_RDONLY);
    if (fd < 0) {
        LOG(ERROR) << "Failed to open " << FOD_PRESS_STATUS_PATH 
                   << ", error: " << strerror(errno);
        return;
    }
    LOG(INFO) << "Opened " << FOD_PRESS_STATUS_PATH << " successfully, fd: " << fd;

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

        if (rc == 0) {
            LOG(VERBOSE) << "FOD press poll timeout (no event)";
            continue;
        }

        if (!(fodPressStatusPoll.revents & (POLLERR | POLLPRI))) {
            if (fodPressStatusPoll.revents & (POLLHUP | POLLNVAL)) {
                LOG(ERROR) << "Poll error event: " << fodPressStatusPoll.revents;
                break;
            }
            LOG(WARNING) << "Unexpected poll revents: " << fodPressStatusPoll.revents;
            fodPressStatusPoll.revents = 0;
            continue;
        }

        fodPressStatusPoll.revents = 0;

        const bool pressed = readBool(fd);
        uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        
        LOG(DEBUG) << "FOD press status changed: " << (pressed ? "PRESSED" : "RELEASED");
        
        if (pressed) {
            mFbDownTimeMs.store(now);
        } else {
            uint64_t elapsed = now - mFbDownTimeMs.load();
            if (elapsed < 250) {
                LOG(INFO) << "UDFPS: Hardware UP too fast (" << elapsed << "ms). Waiting 100ms...";
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (readBool(fd)) {
                    LOG(INFO) << "UDFPS: Finger still present! False physical UP ignored.";
                    continue; 
                }
                LOG(INFO) << "UDFPS: Finger is truly gone after waiting";
            }
        }

        bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);
        int brightness = getBrightness();
        LOG(DEBUG) << "Screen off enabled: " << isScreenOffEnabled << ", brightness: " << brightness;
        
        if (!isScreenOffEnabled && brightness == 0) {
            LOG(INFO) << "UDFPS: Touch ignored. Screen-Off disabled.";
            continue;
        }

        setFingerDown(pressed);
    }

    close(fd);
    LOG(INFO) << "FOD press monitor thread stopped";
}

void GoodixUdfpsHandler::displayEventMonitorThread() {
    LOG(INFO) << "Display event monitor thread started (PID: " << getpid() << ")";
    
    int fd = open(DISP_FEATURE_PATH, O_RDWR);
    if (fd < 0) {
        LOG(ERROR) << "Failed to open " << DISP_FEATURE_PATH 
                   << ", error: " << strerror(errno);
        return;
    }
    LOG(INFO) << "Opened " << DISP_FEATURE_PATH << " successfully, fd: " << fd;

    disp_event_req req;
    req.base.flag = 0;
    req.base.disp_id = MI_DISP_PRIMARY;
    req.type = MI_DISP_EVENT_FOD;
    if (ioctl(fd, MI_DISP_IOCTL_REGISTER_EVENT, &req) < 0) {
        LOG(ERROR) << "Failed to register for display events: " << strerror(errno);
        close(fd);
        return;
    }
    LOG(INFO) << "Registered for display events successfully";

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

        if (rc == 0) {
            LOG(VERBOSE) << "Display poll timeout (no event)";
            continue;
        }

        if (!(dispEventPoll.revents & POLLIN)) {
            if (dispEventPoll.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                LOG(ERROR) << "Display poll error: " << dispEventPoll.revents;
                break;
            }
            LOG(WARNING) << "Unexpected display poll revents: " << dispEventPoll.revents;
            dispEventPoll.revents = 0;
            continue;
        }

        dispEventPoll.revents = 0;

        struct disp_event_resp* response = parseDispEvent(fd);
        if (response == nullptr) {
            LOG(ERROR) << "Failed to parse display event";
            continue;
        }

        if (response->base.type != MI_DISP_EVENT_FOD) {
            LOG(WARNING) << "Unexpected display event type: " << response->base.type;
            continue;
        }

        int value = response->data[0];
        LOG(INFO) << "Display event received, data: 0x" << std::hex << value << std::dec;

        bool localHbmUiReady = value & LOCAL_HBM_UI_READY;
        LOG(INFO) << "Local HBM UI Ready: " << (localHbmUiReady ? "YES" : "NO");
        
        std::lock_guard<std::mutex> deviceLock(device_mutex_);
        if (mDevice != nullptr) {
            int cmd = localHbmUiReady ? PARAM_NIT_FOD : PARAM_NIT_NONE;
            LOG(DEBUG) << "Sending NIT command: " << cmd;
            mDevice->extCmd(mDevice, COMMAND_NIT, cmd);
        } else {
            LOG(ERROR) << "Device is null, cannot send NIT command";
        }
    }

    close(fd);
    LOG(INFO) << "Display event monitor thread stopped";
}

void GoodixUdfpsHandler::setFodStatus(int value) {
    LOG(INFO) << __func__ << " - Setting FOD status to: " << (value ? "ON" : "OFF");
    
    std::lock_guard<std::mutex> lock(touch_mutex_);
    
    if (touch_fd_.get() < 0) {
        LOG(ERROR) << "Touch device not opened, cannot set FOD status";
        return;
    }

    // 1. Original FOD command
    int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, value};
    if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
        LOG(ERROR) << "Failed to set FOD status: " << strerror(errno);
    } else {
        LOG(INFO) << "Successfully set FOD status to " << (value ? "ON" : "OFF");
    }

    // 2. INJECTED D2TW RESTORE: Force panel to keep gesture polling alive
    LOG(INFO) << "Restoring Double-Tap to Wake state (D2TW)";
    int d2twBuf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Doubletap_Mode, 1};
    if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &d2twBuf) < 0) {
        LOG(ERROR) << "Failed to restore D2TW state: " << strerror(errno);
    } else {
        LOG(INFO) << "D2TW restored successfully";
    }
}

void GoodixUdfpsHandler::setFingerDown(bool pressed) {
    LOG(INFO) << __func__ << " - Finger " << (pressed ? "DOWN" : "UP");
    
    {
        std::lock_guard<std::mutex> lock(touch_mutex_);
        if (touch_fd_.get() >= 0) {
            int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, pressed ? 1 : 0};
            if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
                LOG(ERROR) << "Failed to set finger down/up: " << strerror(errno);
            } else {
                LOG(DEBUG) << "Successfully set finger " << (pressed ? "DOWN" : "UP");
            }
        } else {
            LOG(ERROR) << "Touch device not opened, cannot set finger state";
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
            } else {
                LOG(DEBUG) << "HBM " << (pressed ? "enabled" : "disabled");
            }
        } else {
            LOG(ERROR) << "Display device not opened, cannot set HBM";
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
}
