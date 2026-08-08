/*
 * Copyright (C) 2019-2020 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "vendor.lineage.livedisplay-service.xiaomi_sm6225"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <binder/ProcessState.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <thread>
#include <cstring>
#include <livedisplay/sdm/PictureAdjustment.h>

#include "AntiFlicker.h"
#include "SunlightEnhancement.h"
#include "mi_disp.h"

using ::aidl::vendor::lineage::livedisplay::AntiFlicker;
using ::aidl::vendor::lineage::livedisplay::SunlightEnhancement;
using ::aidl::vendor::lineage::livedisplay::sdm::PictureAdjustment;
using ::aidl::vendor::lineage::livedisplay::sdm::SDMController;

static constexpr const char* kDispFeaturePath = "/dev/mi_display/disp_feature";

// Background worker to monitor refresh rate changes via mi_display and adjust SDM hue
void watchRefreshRateAndTune(std::shared_ptr<SDMController> controller) {
    if (!controller) return;

    int fd = -1;
    while (fd < 0) {
        fd = open(kDispFeaturePath, O_RDWR);
        if (fd < 0) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    struct disp_event_req reg_req;
    memset(&reg_req, 0, sizeof(reg_req));
    reg_req.base.disp_id = MI_DISP_PRIMARY;
    reg_req.type = MI_DISP_EVENT_FPS;

    if (ioctl(fd, MI_DISP_IOCTL_REGISTER_EVENT, &reg_req) < 0) {
        LOG(ERROR) << "Failed to register for mi_display FPS events in service lifecycle";
        close(fd);
        return;
    }

    char buffer[256];
    while (true) {
        ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
        if (bytes_read > (ssize_t)sizeof(struct disp_event)) {
            struct disp_event_resp* resp = reinterpret_cast<struct disp_event_resp*>(buffer);
            
            if (resp->base.type == MI_DISP_EVENT_FPS && resp->base.length >= sizeof(uint32_t)) {
                uint32_t current_fps = *reinterpret_cast<uint32_t*>(resp->data);

                if (current_fps == 90) {
                    LOG(INFO) << "90Hz active: adjusting SDM hue to 5.";
                    controller->setPictureAdjustment(5.0f, 0.0f, 0.0f, 0.0f);
                } else {
                    LOG(INFO) << "Restoring default SDM profile for " << current_fps << "Hz.";
                    controller->setPictureAdjustment(0.0f, 0.0f, 0.0f, 0.0f);
                }
            }
        }
    }
    close(fd);
}

int main() {
    android::ProcessState::self()->setThreadPoolMaxThreadCount(1);
    android::ProcessState::self()->startThreadPool();

    std::shared_ptr<SDMController> controller = std::make_shared<SDMController>();

    // Spawn the refresh rate monitor thread in the background
    std::thread(watchRefreshRateAndTune, controller).detach();

    std::shared_ptr<PictureAdjustment> pictureAdjustment =
            ndk::SharedRefBase::make<PictureAdjustment>(controller);
    std::string instance = std::string() + PictureAdjustment::descriptor + "/default";
    if (AServiceManager_addService(pictureAdjustment->asBinder().get(), instance.c_str()) !=
        STATUS_OK) {
        LOG(ERROR) << "Cannot register picture adjustment HAL service.";
        return 1;
    }

#ifdef SUPPORT_ANTI_FLICKER
    std::shared_ptr<AntiFlicker> antiFlicker = ndk::SharedRefBase::make<AntiFlicker>();
    instance = std::string() + AntiFlicker::descriptor + "/default";
    if (AServiceManager_addService(antiFlicker->asBinder().get(), instance.c_str()) != STATUS_OK) {
        LOG(ERROR) << "Cannot register anti flicker HAL service.";
        return 1;
    }
#endif

#ifdef SUPPORT_SUNLIGHT_ENHANCEMENT
    std::shared_ptr<SunlightEnhancement> sunlightEnhancement =
            ndk::SharedRefBase::make<SunlightEnhancement>();
    instance = std::string() + SunlightEnhancement::descriptor + "/default";
    if (AServiceManager_addService(sunlightEnhancement->asBinder().get(), instance.c_str()) !=
        STATUS_OK) {
        LOG(ERROR) << "Cannot register sunlight enhancement HAL service.";
        return 1;
    }
#endif
    LOG(INFO) << "LiveDisplay HAL service is ready.";

    ABinderProcess_joinThreadPool();

    LOG(ERROR) << "LiveDisplay HAL service failed to join thread pool.";
    return 1;
}
