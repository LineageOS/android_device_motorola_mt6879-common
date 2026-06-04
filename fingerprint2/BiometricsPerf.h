//
// Copyright (C) 2023 LibreMobileOS Foundation
//
// SPDX-License-Identifier: Apache-2.0
//

#ifndef ANDROID_HARDWARE_BIOMETRICS_FINGERPRINT_V2_3_BIOMETRICSPERF_H
#define ANDROID_HARDWARE_BIOMETRICS_FINGERPRINT_V2_3_BIOMETRICSPERF_H

#include <android/binder_manager.h>
#include <android/log.h>
#include <log/log.h>
#include <aidl/android/hardware/power/IPower.h>
#include <aidl/google/hardware/power/extension/pixel/IPowerExt.h>

using ::aidl::android::hardware::power::IPower;
using ::aidl::google::hardware::power::extension::pixel::IPowerExt;

namespace android {
namespace hardware {
namespace biometrics {
namespace fingerprint {
namespace V2_3 {
namespace implementation {

static constexpr int kDefaultBoostDurationMs = 700;

struct BiometricsPerf {
    BiometricsPerf();
    ~BiometricsPerf();
    void enableBoost();

private:
    std::shared_ptr<IPower> mPowerHalAidl;
    std::shared_ptr<IPowerExt> mPowerHalExtAidl;

    bool connectPowerHal();
    int setPowerHalExtHint(const std::string &mode, int durationMs);
 };

}  // namespace implementation
}  // namespace V2_3
}  // namespace fingerprint
}  // namespace biometrics
}  // namespace hardware
}  // namespace android
#endif