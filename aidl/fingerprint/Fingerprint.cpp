/*
 * Copyright (C) 2024 The LineageOS Project
 *               2024 Paranoid Android
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Fingerprint.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <dlfcn.h>
#include <fingerprint.sysprop.h>
#include <unistd.h>
#include "util/Util.h"

namespace aidl::android::hardware::biometrics::fingerprint {

namespace {
constexpr int MAX_ENROLLMENTS_PER_USER = 5;
constexpr char HW_COMPONENT_ID[] = "fingerprintSensor";
constexpr char HW_VERSION[] = "vendor/model/revision";
constexpr char FW_VERSION[] = "1.01";
constexpr char SERIAL_NUMBER[] = "00000001";
constexpr char SW_COMPONENT_ID[] = "matchingAlgorithm";
constexpr char SW_VERSION[] = "vendor/version/revision";

typedef struct fingerprint_hal {
    const char* class_name;
} fingerprint_hal_t;

static const fingerprint_hal_t kModules[] = {
        {"fortsense"},  {"fpc"},         {"fpc_fod"}, {"goodix"}, {"goodix:gf_fingerprint"},
        {"goodix_fod"}, {"goodix_fod6"}, {"silead"},  {"syna"},
};

}  // namespace

static const uint16_t kVersion = HARDWARE_MODULE_API_VERSION(2, 1);
static Fingerprint* sInstance;

Fingerprint::Fingerprint(std::shared_ptr<FingerprintConfig> config)
    : mConfig(std::move(config)), mDevice(nullptr), mRbsDevice(nullptr), mAncDevice(nullptr) {
    sInstance = this;  // keep track of the most recent instance

    mRbsDevice = openRbsFingerprintHal();
    if (!mRbsDevice) {
        mAncDevice = openAncFingerprintHal(&mDevice);
    }

    if (!mRbsDevice && !mAncDevice) {
        if (mDevice) {
            ALOGI("fingerprint HAL already opened");
        } else {
            for (auto& [module] : kModules) {
                std::string class_name;
                std::string class_module_id;

                auto parts = ::android::base::Split(module, ":");

                if (parts.size() == 2) {
                    class_name = parts[0];
                    class_module_id = parts[1];
                } else {
                    class_name = module;
                    class_module_id = FINGERPRINT_HARDWARE_MODULE_ID;
                }

                mDevice = openFingerprintHal(class_name.c_str(), class_module_id.c_str());
                if (!mDevice) {
                    ALOGE("Can't open HAL module, class: %s, module_id: %s", class_name.c_str(),
                          class_module_id.c_str());
                    continue;
                }
                ALOGI("Opened fingerprint HAL, class: %s, module_id: %s", class_name.c_str(),
                      class_module_id.c_str());
                break;
            }
            if (!mDevice) {
                ALOGE("Can't open any fingerprint HAL module");
                ::android::base::SetProperty("vendor.hw.fingerprint.status", "fail");
            }
        }
    }

    std::string sensorTypeProp = mConfig->get<std::string>("type");
    if (sensorTypeProp == "udfps" || sensorTypeProp == "udfps_optical") {
        if (sensorTypeProp == "udfps") {
            mSensorType = FingerprintSensorType::UNDER_DISPLAY_ULTRASONIC;
        } else {
            mSensorType = FingerprintSensorType::UNDER_DISPLAY_OPTICAL;
        }
        mUdfpsHandlerFactory = getUdfpsHandlerFactory();
        if (!mUdfpsHandlerFactory) {
            ALOGE("Can't get UdfpsHandlerFactory");
        } else {
            mUdfpsHandler = mUdfpsHandlerFactory->create();
            if (!mUdfpsHandler) {
                ALOGE("Can't create UdfpsHandler");
            } else if (mDevice) {
                mUdfpsHandler->init(mDevice);
            }
        }
    } else if (sensorTypeProp == "side") {
        mSensorType = FingerprintSensorType::POWER_BUTTON;
    } else if (sensorTypeProp == "home") {
        mSensorType = FingerprintSensorType::HOME_BUTTON;
    } else if (sensorTypeProp == "rear") {
        mSensorType = FingerprintSensorType::REAR;
    } else {
        mSensorType = FingerprintSensorType::UNKNOWN;
        UNIMPLEMENTED(FATAL) << "unrecognized or unimplemented fingerprint behavior: "
                             << sensorTypeProp;
    }
    ALOGI("sensorTypeProp: %s", sensorTypeProp.c_str());
}

Fingerprint::~Fingerprint() {
    ALOGV("~Fingerprint()");
    if (mUdfpsHandler) {
        mUdfpsHandlerFactory->destroy(mUdfpsHandler);
    }
    if (mRbsDevice) {
        mRbsDevice->rbs_uninitialize();
        free(mRbsDevice);
        mRbsDevice = nullptr;
    }
    if (mAncDevice) {
        if (mAncDevice->DeinitFingerprintDevice && mDevice) {
            mAncDevice->DeinitFingerprintDevice(mDevice);
        }
        free(mAncDevice);
        mAncDevice = nullptr;
    }
    if (mDevice != nullptr) {
        int err;
        if (0 != (err = mDevice->common.close(reinterpret_cast<hw_device_t*>(mDevice)))) {
            ALOGE("Can't close fingerprint module, error: %d", err);
        }
        mDevice = nullptr;
    }
}

fingerprint_device_t* Fingerprint::openFingerprintHal(const char* class_name,
                                                      const char* module_id) {
    const hw_module_t* hw_mdl = nullptr;

    ALOGD("Opening fingerprint hal library...");
    if (hw_get_module_by_class(module_id, class_name, &hw_mdl) != 0) {
        ALOGE("Can't open fingerprint HW Module");
        return nullptr;
    }

    if (!hw_mdl) {
        ALOGE("No valid fingerprint module");
        return nullptr;
    }

    auto module = reinterpret_cast<const fingerprint_module_t*>(hw_mdl);
    if (!module->common.methods->open) {
        ALOGE("No valid open method");
        return nullptr;
    }

    hw_device_t* device = nullptr;
    if (module->common.methods->open(hw_mdl, nullptr, &device) != 0) {
        ALOGE("Can't open fingerprint methods");
        return nullptr;
    }

    auto fp_device = reinterpret_cast<fingerprint_device_t*>(device);
    if (fp_device->set_notify(fp_device, Fingerprint::notify) != 0) {
        ALOGE("Can't register fingerprint module callback");
        return nullptr;
    }

    return fp_device;
}

rbs_fingerprint_device_t* Fingerprint::openRbsFingerprintHal() {
    bool has_egis = (access("/dev/egis_fp", F_OK) == 0 || access("/dev/ets_fp", F_OK) == 0 ||
                     access("/dev/egis", F_OK) == 0 || access("/dev/esfp0", F_OK) == 0);
    if (!has_egis) return nullptr;

    void* rbs_handle = dlopen("libRbsFlow.so", RTLD_NOW);
    if (rbs_handle == nullptr) {
        ALOGE("Failed to dlopen libRbsFlow.so: %s", dlerror());
        return nullptr;
    }

    ALOGI("Detected Egistec RBS library");
    auto rbsDevice = static_cast<rbs_fingerprint_device_t*>(malloc(sizeof(rbs_fingerprint_device_t)));
    if (!rbsDevice) return nullptr;

    rbsDevice->rbs_initialize = reinterpret_cast<typeof(rbsDevice->rbs_initialize)>(dlsym(rbs_handle, "rbs_initialize"));
    rbsDevice->rbs_uninitialize = reinterpret_cast<typeof(rbsDevice->rbs_uninitialize)>(dlsym(rbs_handle, "rbs_uninitialize"));
    rbsDevice->rbs_cancel = reinterpret_cast<typeof(rbsDevice->rbs_cancel)>(dlsym(rbs_handle, "rbs_cancel"));
    rbsDevice->rbs_active_user_group = reinterpret_cast<typeof(rbsDevice->rbs_active_user_group)>(dlsym(rbs_handle, "rbs_active_user_group"));
    rbsDevice->rbs_set_data_path = reinterpret_cast<typeof(rbsDevice->rbs_set_data_path)>(dlsym(rbs_handle, "rbs_set_data_path"));
    rbsDevice->rbs_chk_secure_id = reinterpret_cast<typeof(rbsDevice->rbs_chk_secure_id)>(dlsym(rbs_handle, "rbs_chk_secure_id"));
    rbsDevice->rbs_pre_enroll = reinterpret_cast<typeof(rbsDevice->rbs_pre_enroll)>(dlsym(rbs_handle, "rbs_pre_enroll"));
    rbsDevice->rbs_enroll = reinterpret_cast<typeof(rbsDevice->rbs_enroll)>(dlsym(rbs_handle, "rbs_enroll"));
    rbsDevice->rbs_post_enroll = reinterpret_cast<typeof(rbsDevice->rbs_post_enroll)>(dlsym(rbs_handle, "rbs_post_enroll"));
    rbsDevice->rbs_chk_auth_token = reinterpret_cast<typeof(rbsDevice->rbs_chk_auth_token)>(dlsym(rbs_handle, "rbs_chk_auth_token"));
    rbsDevice->rbs_authenticator = reinterpret_cast<typeof(rbsDevice->rbs_authenticator)>(dlsym(rbs_handle, "rbs_authenticator"));
    rbsDevice->rbs_remove_fingerprint = reinterpret_cast<typeof(rbsDevice->rbs_remove_fingerprint)>(dlsym(rbs_handle, "rbs_remove_fingerprint"));
    rbsDevice->rbs_get_fingerprint_ids = reinterpret_cast<typeof(rbsDevice->rbs_get_fingerprint_ids)>(dlsym(rbs_handle, "rbs_get_fingerprint_ids"));
    rbsDevice->rbs_get_authenticator_id = reinterpret_cast<typeof(rbsDevice->rbs_get_authenticator_id)>(dlsym(rbs_handle, "rbs_get_authenticator_id"));
    rbsDevice->rbs_set_on_callback_proc = reinterpret_cast<typeof(rbsDevice->rbs_set_on_callback_proc)>(dlsym(rbs_handle, "rbs_set_on_callback_proc"));
    rbsDevice->rbs_extra_api = reinterpret_cast<typeof(rbsDevice->rbs_extra_api)>(dlsym(rbs_handle, "rbs_extra_api"));

    if (rbsDevice->rbs_initialize && rbsDevice->rbs_uninitialize && rbsDevice->rbs_cancel &&
        rbsDevice->rbs_active_user_group && rbsDevice->rbs_chk_secure_id && rbsDevice->rbs_pre_enroll &&
        rbsDevice->rbs_enroll && rbsDevice->rbs_post_enroll && rbsDevice->rbs_chk_auth_token &&
        rbsDevice->rbs_authenticator && rbsDevice->rbs_remove_fingerprint && rbsDevice->rbs_get_fingerprint_ids &&
        rbsDevice->rbs_get_authenticator_id && rbsDevice->rbs_set_on_callback_proc) {

        rbsDevice->rbs_set_on_callback_proc(reinterpret_cast<void*>(Fingerprint::rbsNotify));
        int err = rbsDevice->rbs_initialize(0, 0);
        if (err == 0) {
            ALOGI("Initialized Egistec RBS fingerprint sensor successfully");
            return rbsDevice;
        } else {
            ALOGE("Can't initialize RBS fingerprint, error: %d", err);
        }
    } else {
        ALOGE("Failed to load all RBS symbols from libRbsFlow.so");
    }

    free(rbsDevice);
    return nullptr;
}

void Fingerprint::rbsNotify(uint32_t eventId, uint32_t value1, uint32_t value2, void* buffer, uint32_t buffer_size) {
    if (sInstance) {
        sInstance->handleRbsNotify(eventId, value1, value2, buffer, buffer_size);
    }
}

void Fingerprint::handleRbsNotify(uint32_t eventId, uint32_t value1, uint32_t value2, void* buffer, uint32_t /* buffer_size */) {
    ALOGI("handleRbsNotify: eventId = %u, value1 = %u, value2 = %u", eventId, value1, value2);
    fingerprint_msg_t msg;
    memset(&msg, 0, sizeof(msg));

    switch (eventId) {
        case 0x3eb:
        case 0x401:
            msg.type = FINGERPRINT_ERROR;
            msg.data.error = FINGERPRINT_ERROR_CANCELED;
            break;
        case 0x40e:
            msg.type = FINGERPRINT_ERROR;
            msg.data.error = FINGERPRINT_ERROR_TIMEOUT;
            break;
        case 0x3ec:
        case 0x3ed:
            msg.type = FINGERPRINT_ACQUIRED;
            msg.data.acquired.acquired_info = FINGERPRINT_ACQUIRED_TOO_SLOW;
            break;
        case 0x3ee:
        case 0x3ef:
            msg.type = FINGERPRINT_ACQUIRED;
            msg.data.acquired.acquired_info = FINGERPRINT_ACQUIRED_VENDOR_BASE;
            break;
        case 0x3f5:
            msg.type = FINGERPRINT_ACQUIRED;
            msg.data.acquired.acquired_info = FINGERPRINT_ACQUIRED_INSUFFICIENT;
            break;
        case 0x3f7:
        case 0x3f8:
            msg.type = FINGERPRINT_ACQUIRED;
            msg.data.acquired.acquired_info = FINGERPRINT_ACQUIRED_PARTIAL;
            break;
        case 0x3f9:
        case 0x3fa:
        case 0x3fb:
            msg.type = FINGERPRINT_ACQUIRED;
            msg.data.acquired.acquired_info = FINGERPRINT_ACQUIRED_TOO_FAST;
            break;
        case 0x3fe:
            msg.type = FINGERPRINT_ACQUIRED;
            msg.data.acquired.acquired_info = FINGERPRINT_ACQUIRED_GOOD;
            break;
        case 0x40d:
            msg.type = FINGERPRINT_TEMPLATE_ENROLLING;
            msg.data.enroll.finger.fid = value1;
            msg.data.enroll.finger.gid = mSession ? mSession->getUserId() : 0;
            msg.data.enroll.samples_remaining = value2;
            break;
        case 0x3f2:
        case 0x3f3:
            msg.type = FINGERPRINT_AUTHENTICATED;
            msg.data.authenticated.finger.gid = value1;
            msg.data.authenticated.finger.fid = value2;
            if (value2 != 0 && buffer != nullptr) {
                memcpy(&msg.data.authenticated.hat, buffer, sizeof(hw_auth_token_t));
            }
            break;
        default:
            ALOGW("handleRbsNotify: unknown eventId %u", eventId);
            return;
    }

    notify(&msg);
}

anc_fingerprint_device_t* Fingerprint::openAncFingerprintHal(fingerprint_device_t** outDev) {
    void* anc_handle = dlopen("anc.hal.so", RTLD_NOW);
    if (anc_handle == nullptr) {
        return nullptr;
    }

    ALOGI("Detected Jiiov ANC library (anc.hal.so)");
    auto ancDevice = static_cast<anc_fingerprint_device_t*>(malloc(sizeof(anc_fingerprint_device_t)));
    if (ancDevice) {
        memset(ancDevice, 0, sizeof(anc_fingerprint_device_t));
        ancDevice->GetFingerprintDevice = reinterpret_cast<typeof(ancDevice->GetFingerprintDevice)>(dlsym(anc_handle, "GetFingerprintDevice"));
        ancDevice->InitFingerprintDevice = reinterpret_cast<typeof(ancDevice->InitFingerprintDevice)>(dlsym(anc_handle, "InitFingerprintDevice"));
        ancDevice->DeinitFingerprintDevice = reinterpret_cast<typeof(ancDevice->DeinitFingerprintDevice)>(dlsym(anc_handle, "DeinitFingerprintDevice"));
        ancDevice->AncSetNotifyCallback = reinterpret_cast<typeof(ancDevice->AncSetNotifyCallback)>(dlsym(anc_handle, "AncSetNotifyCallback"));
        ancDevice->AncSetActiveGroup = reinterpret_cast<typeof(ancDevice->AncSetActiveGroup)>(dlsym(anc_handle, "AncSetActiveGroup"));
        ancDevice->AncGenerateChallenge = reinterpret_cast<typeof(ancDevice->AncGenerateChallenge)>(dlsym(anc_handle, "AncGenerateChallenge"));
        ancDevice->AncRevokeChallenge = reinterpret_cast<typeof(ancDevice->AncRevokeChallenge)>(dlsym(anc_handle, "AncRevokeChallenge"));
        ancDevice->AncEnroll = reinterpret_cast<typeof(ancDevice->AncEnroll)>(dlsym(anc_handle, "AncEnroll"));
        ancDevice->AncAuthenticate = reinterpret_cast<typeof(ancDevice->AncAuthenticate)>(dlsym(anc_handle, "AncAuthenticate"));
        ancDevice->AncEnumerate = reinterpret_cast<typeof(ancDevice->AncEnumerate)>(dlsym(anc_handle, "AncEnumerate"));
        ancDevice->AncRemove = reinterpret_cast<typeof(ancDevice->AncRemove)>(dlsym(anc_handle, "AncRemove"));
        ancDevice->AncGetAuthenticatorId = reinterpret_cast<typeof(ancDevice->AncGetAuthenticatorId)>(dlsym(anc_handle, "AncGetAuthenticatorId"));
        ancDevice->AncInvalidateAuthenticatorId = reinterpret_cast<typeof(ancDevice->AncInvalidateAuthenticatorId)>(dlsym(anc_handle, "AncInvalidateAuthenticatorId"));
        ancDevice->AncResetLockout = reinterpret_cast<typeof(ancDevice->AncResetLockout)>(dlsym(anc_handle, "AncResetLockout"));
        ancDevice->AncCancel = reinterpret_cast<typeof(ancDevice->AncCancel)>(dlsym(anc_handle, "AncCancel"));

        if (ancDevice->GetFingerprintDevice && ancDevice->InitFingerprintDevice && ancDevice->AncSetNotifyCallback) {
            fingerprint_device_t* dev = ancDevice->GetFingerprintDevice();
            if (!dev) {
                dev = static_cast<fingerprint_device_t*>(malloc(sizeof(fingerprint_device_t)));
                memset(dev, 0, sizeof(fingerprint_device_t));
            }
            ancDevice->AncSetNotifyCallback(dev, reinterpret_cast<void*>(Fingerprint::notify));
            int err = ancDevice->InitFingerprintDevice(dev);
            if (err == 0) {
                ALOGI("Initialized Jiiov ANC fingerprint sensor successfully");
                if (outDev) *outDev = dev;
                return ancDevice;
            } else {
                ALOGE("Failed to initialize ANC fingerprint device: %d", err);
            }
        } else {
            ALOGE("Failed to load ANC symbols from anc.hal.so");
        }
        free(ancDevice);
    }
    return nullptr;
}

std::vector<SensorLocation> Fingerprint::getSensorLocations() {
    std::vector<SensorLocation> locations;

    auto loc = mConfig->get<std::string>("sensor_location");
    auto entries = ::android::base::Split(loc, ",");

    for (const auto& entry : entries) {
        auto isValidStr = false;
        auto dim = ::android::base::Split(entry, "|");

        if (dim.size() != 3 and dim.size() != 4) {
            if (!loc.empty()) {
                ALOGE("Invalid sensor location input (x|y|radius) or (x|y|radius|display): %s",
                      loc.c_str());
            }
        } else {
            int32_t x, y, r;
            std::string d;
            isValidStr = ParseInt(dim[0], &x) && ParseInt(dim[1], &y) && ParseInt(dim[2], &r);
            if (dim.size() == 4) {
                d = dim[3];
                isValidStr = isValidStr && !d.empty();
            }
            if (isValidStr)
                locations.push_back({.sensorLocationX = x,
                                     .sensorLocationY = y,
                                     .sensorRadius = r,
                                     .display = d});
        }
    }

    return locations;
}

void Fingerprint::notify(const fingerprint_msg_t* msg) {
    Fingerprint* thisPtr = sInstance;
    if (thisPtr == nullptr || thisPtr->mSession == nullptr || thisPtr->mSession->isClosed()) {
        ALOGE("Receiving callbacks before a session is opened.");
        return;
    }
    thisPtr->mSession->notify(msg);
}

ndk::ScopedAStatus Fingerprint::getSensorProps(std::vector<SensorProps>* out) {
    std::vector<common::ComponentInfo> componentInfo = {
            {HW_COMPONENT_ID, HW_VERSION, FW_VERSION, SERIAL_NUMBER, "" /* softwareVersion */},
            {SW_COMPONENT_ID, "" /* hardwareVersion */, "" /* firmwareVersion */,
             "" /* serialNumber */, SW_VERSION}};
    auto sensorId = mConfig->get<std::int32_t>("sensor_id");
    auto sensorStrength = mConfig->get<std::int32_t>("sensor_strength");
    auto navigationGuesture = mConfig->get<bool>("navigation_gesture");
    auto detectInteraction = mConfig->get<bool>("detect_interaction");
    auto displayTouch = mConfig->get<bool>("display_touch");
    auto controlIllumination = mConfig->get<bool>("control_illumination");

    common::CommonProps commonProps = {sensorId, (common::SensorStrength)sensorStrength,
                                       MAX_ENROLLMENTS_PER_USER, componentInfo};

    std::vector<SensorLocation> sensorLocations = getSensorLocations();

    std::vector<std::string> sensorLocationStrings;
    std::transform(sensorLocations.begin(), sensorLocations.end(),
                   std::back_inserter(sensorLocationStrings),
                   [](const SensorLocation& obj) { return obj.toString(); });

    ALOGI("sensor type: %s, location: %s", ::android::internal::ToString(mSensorType).c_str(),
          ::android::base::Join(sensorLocationStrings, ", ").c_str());

    *out = {{commonProps, mSensorType, sensorLocations, navigationGuesture, detectInteraction,
             displayTouch, controlIllumination, std::nullopt}};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Fingerprint::createSession(int32_t /*sensorId*/, int32_t userId,
                                              const std::shared_ptr<ISessionCallback>& cb,
                                              std::shared_ptr<ISession>* out) {
    CHECK(mSession == nullptr || mSession->isClosed()) << "Open session already exists!";

    mSession = SharedRefBase::make<Session>(mDevice, mRbsDevice, mAncDevice, mUdfpsHandler, userId, cb, mLockoutTracker);
    *out = mSession;

    mSession->linkToDeath(cb->asBinder().get());

    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::fingerprint
