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

GoodixUdfpsHandler::GoodixUdfpsHandler() : mDevice(nullptr) {}

GoodixUdfpsHandler::~GoodixUdfpsHandler() {
    LOG(INFO) << "Destructor called, shutting down threads";
    shutdownThreads();
}

void GoodixUdfpsHandler::init(fingerprint_device_t* device) {
    LOG(INFO) << "Initializing Goodix UDFPS handler";
    
    mDevice = device;
    
    touch_fd_ = android::base::unique_fd(open(TOUCH_DEV_PATH, O_RDWR));
    if (touch_fd_.get() < 0) {
        LOG(ERROR) << "Failed to open touch device: " << strerror(errno);
    }

    disp_fd_ = android::base::unique_fd(open(DISP_FEATURE_PATH, O_RDWR));
    if (disp_fd_.get() < 0) {
        LOG(ERROR) << "Failed to open display device: " << strerror(errno);
    }

    fodThread_ = std::thread([this]() { fodPressMonitorThread(); });
    dispThread_ = std::thread([this]() { displayEventMonitorThread(); });
    
    LOG(INFO) << "Goodix UDFPS handler initialized";
}

void GoodixUdfpsHandler::onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) {
    LOG(INFO) << __func__;
    
    mFbDownTimeMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    setFingerDown(true);
}

void GoodixUdfpsHandler::onFingerUp() {
    LOG(INFO) << __func__;
    
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t elapsed = now - mFbDownTimeMs.load();
    
    if (elapsed < 250) {
        LOG(INFO) << "UDFPS: Ignorando falso UP del framework (pasaron " << elapsed << "ms)";
        return;
    }

    setFingerDown(false);
}

void GoodixUdfpsHandler::onAcquired(int32_t result, int32_t vendorCode) {
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
            setFodStatus(FOD_STATUS_OFF);
        }
    }

    if (vendorCode == 21) {
        setFodStatus(FOD_STATUS_ON);
    }
}

void GoodixUdfpsHandler::cancel() {
    LOG(INFO) << __func__;
    enrolling.store(false);
    setFodStatus(FOD_STATUS_OFF);
}

void GoodixUdfpsHandler::preEnroll() {
    LOG(INFO) << __func__;
    enrolling.store(true);
}

void GoodixUdfpsHandler::enroll() {
    LOG(INFO) << __func__;
    enrolling.store(true);
}

void GoodixUdfpsHandler::postEnroll() {
    LOG(INFO) << __func__;
    enrolling.store(false);
    setFodStatus(FOD_STATUS_OFF);
}

int GoodixUdfpsHandler::getBrightness() {
    int fd = open(BRIGHTNESS_PATH, O_RDONLY);
    if (fd < 0) return -1;
    char buf[12];
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0) return -1;
    buf[len] = '\0';
    return atoi(buf);
}

void GoodixUdfpsHandler::shutdownThreads() {
    isRunning.store(false);
    if (fodThread_.joinable()) {
        fodThread_.join();
    }
    if (dispThread_.joinable()) {
        dispThread_.join();
    }
}

void GoodixUdfpsHandler::fodPressMonitorThread() {
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
        if (!isScreenOffEnabled && getBrightness() == 0) {
            LOG(INFO) << "UDFPS: Toque ignorado. Screen-Off desactivado.";
            continue;
        }

        LOG(DEBUG) << "fod_press_status changed: " << (pressed ? "pressed" : "released");
        setFingerDown(pressed);
    }

    close(fd);
    LOG(INFO) << "FOD press monitor thread stopped";
}

void GoodixUdfpsHandler::displayEventMonitorThread() {
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

void GoodixUdfpsHandler::setFodStatus(int value) {
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

void GoodixUdfpsHandler::setFingerDown(bool pressed) {
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
}
