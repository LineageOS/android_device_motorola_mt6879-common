/*
 * Copyright (C) 2024-2025 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cinttypes>
#include <thread>

#include "Legacy2Aidl.h"
#include "Session.h"

#include "CancellationSignal.h"

namespace aidl::android::hardware::biometrics::fingerprint {

void onClientDeath(void* cookie) {
    ALOGI("FingerprintService has died");
    Session* session = static_cast<Session*>(cookie);
    if (session && !session->isClosed()) {
        session->close();
    }
}

Session::Session(fingerprint_device_t* device, rbs_fingerprint_device_t* rbsDevice,
                 anc_fingerprint_device_t* ancDevice, UdfpsHandler* udfpsHandler, int userId,
                 std::shared_ptr<ISessionCallback> cb, LockoutTracker lockoutTracker)
    : mDevice(device),
      mRbsDevice(rbsDevice),
      mAncDevice(ancDevice),
      mLockoutTracker(lockoutTracker),
      mUserId(userId),
      mCb(cb),
      mUdfpsHandler(udfpsHandler) {
    mDeathRecipient = AIBinder_DeathRecipient_new(onClientDeath);

    auto path = std::format("/data/vendor_de/{}/fpdata", userId);
    if (mAncDevice && mAncDevice->AncSetActiveGroup) {
        ALOGI("setActiveGroup (ANC)");
        int rc = mAncDevice->AncSetActiveGroup(mDevice, userId, path.c_str());
        if (rc != 0) {
            ALOGE("AncSetActiveGroup failed, error: %d", rc);
        }
    } else if (mRbsDevice) {
        ALOGI("setActiveGroup (RBS)");
        int rc = mRbsDevice->rbs_active_user_group(userId, path.c_str());
        if (rc != 0) {
            ALOGE("rbs_active_user_group failed, error: %d", rc);
        }
        rc = mRbsDevice->rbs_set_data_path(1, path.c_str());
        if (rc != 0) {
            ALOGE("rbs_set_data_path failed, error: %d", rc);
        }
    } else if (mDevice) {
        ALOGI("setActiveGroup: userId=%d, path=%s", mUserId, path.c_str());
        int rc = mDevice->set_active_group(mDevice, mUserId, path.c_str());
        if (rc != 0) {
            ALOGE("set_active_group failed, error: %d", rc);
        }
    }
}

ndk::ScopedAStatus Session::generateChallenge() {
    if (mAncDevice && mAncDevice->AncGenerateChallenge) {
        uint64_t challenge = mAncDevice->AncGenerateChallenge(mDevice);
        mCb->onChallengeGenerated(challenge);
        return ndk::ScopedAStatus::ok();
    }
    if (mRbsDevice) {
        mChallenge = static_cast<uint64_t>(rand()) | (static_cast<uint64_t>(rand()) << 32);
        int rc = mRbsDevice->rbs_pre_enroll(mUserId, 10);
        if (rc != 0) {
            ALOGE("rbs_pre_enroll failed in generateChallenge: %d", rc);
        }
        mCb->onChallengeGenerated(mChallenge);
        return ndk::ScopedAStatus::ok();
    }
    uint64_t challenge = mDevice->pre_enroll(mDevice);
    ALOGI("generateChallenge: %ld", challenge);
    mCb->onChallengeGenerated(challenge);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::revokeChallenge(int64_t challenge) {
    if (mAncDevice && mAncDevice->AncRevokeChallenge) {
        int error = mAncDevice->AncRevokeChallenge(mDevice, challenge);
        if (error) {
            ALOGE("Failed to revoke challenge (ANC)=%" PRId64 " error=%d", challenge, error);
        }
        mCb->onChallengeRevoked(challenge);
        return ndk::ScopedAStatus::ok();
    }
    if (mRbsDevice) {
        mChallenge = 0;
        int rc = mRbsDevice->rbs_post_enroll();
        if (rc != 0) {
            ALOGE("rbs_post_enroll failed in revokeChallenge: %d", rc);
        }
        mCb->onChallengeRevoked(challenge);
        return ndk::ScopedAStatus::ok();
    }
    ALOGI("revokeChallenge: %ld", challenge);
    mDevice->post_enroll(mDevice);
    mCb->onChallengeRevoked(challenge);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enroll(const HardwareAuthToken& hat,
                                   std::shared_ptr<ICancellationSignal>* out) {
    if (mAncDevice && mAncDevice->AncEnroll) {
        hw_auth_token_t authToken;
        translate(hat, authToken);
        int error = mAncDevice->AncEnroll(mDevice, &authToken, mUserId, 60);
        if (error) {
            ALOGE("AncEnroll failed: %d", error);
            mCb->onError(Error::UNABLE_TO_PROCESS, error);
        }
        *out = SharedRefBase::make<CancellationSignal>(this);
        return ndk::ScopedAStatus::ok();
    }

    if (mRbsDevice) {
        hw_auth_token_t authToken;
        translate(hat, authToken);

        if (authToken.timestamp == 0) {
            ALOGE("HAT timestamp is 0");
            mCb->onError(Error::UNABLE_TO_PROCESS, 0);
            *out = SharedRefBase::make<CancellationSignal>(this);
            return ndk::ScopedAStatus::ok();
        }

        if (authToken.challenge != mChallenge) {
            ALOGE("Challenge does not match: %ld vs %ld", authToken.challenge, mChallenge);
            mCb->onError(Error::UNABLE_TO_PROCESS, 0);
            *out = SharedRefBase::make<CancellationSignal>(this);
            return ndk::ScopedAStatus::ok();
        }

        if (authToken.version != 0) {
            ALOGE("Invalid HAT version = %d", (int)authToken.version);
            mCb->onError(Error::UNABLE_TO_PROCESS, 0);
            *out = SharedRefBase::make<CancellationSignal>(this);
            return ndk::ScopedAStatus::ok();
        }

        int rc = mRbsDevice->rbs_chk_auth_token(&authToken, sizeof(hw_auth_token_t));
        if (rc != 0) {
            ALOGE("Auth token check failed, error %d", rc);
            mCb->onError(Error::UNABLE_TO_PROCESS, rc);
            *out = SharedRefBase::make<CancellationSignal>(this);
            return ndk::ScopedAStatus::ok();
        }

        rc = mRbsDevice->rbs_chk_secure_id(mUserId, authToken.user_id);
        if (rc != 0) {
            ALOGD("Secure ID check failed, error %d", rc);
            if (rc == 0x21) {
                mRbsDevice->rbs_remove_fingerprint(mUserId, 0);
                rc = mRbsDevice->rbs_chk_secure_id(mUserId, authToken.user_id);
            }
            if (rc != 0) {
                ALOGE("Secure ID check failed after check/remove, error %d", rc);
                mCb->onError(Error::UNABLE_TO_PROCESS, rc);
                *out = SharedRefBase::make<CancellationSignal>(this);
                return ndk::ScopedAStatus::ok();
            }
        }

        rc = mRbsDevice->rbs_pre_enroll(mUserId, 10);
        if (rc != 0) {
            ALOGE("rbs_pre_enroll failed: %d", rc);
        }
        mRbsDevice->rbs_cancel(nullptr, 2);
        if (mRbsDevice->rbs_extra_api) {
            mRbsDevice->rbs_extra_api(3, nullptr, 0, nullptr, nullptr);
        }

        rc = mRbsDevice->rbs_enroll();
        if (rc != 0) {
            ALOGE("rbs_enroll failed: %d", rc);
            mCb->onError(Error::UNABLE_TO_PROCESS, rc);
        }

        *out = SharedRefBase::make<CancellationSignal>(this);
        return ndk::ScopedAStatus::ok();
    }

    hw_auth_token_t authToken;
    translate(hat, authToken);
    int error = mDevice->enroll(mDevice, &authToken, mUserId, 60);
    if (error) {
        ALOGE("enroll failed: %d", error);
        mCb->onError(Error::UNABLE_TO_PROCESS, error);
    }

    *out = SharedRefBase::make<CancellationSignal>(this);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticate(int64_t operationId,
                                         std::shared_ptr<ICancellationSignal>* out) {
    if (mAncDevice && mAncDevice->AncAuthenticate) {
        checkSensorLockout();
        int error = mAncDevice->AncAuthenticate(mDevice, operationId, mUserId);
        if (error) {
            ALOGE("AncAuthenticate failed: %d", error);
        }
        *out = SharedRefBase::make<CancellationSignal>(this);
        return ndk::ScopedAStatus::ok();
    }

    if (mRbsDevice) {
        checkSensorLockout();
        mRbsDevice->rbs_cancel(nullptr, 2);
        mRbsDevice->rbs_cancel(nullptr, 3);
        mRbsDevice->rbs_cancel(nullptr, 5);
        int rc = mRbsDevice->rbs_authenticator(mUserId, nullptr, 0, operationId);
        if (rc != 0) {
            ALOGE("rbs_authenticator failed, error %d", rc);
            mCb->onError(Error::CANCELED, 0);
            if (rc == 4) {
                mCb->onError(Error::HW_UNAVAILABLE, 0);
            }
        }
        *out = SharedRefBase::make<CancellationSignal>(this);
        return ndk::ScopedAStatus::ok();
    }

    checkSensorLockout();
    int error = mDevice->authenticate(mDevice, operationId, mUserId);
    if (error) {
        ALOGE("authenticate failed: %d", error);
        mCb->onError(Error::UNABLE_TO_PROCESS, error);
    }

    *out = SharedRefBase::make<CancellationSignal>(this);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::detectInteraction(std::shared_ptr<ICancellationSignal>* out) {
    ALOGD("Detect interaction is not supported");
    mCb->onError(Error::UNABLE_TO_PROCESS, 0 /* vendorCode */);

    *out = SharedRefBase::make<CancellationSignal>(this);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enumerateEnrollments() {
    if (mAncDevice && mAncDevice->AncEnumerate) {
        int error = mAncDevice->AncEnumerate(mDevice);
        if (error) {
            ALOGE("AncEnumerate failed: %d", error);
        }
        return ndk::ScopedAStatus::ok();
    }

    if (mRbsDevice) {
        uint32_t num_fids = 0;
        uint32_t fids[5] = {};
        int rc = mRbsDevice->rbs_get_fingerprint_ids(mUserId, fids, &num_fids);
        if (rc != 0) {
            ALOGE("RBS get_fingerprint_ids failed, error: %d", rc);
            mCb->onError(Error::UNABLE_TO_PROCESS, rc);
            return ndk::ScopedAStatus::ok();
        }
        std::vector<int32_t> enrollments;
        for (uint32_t i = 0; i < num_fids; i++) {
            enrollments.push_back(fids[i]);
        }
        mCb->onEnrollmentsEnumerated(enrollments);
        return ndk::ScopedAStatus::ok();
    }

    mEnumeratedEnrollments.clear();
    int error = mDevice->enumerate(mDevice);
    if (error) {
        ALOGE("enumerate failed: %d", error);
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::removeEnrollments(const std::vector<int32_t>& enrollmentIds) {
    ALOGI("removeEnrollments, size: %zu", enrollmentIds.size());

    if (mAncDevice && mAncDevice->AncRemove) {
        if (enrollmentIds.empty()) {
            mAncDevice->AncRemove(mDevice, mUserId, 0);
        } else {
            for (int32_t fid : enrollmentIds) {
                mAncDevice->AncRemove(mDevice, mUserId, fid);
            }
        }
        return ndk::ScopedAStatus::ok();
    }

    if (mRbsDevice) {
        if (enrollmentIds.empty()) {
            uint32_t num_fids = 0;
            uint32_t fids[5] = {};
            int rc = mRbsDevice->rbs_get_fingerprint_ids(mUserId, fids, &num_fids);
            if (rc == 0 && num_fids > 0) {
                rc = mRbsDevice->rbs_remove_fingerprint(mUserId, 0);
                if (rc == 0) {
                    std::vector<int32_t> removedIds;
                    for (uint32_t i = 0; i < num_fids; i++) {
                        removedIds.push_back(fids[i]);
                    }
                    mCb->onEnrollmentsRemoved(removedIds);
                } else {
                    ALOGE("RBS remove failed, error: %d", rc);
                    mCb->onError(Error::UNABLE_TO_REMOVE, rc);
                }
            } else {
                mCb->onEnrollmentsRemoved({});
            }
        } else {
            std::vector<int32_t> removedIds;
            for (int32_t fid : enrollmentIds) {
                int rc = mRbsDevice->rbs_remove_fingerprint(mUserId, fid);
                if (rc == 0) {
                    removedIds.push_back(fid);
                } else {
                    ALOGE("RBS remove failed for fid %d, error: %d", fid, rc);
                }
            }
            mCb->onEnrollmentsRemoved(removedIds);
        }
        return ndk::ScopedAStatus::ok();
    }

#ifdef IMPL_V2
    if (enrollmentIds.empty()) {
        mCb->onEnrollmentsRemoved({});
        return ndk::ScopedAStatus::ok();
    }
    int error = mDevice->aidl_remove(mDevice, mUserId, enrollmentIds.data(),
                                     enrollmentIds.size());
    if (error) {
        ALOGE("aidl_remove failed: %d", error);
    }
    return ndk::ScopedAStatus::ok();
#else

    for (int32_t fid : enrollmentIds) {
        int error = mDevice->remove(mDevice, mUserId, fid);
        if (error) {
            ALOGE("remove failed: %d", error);
        }
    }
#endif
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getAuthenticatorId() {
    if (mAncDevice && mAncDevice->AncGetAuthenticatorId) {
        uint64_t auth_id = mAncDevice->AncGetAuthenticatorId(mDevice);
        mCb->onAuthenticatorIdRetrieved(auth_id);
        return ndk::ScopedAStatus::ok();
    }

    if (mRbsDevice) {
        uint64_t auth_id = 0;
        int rc = mRbsDevice->rbs_get_authenticator_id(&auth_id);
        if (rc != 0) {
            ALOGE("RBS get_authenticator_id failed, error: %d", rc);
        }
        mCb->onAuthenticatorIdRetrieved(auth_id);
        return ndk::ScopedAStatus::ok();
    }

    uint64_t auth_id = mDevice->get_authenticator_id(mDevice);
    ALOGI("getAuthenticatorId: %ld", auth_id);
    mCb->onAuthenticatorIdRetrieved(auth_id);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::invalidateAuthenticatorId() {
    if (mAncDevice && mAncDevice->AncInvalidateAuthenticatorId) {
        uint64_t new_auth_id = mAncDevice->AncInvalidateAuthenticatorId(mDevice);
        mCb->onAuthenticatorIdInvalidated(new_auth_id);
        return ndk::ScopedAStatus::ok();
    }

    if (mRbsDevice) {
        uint64_t new_auth_id = 0;
        int rc = mRbsDevice->rbs_get_authenticator_id(&new_auth_id);
        if (rc != 0) {
            ALOGE("RBS get_authenticator_id failed, error: %d", rc);
        }
        mCb->onAuthenticatorIdInvalidated(new_auth_id);
        return ndk::ScopedAStatus::ok();
    }

    uint64_t auth_id = mDevice->get_authenticator_id(mDevice);
    ALOGI("invalidateAuthenticatorId: %ld", auth_id);
    mCb->onAuthenticatorIdInvalidated(auth_id);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::resetLockout(const HardwareAuthToken& hat) {
    if (mAncDevice && mAncDevice->AncResetLockout) {
        hw_auth_token_t authToken;
        translate(hat, authToken);
        int error = mAncDevice->AncResetLockout(mDevice, &authToken);
        if (error) {
            ALOGE("AncResetLockout failed: %d", error);
        }
    }
#ifdef IMPL_V2
    else if (mDevice) {
        hw_auth_token_t authToken;
        translate(hat, authToken);
        int error = mDevice->reset_lockout(mDevice, &authToken);
        if (error) {
            ALOGE("reset_lockout failed: %d", error);
        }
    }
#endif
    clearLockout(true);
    if (mIsLockoutTimerStarted) mIsLockoutTimerAborted = true;

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerDown(int32_t /*pointerId*/, int32_t x, int32_t y, float minor,
                                          float major) {
    if (mUdfpsHandler) {
        mUdfpsHandler->onFingerDown(x, y, minor, major);
    }
    if (mRbsDevice && mRbsDevice->rbs_extra_api) {
        mRbsDevice->rbs_extra_api(1, nullptr, 0, nullptr, nullptr);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerUp(int32_t /*pointerId*/) {
    if (mUdfpsHandler) {
        mUdfpsHandler->onFingerUp();
    }
    if (mRbsDevice && mRbsDevice->rbs_extra_api) {
        mRbsDevice->rbs_extra_api(2, nullptr, 0, nullptr, nullptr);
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onUiReady() {
    // TODO: stub

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticateWithContext(
        int64_t operationId, const common::OperationContext& /*context*/,
        std::shared_ptr<common::ICancellationSignal>* out) {
    return authenticate(operationId, out);
}

ndk::ScopedAStatus Session::enrollWithContext(const keymaster::HardwareAuthToken& hat,
                                              const common::OperationContext& /*context*/,
                                              std::shared_ptr<common::ICancellationSignal>* out) {
    return enroll(hat, out);
}

ndk::ScopedAStatus Session::detectInteractionWithContext(
        const common::OperationContext& /*context*/,
        std::shared_ptr<common::ICancellationSignal>* out) {
    return detectInteraction(out);
}

ndk::ScopedAStatus Session::onPointerDownWithContext(const PointerContext& context) {
    return onPointerDown(context.pointerId, context.x, context.y, context.minor, context.major);
}

ndk::ScopedAStatus Session::onPointerUpWithContext(const PointerContext& context) {
    return onPointerUp(context.pointerId);
}

ndk::ScopedAStatus Session::onContextChanged(const common::OperationContext& /*context*/) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerCancelWithContext(const PointerContext& /*context*/) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::setIgnoreDisplayTouches(bool /*shouldIgnore*/) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::cancel() {
    if (mUdfpsHandler) {
        mUdfpsHandler->cancel();
    }

    if (mAncDevice && mAncDevice->AncCancel) {
        int ret = mAncDevice->AncCancel(mDevice);
        if (ret == 0) {
            mCb->onError(Error::CANCELED, 0);
            return ndk::ScopedAStatus::ok();
        }
        return ndk::ScopedAStatus::fromServiceSpecificError(ret);
    }

    if (mRbsDevice) {
        mRbsDevice->rbs_cancel(nullptr, 2);
        mRbsDevice->rbs_cancel(nullptr, 3);
        mRbsDevice->rbs_cancel(nullptr, 5);
        mCb->onError(Error::CANCELED, 0);
        return ndk::ScopedAStatus::ok();
    }

    if (mDevice) {
        int ret = mDevice->cancel(mDevice);
        if (ret == 0) {
            mCb->onError(Error::CANCELED, 0 /* vendorCode */);
            return ndk::ScopedAStatus::ok();
        }
        return ndk::ScopedAStatus::fromServiceSpecificError(ret);
    }

    mCb->onError(Error::CANCELED, 0);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::close() {
    mClosed = true;
    mCb->onSessionClosed();
    AIBinder_DeathRecipient_delete(mDeathRecipient);
    return ndk::ScopedAStatus::ok();
}

binder_status_t Session::linkToDeath(AIBinder* binder) {
    return AIBinder_linkToDeath(binder, mDeathRecipient, this);
}

bool Session::isClosed() {
    return mClosed;
}

// Translate from errors returned by traditional HAL (see fingerprint.h) to
// AIDL-compliant Error
Error Session::VendorErrorFilter(int32_t error, int32_t* vendorCode) {
    *vendorCode = 0;

    switch (error) {
        case FINGERPRINT_ERROR_HW_UNAVAILABLE:
        case FINGERPRINT_ERROR_VENDOR_BASE + 1:
        case FINGERPRINT_ERROR_VENDOR_BASE + 2:
        case FINGERPRINT_ERROR_VENDOR_BASE + 3:
            return Error::HW_UNAVAILABLE;
        case FINGERPRINT_ERROR_UNABLE_TO_PROCESS:
        case FINGERPRINT_ERROR_VENDOR_BASE + 4:
        case FINGERPRINT_ERROR_VENDOR_BASE + 5:
            return Error::UNABLE_TO_PROCESS;
        case FINGERPRINT_ERROR_TIMEOUT:
            return Error::TIMEOUT;
        case FINGERPRINT_ERROR_NO_SPACE:
            return Error::NO_SPACE;
        case FINGERPRINT_ERROR_CANCELED:
            return Error::CANCELED;
        case FINGERPRINT_ERROR_UNABLE_TO_REMOVE:
            return Error::UNABLE_TO_REMOVE;
        case FINGERPRINT_ERROR_LOCKOUT:
        case 0x3f0: {
            *vendorCode = FINGERPRINT_ERROR_LOCKOUT;
            return Error::VENDOR;
        }
        default:
            if (error > 999) {
                *vendorCode = error - 1000;
                return Error::VENDOR;
            }
            if (error >= FINGERPRINT_ERROR_VENDOR_BASE) {
                // vendor specific code.
                *vendorCode = error - FINGERPRINT_ERROR_VENDOR_BASE;
                return Error::VENDOR;
            }
    }
    ALOGE("Unknown error from fingerprint vendor library: %d", error);
    return Error::UNABLE_TO_PROCESS;
}

// Translate acquired messages returned by traditional HAL (see fingerprint.h)
// to AIDL-compliant AcquiredInfo
AcquiredInfo Session::VendorAcquiredFilter(int32_t info, int32_t* vendorCode) {
    *vendorCode = 0;

    switch (info) {
        case FINGERPRINT_ACQUIRED_GOOD:
            return AcquiredInfo::GOOD;
        case FINGERPRINT_ACQUIRED_PARTIAL:
            return AcquiredInfo::PARTIAL;
        case FINGERPRINT_ACQUIRED_INSUFFICIENT:
            return AcquiredInfo::INSUFFICIENT;
        case FINGERPRINT_ACQUIRED_IMAGER_DIRTY:
            return AcquiredInfo::SENSOR_DIRTY;
        case FINGERPRINT_ACQUIRED_TOO_SLOW:
            return AcquiredInfo::TOO_SLOW;
        case FINGERPRINT_ACQUIRED_TOO_FAST:
        case FINGERPRINT_ACQUIRED_VENDOR_BASE + 8:
            return AcquiredInfo::TOO_FAST;
        case FINGERPRINT_ACQUIRED_VENDOR_BASE + 5:
        case FINGERPRINT_ACQUIRED_VENDOR_BASE + 6:
        case FINGERPRINT_ACQUIRED_VENDOR_BASE + 7:
            *vendorCode = info - FINGERPRINT_ACQUIRED_VENDOR_BASE;
            return AcquiredInfo::VENDOR;
        default:
            if (info >= FINGERPRINT_ACQUIRED_VENDOR_BASE) {
                // vendor specific code.
                *vendorCode = info - FINGERPRINT_ACQUIRED_VENDOR_BASE;
                return AcquiredInfo::VENDOR;
            }
    }
    ALOGE("Unknown acquired message from fingerprint vendor library: %d", info);
    return AcquiredInfo::UNKNOWN;
}

bool Session::checkSensorLockout() {
    LockoutTracker::LockoutMode lockoutMode = mLockoutTracker.getMode();
    if (lockoutMode == LockoutTracker::LockoutMode::kPermanent) {
        ALOGE("Fail: lockout permanent");
        mCb->onLockoutPermanent();
        mIsLockoutTimerAborted = true;
        return true;
    }
    if (lockoutMode == LockoutTracker::LockoutMode::kTimed) {
        int64_t timeLeft = mLockoutTracker.getLockoutTimeLeft();
        ALOGE("Fail: lockout timed: %ld", timeLeft);
        mCb->onLockoutTimed(timeLeft);
        if (!mIsLockoutTimerStarted) startLockoutTimer(timeLeft);
        return true;
    }
    return false;
}

void Session::clearLockout(bool clearAttemptCounter) {
    mLockoutTracker.reset(clearAttemptCounter);
    mCb->onLockoutCleared();
}

void Session::startLockoutTimer(int64_t timeout) {
    std::function<void()> action = std::bind(&Session::lockoutTimerExpired, this);
    std::thread([timeout, action]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
        action();
    }).detach();

    mIsLockoutTimerStarted = true;
}

void Session::lockoutTimerExpired() {
    if (!mIsLockoutTimerAborted) clearLockout(false);

    mIsLockoutTimerStarted = false;
    mIsLockoutTimerAborted = false;
}

void Session::notify(const fingerprint_msg_t* msg) {
    // const uint64_t devId = reinterpret_cast<uint64_t>(mDevice);
    switch (msg->type) {
        case FINGERPRINT_ERROR: {
            int32_t vendorCode = 0;
            Error result = VendorErrorFilter(msg->data.error, &vendorCode);
            ALOGD("onError(%hhd, %d)", result, vendorCode);
            mCb->onError(result, vendorCode);
        } break;
        case FINGERPRINT_ACQUIRED: {
            int32_t vendorCode = 0;
            AcquiredInfo result =
                    VendorAcquiredFilter(msg->data.acquired.acquired_info, &vendorCode);
            ALOGD("onAcquired(%hhd, %d)", result, vendorCode);
            if (mUdfpsHandler) {
                mUdfpsHandler->onAcquired(static_cast<int32_t>(result), vendorCode);
            }
            // Filter specific vendor codes that cause framework to incorrectly disable
            // UDFPS display mode during processing. Pass through other vendor messages
            // that may contain useful user feedback.
            if (result == AcquiredInfo::VENDOR && (vendorCode >= 5 && vendorCode <= 7)) {
                ALOGD("Filtering vendor acquired code %d", vendorCode);
            } else {
                mCb->onAcquired(result, vendorCode);
            }
        } break;
        case FINGERPRINT_TEMPLATE_ENROLLING: {
            ALOGD("onEnrollResult(fid=%d, gid=%d, rem=%d)", msg->data.enroll.finger.fid,
                  msg->data.enroll.finger.gid, msg->data.enroll.samples_remaining);
            mCb->onEnrollmentProgress(msg->data.enroll.finger.fid,
                                      msg->data.enroll.samples_remaining);
            if (msg->data.enroll.samples_remaining == 0 && mUdfpsHandler) {
                mUdfpsHandler->onFingerUp();
            }
        } break;
        case FINGERPRINT_TEMPLATE_REMOVED: {
            ALOGD("onRemove(fid=%d, gid=%d, rem=%d)", msg->data.removed.finger.fid,
                  msg->data.removed.finger.gid, msg->data.removed.remaining_templates);
            std::vector<int> enrollments;
            enrollments.push_back(msg->data.removed.finger.fid);
            mCb->onEnrollmentsRemoved(enrollments);
        } break;
        case FINGERPRINT_AUTHENTICATED: {
            ALOGD("onAuthenticated(fid=%d, gid=%d)", msg->data.authenticated.finger.fid,
                  msg->data.authenticated.finger.gid);
            if (msg->data.authenticated.finger.fid != 0) {
                const hw_auth_token_t hat = msg->data.authenticated.hat;
                HardwareAuthToken authToken;
                translate(hat, authToken);

                if (mUdfpsHandler) {
                    mUdfpsHandler->onAuthenticationSucceeded();
                }
                mCb->onAuthenticationSucceeded(msg->data.authenticated.finger.fid, authToken);
                mLockoutTracker.reset(true);
            } else {
                if (mUdfpsHandler) {
                    mUdfpsHandler->onAuthenticationFailed();
                }
                mCb->onAuthenticationFailed();
                mLockoutTracker.addFailedAttempt();
                checkSensorLockout();
            }
        } break;
        case FINGERPRINT_TEMPLATE_ENUMERATING: {
            ALOGD("onEnumerate(fid=%d, gid=%d, rem=%d)", msg->data.enumerated.finger.fid,
                  msg->data.enumerated.finger.gid, msg->data.enumerated.remaining_templates);
            if (msg->data.enumerated.finger.fid != 0) {
                mEnumeratedEnrollments.push_back(msg->data.enumerated.finger.fid);
            }
            if (msg->data.enumerated.remaining_templates == 0) {
                mCb->onEnrollmentsEnumerated(mEnumeratedEnrollments);
                mEnumeratedEnrollments.clear();
            }
        } break;
        case FINGERPRINT_GENERATE_CHALLENGE: {
            int64_t challenge = msg->data.data;
            ALOGI("onChallengeGenerated: %ld", challenge);
            mCb->onChallengeGenerated(challenge);
        } break;
        case FINGERPRINT_REVOKE_CHALLENGE: {
            int64_t challenge = msg->data.data;
            ALOGI("onChallengeRevoked: %ld", challenge);
            mCb->onChallengeRevoked(challenge);
        } break;
        case FINGERPRINT_GET_AUTHENTICATOR_ID: {
            int auth_id = msg->data.data;
            ALOGI("onAuthenticatorIDRetrieved: %d", auth_id);
            if (mUdfpsHandler) {
                mUdfpsHandler->onFingerUp();
            }
            mCb->onAuthenticatorIdRetrieved(auth_id);
        } break;
        case FINGERPRINT_INVALIDATE_AUTHENTICATOR_ID: {
            int64_t new_auth_id = msg->data.data;
            ALOGI("onAuthenticatorIDInvalidated, new auth id: %ld", new_auth_id);
            mCb->onAuthenticatorIdInvalidated(new_auth_id);
        } break;
    }
}

}  // namespace aidl::android::hardware::biometrics::fingerprint
