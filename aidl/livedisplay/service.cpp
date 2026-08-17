/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.livedisplay-service.mt6879"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <binder/ProcessState.h>

#include "AntiFlicker.h"
#include "SunlightEnhancement.h"

using ::aidl::vendor::lineage::livedisplay::AntiFlicker;
using ::aidl::vendor::lineage::livedisplay::SunlightEnhancement;

int main() {
    android::ProcessState::self()->setThreadPoolMaxThreadCount(1);
    android::ProcessState::self()->startThreadPool();

    std::shared_ptr<AntiFlicker> antiFlicker =
            ndk::SharedRefBase::make<AntiFlicker>();
    std::shared_ptr<SunlightEnhancement> sunlightEnhancement =
            ndk::SharedRefBase::make<SunlightEnhancement>();
    binder_status_t status;

    LOG(INFO) << "LiveDisplay HAL service is starting.";

    if (antiFlicker == nullptr) {
        LOG(ERROR) << "Can not create an instance of LiveDisplay HAL AntiFlicker Iface, exiting.";
        goto shutdown;
    }

    if (sunlightEnhancement == nullptr) {
        LOG(ERROR) << "Can not create an instance of LiveDisplay HAL SunlightEnhancement Iface, exiting.";
        goto shutdown;
    }

    {
        std::string instance = std::string(AntiFlicker::descriptor) + "/default";
        status = AServiceManager_addService(antiFlicker->asBinder().get(), instance.c_str());
        if (status != STATUS_OK) {
            LOG(ERROR) << "Could not register service for LiveDisplay HAL AntiFlicker Iface ("
                       << status << ")";
            goto shutdown;
        }
    }

    {
        std::string instance = std::string(SunlightEnhancement::descriptor) + "/default";
        status = AServiceManager_addService(sunlightEnhancement->asBinder().get(), instance.c_str());
        if (status != STATUS_OK) {
            LOG(ERROR) << "Could not register service for LiveDisplay HAL SunlightEnhancement Iface ("
                       << status << ")";
            goto shutdown;
        }
    }

    LOG(INFO) << "LiveDisplay HAL service is ready.";
    ABinderProcess_joinThreadPool();
    // Should not pass this line

shutdown:
    LOG(ERROR) << "LiveDisplay HAL service is shutting down.";
    return 1;
}
