/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "AntiFlickerService"

#include "AntiFlicker.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>

using android::base::ParseInt;
using android::base::ReadFileToString;
using android::base::Trim;
using android::base::WriteStringToFile;

namespace aidl {
namespace vendor {
namespace lineage {
namespace livedisplay {

static constexpr const char* kDCPath = "/sys/devices/platform/14017000.dsi/dc";

ndk::ScopedAStatus AntiFlicker::getEnabled(bool* _aidl_return) {
    std::string tmp;

    if (!ReadFileToString(kDCPath, &tmp)) {
        LOG(ERROR) << "Failed to read from " << kDCPath;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    int32_t val = 0;
    if (!ParseInt(Trim(tmp), &val)) {
        LOG(ERROR) << "Failed to parse dc value: " << tmp;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    *_aidl_return = val > 0;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus AntiFlicker::setEnabled(bool enabled) {
    if (!WriteStringToFile(enabled ? "1" : "0", kDCPath, true)) {
        LOG(ERROR) << "Failed to write to " << kDCPath;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    return ndk::ScopedAStatus::ok();
}

}  // namespace livedisplay
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
