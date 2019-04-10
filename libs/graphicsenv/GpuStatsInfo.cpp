/*
 * Copyright 2019 The Android Open Source Project
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

#include <inttypes.h>

#include <android-base/stringprintf.h>
#include <binder/Parcel.h>
#include <graphicsenv/GpuStatsInfo.h>

namespace android {

using base::StringAppendF;

status_t GpuStatsGlobalInfo::writeToParcel(Parcel* parcel) const {
    status_t status;
    if ((status = parcel->writeUtf8AsUtf16(driverPackageName)) != OK) return status;
    if ((status = parcel->writeUtf8AsUtf16(driverVersionName)) != OK) return status;
    if ((status = parcel->writeUint64(driverVersionCode)) != OK) return status;
    if ((status = parcel->writeInt64(driverBuildTime)) != OK) return status;
    if ((status = parcel->writeInt32(glLoadingCount)) != OK) return status;
    if ((status = parcel->writeInt32(glLoadingFailureCount)) != OK) return status;
    if ((status = parcel->writeInt32(vkLoadingCount)) != OK) return status;
    if ((status = parcel->writeInt32(vkLoadingFailureCount)) != OK) return status;
    return OK;
}

status_t GpuStatsGlobalInfo::readFromParcel(const Parcel* parcel) {
    status_t status;
    if ((status = parcel->readUtf8FromUtf16(&driverPackageName)) != OK) return status;
    if ((status = parcel->readUtf8FromUtf16(&driverVersionName)) != OK) return status;
    if ((status = parcel->readUint64(&driverVersionCode)) != OK) return status;
    if ((status = parcel->readInt64(&driverBuildTime)) != OK) return status;
    if ((status = parcel->readInt32(&glLoadingCount)) != OK) return status;
    if ((status = parcel->readInt32(&glLoadingFailureCount)) != OK) return status;
    if ((status = parcel->readInt32(&vkLoadingCount)) != OK) return status;
    if ((status = parcel->readInt32(&vkLoadingFailureCount)) != OK) return status;
    return OK;
}

std::string GpuStatsGlobalInfo::toString() const {
    std::string result;
    StringAppendF(&result, "driverPackageName = %s\n", driverPackageName.c_str());
    StringAppendF(&result, "driverVersionName = %s\n", driverVersionName.c_str());
    StringAppendF(&result, "driverVersionCode = %" PRIu64 "\n", driverVersionCode);
    StringAppendF(&result, "driverBuildTime = %" PRId64 "\n", driverBuildTime);
    StringAppendF(&result, "glLoadingCount = %d\n", glLoadingCount);
    StringAppendF(&result, "glLoadingFailureCount = %d\n", glLoadingFailureCount);
    StringAppendF(&result, "vkLoadingCount = %d\n", vkLoadingCount);
    StringAppendF(&result, "vkLoadingFailureCount = %d\n", vkLoadingFailureCount);
    return result;
}

status_t GpuStatsAppInfo::writeToParcel(Parcel* parcel) const {
    status_t status;
    if ((status = parcel->writeUtf8AsUtf16(appPackageName)) != OK) return status;
    if ((status = parcel->writeUint64(driverVersionCode)) != OK) return status;
    if ((status = parcel->writeInt64Vector(glDriverLoadingTime)) != OK) return status;
    if ((status = parcel->writeInt64Vector(vkDriverLoadingTime)) != OK) return status;
    return OK;
}

status_t GpuStatsAppInfo::readFromParcel(const Parcel* parcel) {
    status_t status;
    if ((status = parcel->readUtf8FromUtf16(&appPackageName)) != OK) return status;
    if ((status = parcel->readUint64(&driverVersionCode)) != OK) return status;
    if ((status = parcel->readInt64Vector(&glDriverLoadingTime)) != OK) return status;
    if ((status = parcel->readInt64Vector(&vkDriverLoadingTime)) != OK) return status;
    return OK;
}

std::string GpuStatsAppInfo::toString() const {
    std::string result;
    StringAppendF(&result, "appPackageName = %s\n", appPackageName.c_str());
    StringAppendF(&result, "driverVersionCode = %" PRIu64 "\n", driverVersionCode);
    result.append("glDriverLoadingTime:");
    for (int32_t loadingTime : glDriverLoadingTime) {
        StringAppendF(&result, " %d", loadingTime);
    }
    result.append("\n");
    result.append("vkDriverLoadingTime:");
    for (int32_t loadingTime : vkDriverLoadingTime) {
        StringAppendF(&result, " %d", loadingTime);
    }
    result.append("\n");
    return result;
}

} // namespace android
