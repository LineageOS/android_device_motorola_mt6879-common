/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "libudfpshandler"

#include "UdfpsHandler.h"
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <com/motorola/hardware/biometric/fingerprint/1.0/IMotoFingerPrint.h>
#include <drm/mediatek_drm.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <thread>
#include <mutex>
#include <vector>

using ::com::motorola::hardware::biometric::fingerprint::V1_0::IMotFodEventResult;
using ::com::motorola::hardware::biometric::fingerprint::V1_0::IMotFodEventType;
using ::com::motorola::hardware::biometric::fingerprint::V1_0::IMotoFingerPrint;
using ::android::sp;
using ::android::hardware::hidl_vec;

#define NOTIFY_FINGER_UP IMotFodEventType::FINGER_UP
#define NOTIFY_FINGER_DOWN IMotFodEventType::FINGER_DOWN

enum HBM_STATE { OFF = 0, ON = 2 };

namespace {
void setHbmState(int state) {
    struct panel_param_info param_info;
    int32_t node = open("/dev/dri/card0", O_RDWR);
    int32_t ret = 0;

    if (node < 0) {
        LOG(ERROR) << "Failed to get card0!";
        return;
    }

    param_info.param_idx = PARAM_HBM;
    param_info.value = state;

    ret = ioctl(node, DRM_IOCTL_SET_PANEL_FEATURE, &param_info);
    if (ret < 0) {
        LOG(ERROR) << "IOCTL call failed with ret = " << ret;
    } else {
        LOG(INFO) << "HBM state set successfully. New state: " << state;
    }

    close(node);
}
} // namespace

class ManausUdfpsHandler : public UdfpsHandler {
  public:
    ManausUdfpsHandler() : hbmFodEnabled(false), mMotoFingerprint(nullptr) {
        mIsEgis = android::base::GetProperty("vendor.hw.fps.ident", "") == "egis";
    }

    ~ManausUdfpsHandler() override {
        disableHighBrightFod();
    }

    void onFingerDown(uint32_t x, uint32_t y, float minor, float major) override {
        (void)x;
        (void)y;
        (void)minor;
        (void)major;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        enableHighBrightFod();

        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            onFingerUp();
        }).detach();
    }

    void onFingerUp() override {
        disableHighBrightFod();
    }

    void cancel() override {
        onFingerUp();
    }

  private:
    sp<IMotoFingerPrint> getMotoFingerprint() {
        if (!mMotoFingerprint) {
            mMotoFingerprint = IMotoFingerPrint::tryGetService();
            if (!mMotoFingerprint) {
                LOG(WARNING) << "Failed to get IMotoFingerPrint service via tryGetService";
            }
        }
        return mMotoFingerprint;
    }

    void extraApiWrapper(int cidValue) {
        void* rbs_handle = dlopen("libRbsFlow.so", RTLD_NOW);
        if (!rbs_handle) {
            LOG(WARNING) << "Failed to dlopen libRbsFlow.so: " << dlerror();
            return;
        }

        typedef int (*rbs_extra_api_t)(uint32_t, const uint8_t*, uint32_t, uint8_t*, uint32_t*);
        auto extra_api = reinterpret_cast<rbs_extra_api_t>(dlsym(rbs_handle, "rbs_extra_api"));
        if (extra_api) {
            int cid[1] = {cidValue};
            int rc = extra_api(7, reinterpret_cast<const uint8_t*>(cid), sizeof(cid), nullptr, nullptr);
            if (rc != 0) {
                LOG(ERROR) << "rbs_extra_api(7, " << cidValue << ") failed, error: " << rc;
            }
        } else {
            LOG(ERROR) << "Failed to dlsym rbs_extra_api: " << dlerror();
        }
        dlclose(rbs_handle);
    }

    void disableHighBrightFod() {
        std::lock_guard<std::mutex> lock(mSetHbmFodMutex);

        if (!hbmFodEnabled)
            return;

        if (mIsEgis) {
            extraApiWrapper(102);
        } else {
            auto motoFingerprint = getMotoFingerprint();
            if (motoFingerprint) {
                motoFingerprint->sendFodEvent(NOTIFY_FINGER_UP, {},
                                               [](IMotFodEventResult, const hidl_vec<signed char> &) {});
            }
        }
        setHbmState(OFF);

        hbmFodEnabled = false;
    }

    void enableHighBrightFod() {
        std::lock_guard<std::mutex> lock(mSetHbmFodMutex);

        if (hbmFodEnabled)
            return;

        setHbmState(ON);
        if (mIsEgis) {
            extraApiWrapper(101);
        } else {
            auto motoFingerprint = getMotoFingerprint();
            if (motoFingerprint) {
                motoFingerprint->sendFodEvent(NOTIFY_FINGER_DOWN, {},
                                               [](IMotFodEventResult, const hidl_vec<signed char> &) {});
            }
        }

        hbmFodEnabled = true;
    }

    bool hbmFodEnabled;
    bool mIsEgis;
    std::mutex mSetHbmFodMutex;
    sp<IMotoFingerPrint> mMotoFingerprint;
};

static UdfpsHandler* create_handler() {
    return new ManausUdfpsHandler();
}

static void destroy_handler(UdfpsHandler* handler) {
    delete handler;
}

extern "C" UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
    .create = create_handler,
    .destroy = destroy_handler,
};
