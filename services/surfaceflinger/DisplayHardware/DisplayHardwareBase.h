/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ANDROID_DISPLAY_HARDWARE_BASE_H
#define ANDROID_DISPLAY_HARDWARE_BASE_H

#include <stdint.h>
#include <utils/RefBase.h>
#include <utils/StrongPointer.h>
#include <utils/threads.h>

namespace android {

class SurfaceFlinger; 

class DisplayHardwareBase
{
public:
    DisplayHardwareBase(
            const sp<SurfaceFlinger>& flinger,
            uint32_t displayIndex);

    ~DisplayHardwareBase();

    void startSleepManagement() const;

    // console management
    void releaseScreen() const;
    void acquireScreen() const;
    bool isScreenAcquired() const;

    bool canDraw() const;


private:
    class DisplayEventThread : public Thread {
        wp<SurfaceFlinger> mFlinger;
        status_t waitForFbSleep();
        status_t waitForFbWake();
    public:
        DisplayEventThread(const sp<SurfaceFlinger>& flinger);
        virtual ~DisplayEventThread();
        virtual bool threadLoop();
        status_t initCheck() const;
    private:
        int always_on;
    };

    sp<DisplayEventThread>  mDisplayEventThread;
    mutable int             mScreenAcquired;
};

}; // namespace android

#endif // ANDROID_DISPLAY_HARDWARE_BASE_H
