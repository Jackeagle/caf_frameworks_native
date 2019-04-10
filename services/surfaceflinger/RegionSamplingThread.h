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

#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <android-base/thread_annotations.h>
#include <binder/IBinder.h>
#include <ui/Rect.h>
#include <utils/StrongPointer.h>
#include "Scheduler/IdleTimer.h"

namespace android {

class GraphicBuffer;
class IRegionSamplingListener;
class Layer;
class Scheduler;
class SurfaceFlinger;
struct SamplingOffsetCallback;

class RegionSamplingThread : public IBinder::DeathRecipient {
public:
    struct TimingTunables {
        // debug.sf.sampling_offset_ns
        // When asynchronously collecting sample, the offset, from zero phase in the vsync timeline
        // at which the sampling should start.
        std::chrono::nanoseconds mSamplingOffset;
        // debug.sf.sampling_period_ns
        // This is the maximum amount of time the luma recieving client
        // should have to wait for a new luma value after a frame is updated. The inverse of this is
        // roughly the sampling rate. Sampling system rounds up sub-vsync sampling period to vsync
        // period.
        std::chrono::nanoseconds mSamplingPeriod;
        // debug.sf.sampling_timer_timeout_ns
        // This is the interval at which the luma sampling system will check that the luma clients
        // have up to date information. It defaults to the mSamplingPeriod.
        std::chrono::nanoseconds mSamplingTimerTimeout;
    };
    struct EnvironmentTimingTunables : TimingTunables {
        EnvironmentTimingTunables();
    };
    explicit RegionSamplingThread(SurfaceFlinger& flinger, Scheduler& scheduler,
                                  const TimingTunables& tunables);
    explicit RegionSamplingThread(SurfaceFlinger& flinger, Scheduler& scheduler);

    ~RegionSamplingThread();

    // Add a listener to receive luma notifications. The luma reported via listener will
    // report the median luma for the layers under the stopLayerHandle, in the samplingArea region.
    void addListener(const Rect& samplingArea, const sp<IBinder>& stopLayerHandle,
                     const sp<IRegionSamplingListener>& listener);
    // Remove the listener to stop receiving median luma notifications.
    void removeListener(const sp<IRegionSamplingListener>& listener);

    // Notifies sampling engine that new content is available. This will trigger a sampling
    // pass at some point in the future.
    void notifyNewContent();

    // Notifies the sampling engine that it has a good timing window in which to sample.
    void notifySamplingOffset();

private:
    struct Descriptor {
        Rect area = Rect::EMPTY_RECT;
        wp<Layer> stopLayer;
        sp<IRegionSamplingListener> listener;
    };

    struct WpHash {
        size_t operator()(const wp<IBinder>& p) const {
            return std::hash<IBinder*>()(p.unsafe_get());
        }
    };
    std::vector<float> sampleBuffer(
            const sp<GraphicBuffer>& buffer, const Point& leftTop,
            const std::vector<RegionSamplingThread::Descriptor>& descriptors);

    void doSample();
    void binderDied(const wp<IBinder>& who) override;
    void checkForStaleLuma();

    void captureSample() REQUIRES(mMutex);
    void threadMain();

    SurfaceFlinger& mFlinger;
    Scheduler& mScheduler;
    const TimingTunables mTunables;
    scheduler::IdleTimer mIdleTimer;

    std::unique_ptr<SamplingOffsetCallback> const mPhaseCallback;

    std::mutex mThreadMutex;
    std::thread mThread GUARDED_BY(mThreadMutex);

    std::mutex mMutex;
    std::condition_variable_any mCondition;
    bool mRunning GUARDED_BY(mMutex) = true;
    bool mSampleRequested GUARDED_BY(mMutex) = false;

    std::unordered_map<wp<IBinder>, Descriptor, WpHash> mDescriptors GUARDED_BY(mMutex);
    std::chrono::nanoseconds lastSampleTime GUARDED_BY(mMutex);
    bool mDiscardedFrames GUARDED_BY(mMutex) = false;
};

} // namespace android
