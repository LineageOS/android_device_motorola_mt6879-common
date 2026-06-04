//
// Copyright (C) 2023 LibreMobileOS Foundation
//
// SPDX-License-Identifier: Apache-2.0
//

#include "BiometricsPerf.h"

namespace android {
namespace hardware {
namespace biometrics {
namespace fingerprint {
namespace V2_3 {
namespace implementation {

BiometricsPerf::BiometricsPerf() {
    connectPowerHal();
}

BiometricsPerf::~BiometricsPerf() {
    mPowerHalExtAidl = nullptr;
    mPowerHalAidl = nullptr;
}

bool BiometricsPerf::connectPowerHal() {
    if (mPowerHalAidl && mPowerHalExtAidl) {
        return true;
    }

    const std::string kInstance = std::string(IPower::descriptor) + "/default";
    ndk::SpAIBinder pwBinder = ndk::SpAIBinder(AServiceManager_getService(kInstance.c_str()));

    mPowerHalAidl = IPower::fromBinder(pwBinder);

    if (!mPowerHalAidl) {
        ALOGE("failed to connect power HAL");
        return false;
    }

    ndk::SpAIBinder pwExtBinder;
    AIBinder_getExtension(pwBinder.get(), pwExtBinder.getR());

    mPowerHalExtAidl = IPowerExt::fromBinder(pwExtBinder);

    if (!mPowerHalExtAidl) {
        mPowerHalAidl = nullptr;
        ALOGE("failed to connect power HAL extension");
        return false;
    }

    ALOGE("connected power HAL successfully");
    enableBoost();
    return true;
}

int BiometricsPerf::setPowerHalExtHint(const std::string &hint,
                                        int durationMs) {
    if (hint.empty() || !connectPowerHal()) {
        ALOGE("ERROR");
        return -EINVAL;
    }
    ALOGE("sending hint");
    auto ret = mPowerHalExtAidl->setBoost(hint.c_str(), durationMs);
    if (!ret.isOk()) {
        ALOGE("failed to send power HAL extension hint: mode=%s, duration=%d", hint.c_str(),
              durationMs);
        if (ret.getExceptionCode() == EX_TRANSACTION_FAILED) {
            /*
             * PowerHAL service may crash due to some reasons, this could end up
             * binder transaction failure. Set nullptr here to trigger re-connection.
             */
            ALOGE("binder transaction failed for power HAL extension hint");
            mPowerHalExtAidl = nullptr;
            return -ENOTCONN;
        }
        return -EINVAL;
    }
    ALOGE("done hint");
    return 0;
}

void BiometricsPerf::enableBoost() {
    setPowerHalExtHint("FP_BOOST", kDefaultBoostDurationMs);
}

}  // namespace implementation
}  // namespace V2_3
}  // namespace fingerprint
}  // namespace biometrics
}  // namespace hardware
}  // namespace android
