/*
 * Copyright (C) 2024 The LineageOS Project
 *               2024 Paranoid Android
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>

#include "FingerprintConfig.h"
#include "LockoutTracker.h"
#include "Session.h"
#include "UdfpsHandler.h"

using ::aidl::android::hardware::biometrics::fingerprint::FingerprintSensorType;
using ::aidl::android::hardware::biometrics::fingerprint::ISession;
using ::aidl::android::hardware::biometrics::fingerprint::ISessionCallback;
using ::aidl::android::hardware::biometrics::fingerprint::SensorProps;

namespace aidl::android::hardware::biometrics::fingerprint {

class Fingerprint : public BnFingerprint {
  public:
    Fingerprint(std::shared_ptr<FingerprintConfig> config);
    ~Fingerprint();

    ndk::ScopedAStatus getSensorProps(std::vector<SensorProps>* _aidl_return) override;
    ndk::ScopedAStatus createSession(int32_t sensorId, int32_t userId,
                                     const std::shared_ptr<ISessionCallback>& cb,
                                     std::shared_ptr<ISession>* out) override;

  private:
    fingerprint_device_t* openFingerprintHal(const char* class_name, const char* module_id);
    rbs_fingerprint_device_t* openRbsFingerprintHal();
    anc_fingerprint_device_t* openAncFingerprintHal(fingerprint_device_t** outDev);
    std::vector<SensorLocation> getSensorLocations();
    static void notify(const fingerprint_msg_t* msg);
    static void rbsNotify(uint32_t eventId, uint32_t value1, uint32_t value2, void* buffer, uint32_t buffer_size);
    void handleRbsNotify(uint32_t eventId, uint32_t value1, uint32_t value2, void* buffer, uint32_t buffer_size);

    std::shared_ptr<FingerprintConfig> mConfig;
    std::shared_ptr<Session> mSession;
    LockoutTracker mLockoutTracker;
    FingerprintSensorType mSensorType;

    fingerprint_device_t* mDevice;
    rbs_fingerprint_device_t* mRbsDevice;
    anc_fingerprint_device_t* mAncDevice;
    UdfpsHandlerFactory* mUdfpsHandlerFactory;
    UdfpsHandler* mUdfpsHandler;
};

}  // namespace aidl::android::hardware::biometrics::fingerprint
