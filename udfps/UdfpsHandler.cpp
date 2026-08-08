/*
 * Copyright (C) 2024 Cedric Loste
 *
 * Based on original LineageOS UdfpsHandler
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "UdfpsHandler.xiaomi_sm6225"

#include <android-base/logging.h>
#include <android-base/properties.h>

#include "UdfpsHandler.h"
#include "GoodixUdfpsHandler.h"
#include "FpcUdfpsHandler.h"

static UdfpsHandler* create() {
    std::string fpVendor = android::base::GetProperty("persist.vendor.sys.fp.vendor", "none");
    LOG(INFO) << "Detected Fingerprint vendor: " << fpVendor;
    
    if (fpVendor == "fpc_fod") {
        LOG(INFO) << "Routing to FPC UDFPS Handler";
        return new FpcUdfpsHandler();
    } else if (fpVendor == "goodix_fod") {
        LOG(INFO) << "Routing to Goodix UDFPS Handler";
        return new GoodixUdfpsHandler();
    } else {
        LOG(ERROR) << "Unknown or missing fingerprint vendor property: " << fpVendor;
        return nullptr; 
    }
}

static void destroy(UdfpsHandler* handler) {
    if (handler != nullptr) {
        delete handler;
    }
}

extern "C" UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
    .create = create,
    .destroy = destroy,
};
