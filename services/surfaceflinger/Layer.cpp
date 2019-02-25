/*
 * Copyright (C) 2007 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "Layer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <algorithm>

#include <android-base/stringprintf.h>

#include <cutils/compiler.h>
#include <cutils/native_handle.h>
#include <cutils/properties.h>

#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/NativeHandle.h>
#include <utils/StopWatch.h>
#include <utils/Trace.h>

#include <ui/DebugUtils.h>
#include <ui/GraphicBuffer.h>
#include <ui/PixelFormat.h>

#include <gui/BufferItem.h>
#include <gui/LayerDebugInfo.h>
#include <gui/Surface.h>

#include "BufferLayer.h"
#include "ColorLayer.h"
#include "Colorizer.h"
#include "DisplayDevice.h"
#include "Layer.h"
#include "LayerRejecter.h"
#include "MonitoredProducer.h"
#include "SurfaceFlinger.h"

#include "DisplayHardware/HWComposer.h"
#include "TimeStats/TimeStats.h"

#include <renderengine/RenderEngine.h>

#include <mutex>
#include "LayerProtoHelper.h"

#define DEBUG_RESIZE 0

namespace android {

using base::StringAppendF;

std::atomic<int32_t> Layer::sSequence{1};

Layer::Layer(const LayerCreationArgs& args)
      : mFlinger(args.flinger),
        mName(args.name),
        mClientRef(args.client),
        mBE{this, args.name.string()} {
    mCurrentCrop.makeInvalid();

    uint32_t layerFlags = 0;
    if (args.flags & ISurfaceComposerClient::eHidden) layerFlags |= layer_state_t::eLayerHidden;
    if (args.flags & ISurfaceComposerClient::eOpaque) layerFlags |= layer_state_t::eLayerOpaque;
    if (args.flags & ISurfaceComposerClient::eSecure) layerFlags |= layer_state_t::eLayerSecure;

    mTransactionName = String8("TX - ") + mName;

    mCurrentState.active_legacy.w = args.w;
    mCurrentState.active_legacy.h = args.h;
    mCurrentState.flags = layerFlags;
    mCurrentState.active_legacy.transform.set(0, 0);
    mCurrentState.crop_legacy.makeInvalid();
    mCurrentState.requestedCrop_legacy = mCurrentState.crop_legacy;
    mCurrentState.z = 0;
    mCurrentState.color.a = 1.0f;
    mCurrentState.layerStack = 0;
    mCurrentState.sequence = 0;
    mCurrentState.requested_legacy = mCurrentState.active_legacy;
    mCurrentState.active.w = UINT32_MAX;
    mCurrentState.active.h = UINT32_MAX;
    mCurrentState.active.transform.set(0, 0);
    mCurrentState.transform = 0;
    mCurrentState.transformToDisplayInverse = false;
    mCurrentState.crop.makeInvalid();
    mCurrentState.acquireFence = new Fence(-1);
    mCurrentState.dataspace = ui::Dataspace::UNKNOWN;
    mCurrentState.hdrMetadata.validTypes = 0;
    mCurrentState.surfaceDamageRegion.clear();
    mCurrentState.cornerRadius = 0.0f;
    mCurrentState.api = -1;
    mCurrentState.hasColorTransform = false;

    // drawing state & current state are identical
    mDrawingState = mCurrentState;

    CompositorTiming compositorTiming;
    args.flinger->getCompositorTiming(&compositorTiming);
    mFrameEventHistory.initializeCompositorTiming(compositorTiming);
    mFrameTracker.setDisplayRefreshPeriod(compositorTiming.interval);

    mFlinger->onLayerCreated();
}

Layer::~Layer() {
    sp<Client> c(mClientRef.promote());
    if (c != 0) {
        c->detachLayer(this);
    }

    mFrameTracker.logAndResetStats(mName);

    destroyAllHwcLayersPlusChildren();

    mFlinger->onLayerDestroyed();
}

// ---------------------------------------------------------------------------
// callbacks
// ---------------------------------------------------------------------------

/*
 * onLayerDisplayed is only meaningful for BufferLayer, but, is called through
 * Layer.  So, the implementation is done in BufferLayer.  When called on a
 * ColorLayer object, it's essentially a NOP.
 */
void Layer::onLayerDisplayed(const sp<Fence>& /*releaseFence*/) {}

void Layer::onRemovedFromCurrentState() {
    mRemovedFromCurrentState = true;

    // the layer is removed from SF mCurrentState to mLayersPendingRemoval
    if (mCurrentState.zOrderRelativeOf != nullptr) {
        sp<Layer> strongRelative = mCurrentState.zOrderRelativeOf.promote();
        if (strongRelative != nullptr) {
            strongRelative->removeZOrderRelative(this);
            mFlinger->setTransactionFlags(eTraversalNeeded);
        }
        mCurrentState.zOrderRelativeOf = nullptr;
    }

    // Since we are no longer reachable from CurrentState SurfaceFlinger
    // will no longer invoke doTransaction for us, and so we will
    // never finish applying transactions. We signal the sync point
    // now so that another layer will not become indefinitely
    // blocked.
    for (auto& point: mRemoteSyncPoints) {
        point->setTransactionApplied();
    }
    mRemoteSyncPoints.clear();

    {
    Mutex::Autolock syncLock(mLocalSyncPointMutex);
    for (auto& point : mLocalSyncPoints) {
        point->setFrameAvailable();
    }
    mLocalSyncPoints.clear();
    }

    for (const auto& child : mCurrentChildren) {
        child->onRemovedFromCurrentState();
    }

    mFlinger->markLayerPendingRemovalLocked(this);
}

void Layer::addToCurrentState() {
    mRemovedFromCurrentState = false;

    for (const auto& child : mCurrentChildren) {
        child->addToCurrentState();
    }
}

// ---------------------------------------------------------------------------
// set-up
// ---------------------------------------------------------------------------

const String8& Layer::getName() const {
    return mName;
}

bool Layer::getPremultipledAlpha() const {
    return mPremultipliedAlpha;
}

sp<IBinder> Layer::getHandle() {
    Mutex::Autolock _l(mLock);
    return new Handle(mFlinger, this);
}

// ---------------------------------------------------------------------------
// h/w composer set-up
// ---------------------------------------------------------------------------

bool Layer::createHwcLayer(HWComposer* hwc, DisplayId displayId) {
    LOG_ALWAYS_FATAL_IF(hasHwcLayer(displayId), "Already have a layer for display %s",
                        to_string(displayId).c_str());
    auto layer = std::shared_ptr<HWC2::Layer>(
            hwc->createLayer(displayId),
            [hwc, displayId](HWC2::Layer* layer) {
               hwc->destroyLayer(displayId, layer); });
    if (!layer) {
        return false;
    }
    LayerBE::HWCInfo& hwcInfo = getBE().mHwcLayers[displayId];
    hwcInfo.hwc = hwc;
    hwcInfo.layer = layer;
    layer->setLayerDestroyedListener(
            [this, displayId](HWC2::Layer* /*layer*/) { getBE().mHwcLayers.erase(displayId); });
    return true;
}

bool Layer::destroyHwcLayer(DisplayId displayId) {
    if (!hasHwcLayer(displayId)) {
        return false;
    }
    auto& hwcInfo = getBE().mHwcLayers[displayId];
    LOG_ALWAYS_FATAL_IF(hwcInfo.layer == nullptr, "Attempt to destroy null layer");
    LOG_ALWAYS_FATAL_IF(hwcInfo.hwc == nullptr, "Missing HWComposer");
    hwcInfo.layer = nullptr;

    return true;
}

void Layer::destroyHwcLayersForAllDisplays() {
    size_t numLayers = getBE().mHwcLayers.size();
    for (size_t i = 0; i < numLayers; ++i) {
        LOG_ALWAYS_FATAL_IF(getBE().mHwcLayers.empty(), "destroyAllHwcLayers failed");
        destroyHwcLayer(getBE().mHwcLayers.begin()->first);
    }
}

void Layer::destroyAllHwcLayersPlusChildren() {
    destroyHwcLayersForAllDisplays();
    LOG_ALWAYS_FATAL_IF(!getBE().mHwcLayers.empty(),
                        "All hardware composer layers should have been destroyed");

    for (const sp<Layer>& child : mDrawingChildren) {
        child->destroyAllHwcLayersPlusChildren();
    }
}

Rect Layer::getContentCrop() const {
    // this is the crop rectangle that applies to the buffer
    // itself (as opposed to the window)
    Rect crop;
    if (!mCurrentCrop.isEmpty()) {
        // if the buffer crop is defined, we use that
        crop = mCurrentCrop;
    } else if (getBE().compositionInfo.mBuffer != nullptr) {
        // otherwise we use the whole buffer
        crop = getBE().compositionInfo.mBuffer->getBounds();
    } else {
        // if we don't have a buffer yet, we use an empty/invalid crop
        crop.makeInvalid();
    }
    return crop;
}

static Rect reduce(const Rect& win, const Region& exclude) {
    if (CC_LIKELY(exclude.isEmpty())) {
        return win;
    }
    if (exclude.isRect()) {
        return win.reduce(exclude.getBounds());
    }
    return Region(win).subtract(exclude).getBounds();
}

static FloatRect reduce(const FloatRect& win, const Region& exclude) {
    if (CC_LIKELY(exclude.isEmpty())) {
        return win;
    }
    // Convert through Rect (by rounding) for lack of FloatRegion
    return Region(Rect{win}).subtract(exclude).getBounds().toFloatRect();
}

Rect Layer::computeScreenBounds(bool reduceTransparentRegion) const {
    const State& s(getDrawingState());
    Region transparentRegion = reduceTransparentRegion ? getActiveTransparentRegion(s) : Region();
    FloatRect bounds = computeBounds(transparentRegion);
    ui::Transform t = getTransform();
    // Transform to screen space.
    bounds = t.transform(bounds);
    return Rect{bounds};
}

FloatRect Layer::computeBounds() const {
    const State& s(getDrawingState());
    return computeBounds(getActiveTransparentRegion(s));
}

FloatRect Layer::computeBounds(const Region& activeTransparentRegion) const {
    const State& s(getDrawingState());
    Rect bounds = getCroppedBufferSize(s);
    FloatRect floatBounds = bounds.toFloatRect();
    if (bounds.isValid()) {
        // Layer has bounds. Pass in our bounds as a special case. Then pass on to our parents so
        // that they can clip it.
        floatBounds = cropChildBounds(floatBounds);
    } else {
        // Layer does not have bounds, so we fill to our parent bounds. This is done by getting our
        // parent bounds and inverting the transform to get the maximum bounds we can have that
        // will fit within our parent bounds.
        const auto& p = mDrawingParent.promote();
        if (p != nullptr) {
            ui::Transform t = s.active_legacy.transform;
            // When calculating the parent bounds for purposes of clipping, we don't need to
            // constrain the parent to its transparent region. The transparent region is an
            // optimization based on the buffer contents of the layer, but does not affect the
            // space allocated to it by policy, and thus children should be allowed to extend into
            // the parent's transparent region.
            // One of the main uses is a parent window with a child sitting behind the parent
            // window, marked by a transparent region. When computing the parent bounds from the
            // parent's perspective we pass in the transparent region to reduce buffer allocation
            // size. When computing the parent bounds from the child's perspective, we pass in an
            // empty transparent region in order to extend into the the parent bounds.
            floatBounds = p->computeBounds(Region());
            // Transform back to layer space.
            floatBounds = t.inverse().transform(floatBounds);
        }
    }

    // Subtract the transparent region and snap to the bounds.
    return reduce(floatBounds, activeTransparentRegion);
}

FloatRect Layer::cropChildBounds(const FloatRect& childBounds) const {
    const State& s(getDrawingState());
    Rect bounds = getCroppedBufferSize(s);
    FloatRect croppedBounds = childBounds;

    // If the layer has bounds, then crop the passed in child bounds and pass
    // it to our parents so they can crop it as well. If the layer has no bounds,
    // then pass on the child bounds.
    if (bounds.isValid()) {
        croppedBounds = croppedBounds.intersect(bounds.toFloatRect());
    }

    const auto& p = mDrawingParent.promote();
    if (p != nullptr) {
        // Transform to parent space and allow parent layer to crop the
        // child bounds as well.
        ui::Transform t = s.active_legacy.transform;
        croppedBounds = t.transform(croppedBounds);
        croppedBounds = p->cropChildBounds(croppedBounds);
        croppedBounds = t.inverse().transform(croppedBounds);
    }
    return croppedBounds;
}

Rect Layer::getCroppedBufferSize(const State& s) const {
    Rect size = getBufferSize(s);
    Rect crop = getCrop(s);
    if (!crop.isEmpty() && size.isValid()) {
        size.intersect(crop, &size);
    } else if (!crop.isEmpty()) {
        size = crop;
    }
    return size;
}

Rect Layer::computeInitialCrop(const sp<const DisplayDevice>& display) const {
    // the crop is the area of the window that gets cropped, but not
    // scaled in any ways.
    const State& s(getDrawingState());

    // apply the projection's clipping to the window crop in
    // layerstack space, and convert-back to layer space.
    // if there are no window scaling involved, this operation will map to full
    // pixels in the buffer.

    FloatRect activeCropFloat = computeBounds();
    ui::Transform t = getTransform();
    // Transform to screen space.
    activeCropFloat = t.transform(activeCropFloat);
    activeCropFloat = activeCropFloat.intersect(display->getViewport().toFloatRect());
    // Back to layer space to work with the content crop.
    activeCropFloat = t.inverse().transform(activeCropFloat);
    // This needs to be here as transform.transform(Rect) computes the
    // transformed rect and then takes the bounding box of the result before
    // returning. This means
    // transform.inverse().transform(transform.transform(Rect)) != Rect
    // in which case we need to make sure the final rect is clipped to the
    // display bounds.
    Rect activeCrop{activeCropFloat};
    if (!activeCrop.intersect(getBufferSize(s), &activeCrop)) {
        activeCrop.clear();
    }
    return activeCrop;
}

void Layer::setupRoundedCornersCropCoordinates(Rect win,
                                               const FloatRect& roundedCornersCrop) const {
    // Translate win by the rounded corners rect coordinates, to have all values in
    // layer coordinate space.
    win.left -= roundedCornersCrop.left;
    win.right -= roundedCornersCrop.left;
    win.top -= roundedCornersCrop.top;
    win.bottom -= roundedCornersCrop.top;

    renderengine::Mesh::VertexArray<vec2> cropCoords(getBE().mMesh.getCropCoordArray<vec2>());
    cropCoords[0] = vec2(win.left, win.top);
    cropCoords[1] = vec2(win.left, win.top + win.getHeight());
    cropCoords[2] = vec2(win.right, win.top + win.getHeight());
    cropCoords[3] = vec2(win.right, win.top);
}

FloatRect Layer::computeCrop(const sp<const DisplayDevice>& display) const {
    // the content crop is the area of the content that gets scaled to the
    // layer's size. This is in buffer space.
    FloatRect crop = getContentCrop().toFloatRect();

    // In addition there is a WM-specified crop we pull from our drawing state.
    const State& s(getDrawingState());

    Rect activeCrop = computeInitialCrop(display);
    Rect bufferSize = getBufferSize(s);

    // Transform the window crop to match the buffer coordinate system,
    // which means using the inverse of the current transform set on the
    // SurfaceFlingerConsumer.
    uint32_t invTransform = mCurrentTransform;
    if (getTransformToDisplayInverse()) {
        /*
         * the code below applies the primary display's inverse transform to the
         * buffer
         */
        uint32_t invTransformOrient = DisplayDevice::getPrimaryDisplayOrientationTransform();
        // calculate the inverse transform
        if (invTransformOrient & NATIVE_WINDOW_TRANSFORM_ROT_90) {
            invTransformOrient ^= NATIVE_WINDOW_TRANSFORM_FLIP_V | NATIVE_WINDOW_TRANSFORM_FLIP_H;
        }
        // and apply to the current transform
        invTransform = (ui::Transform(invTransformOrient) *
                        ui::Transform(invTransform)).getOrientation();
    }

    int winWidth = bufferSize.getWidth();
    int winHeight = bufferSize.getHeight();
    if (invTransform & NATIVE_WINDOW_TRANSFORM_ROT_90) {
        // If the activeCrop has been rotate the ends are rotated but not
        // the space itself so when transforming ends back we can't rely on
        // a modification of the axes of rotation. To account for this we
        // need to reorient the inverse rotation in terms of the current
        // axes of rotation.
        bool is_h_flipped = (invTransform & NATIVE_WINDOW_TRANSFORM_FLIP_H) != 0;
        bool is_v_flipped = (invTransform & NATIVE_WINDOW_TRANSFORM_FLIP_V) != 0;
        if (is_h_flipped == is_v_flipped) {
            invTransform ^= NATIVE_WINDOW_TRANSFORM_FLIP_V | NATIVE_WINDOW_TRANSFORM_FLIP_H;
        }
        std::swap(winWidth, winHeight);
    }
    const Rect winCrop =
            activeCrop.transform(invTransform, bufferSize.getWidth(), bufferSize.getHeight());

    // below, crop is intersected with winCrop expressed in crop's coordinate space
    float xScale = crop.getWidth() / float(winWidth);
    float yScale = crop.getHeight() / float(winHeight);

    float insetL = winCrop.left * xScale;
    float insetT = winCrop.top * yScale;
    float insetR = (winWidth - winCrop.right) * xScale;
    float insetB = (winHeight - winCrop.bottom) * yScale;

    crop.left += insetL;
    crop.top += insetT;
    crop.right -= insetR;
    crop.bottom -= insetB;

    return crop;
}

void Layer::setGeometry(const sp<const DisplayDevice>& display, uint32_t z) {
    const auto displayId = display->getId();
    LOG_ALWAYS_FATAL_IF(!displayId);
    RETURN_IF_NO_HWC_LAYER(*displayId);
    auto& hwcInfo = getBE().mHwcLayers[*displayId];

    // enable this layer
    hwcInfo.forceClientComposition = false;

    if (isSecure() && !display->isSecure()) {
        hwcInfo.forceClientComposition = true;
    }

    auto& hwcLayer = hwcInfo.layer;

    // this gives us only the "orientation" component of the transform
    const State& s(getDrawingState());
    const Rect bufferSize = getBufferSize(s);
    auto blendMode = HWC2::BlendMode::None;
    if (!isOpaque(s) || getAlpha() != 1.0f) {
        blendMode =
                mPremultipliedAlpha ? HWC2::BlendMode::Premultiplied : HWC2::BlendMode::Coverage;
    }
    auto error = hwcLayer->setBlendMode(blendMode);
    ALOGE_IF(error != HWC2::Error::None,
             "[%s] Failed to set blend mode %s:"
             " %s (%d)",
             mName.string(), to_string(blendMode).c_str(), to_string(error).c_str(),
             static_cast<int32_t>(error));
    getBE().compositionInfo.hwc.blendMode = blendMode;

    // apply the layer's transform, followed by the display's global transform
    // here we're guaranteed that the layer's transform preserves rects
    Region activeTransparentRegion(getActiveTransparentRegion(s));
    ui::Transform t = getTransform();
    Rect activeCrop = getCrop(s);
    if (!activeCrop.isEmpty() && bufferSize.isValid()) {
        activeCrop = t.transform(activeCrop);
        if (!activeCrop.intersect(display->getViewport(), &activeCrop)) {
            activeCrop.clear();
        }
        activeCrop = t.inverse().transform(activeCrop, true);
        // This needs to be here as transform.transform(Rect) computes the
        // transformed rect and then takes the bounding box of the result before
        // returning. This means
        // transform.inverse().transform(transform.transform(Rect)) != Rect
        // in which case we need to make sure the final rect is clipped to the
        // display bounds.
        if (!activeCrop.intersect(bufferSize, &activeCrop)) {
            activeCrop.clear();
        }
        // mark regions outside the crop as transparent
        activeTransparentRegion.orSelf(Rect(0, 0, bufferSize.getWidth(), activeCrop.top));
        activeTransparentRegion.orSelf(
                Rect(0, activeCrop.bottom, bufferSize.getWidth(), bufferSize.getHeight()));
        activeTransparentRegion.orSelf(Rect(0, activeCrop.top, activeCrop.left, activeCrop.bottom));
        activeTransparentRegion.orSelf(
                Rect(activeCrop.right, activeCrop.top, bufferSize.getWidth(), activeCrop.bottom));
    }

    // computeBounds returns a FloatRect to provide more accuracy during the
    // transformation. We then round upon constructing 'frame'.
    Rect frame{t.transform(computeBounds(activeTransparentRegion))};
    if (!frame.intersect(display->getViewport(), &frame)) {
        frame.clear();
    }
    const ui::Transform& tr = display->getTransform();
    Rect transformedFrame = tr.transform(frame);
    error = hwcLayer->setDisplayFrame(transformedFrame);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set display frame [%d, %d, %d, %d]: %s (%d)", mName.string(),
              transformedFrame.left, transformedFrame.top, transformedFrame.right,
              transformedFrame.bottom, to_string(error).c_str(), static_cast<int32_t>(error));
    } else {
        hwcInfo.displayFrame = transformedFrame;
    }
    getBE().compositionInfo.hwc.displayFrame = transformedFrame;

    FloatRect sourceCrop = computeCrop(display);
    error = hwcLayer->setSourceCrop(sourceCrop);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set source crop [%.3f, %.3f, %.3f, %.3f]: "
              "%s (%d)",
              mName.string(), sourceCrop.left, sourceCrop.top, sourceCrop.right, sourceCrop.bottom,
              to_string(error).c_str(), static_cast<int32_t>(error));
    } else {
        hwcInfo.sourceCrop = sourceCrop;
    }
    getBE().compositionInfo.hwc.sourceCrop = sourceCrop;

    float alpha = static_cast<float>(getAlpha());
    error = hwcLayer->setPlaneAlpha(alpha);
    ALOGE_IF(error != HWC2::Error::None,
             "[%s] Failed to set plane alpha %.3f: "
             "%s (%d)",
             mName.string(), alpha, to_string(error).c_str(), static_cast<int32_t>(error));
    getBE().compositionInfo.hwc.alpha = alpha;

    error = hwcLayer->setZOrder(z);
    ALOGE_IF(error != HWC2::Error::None, "[%s] Failed to set Z %u: %s (%d)", mName.string(), z,
             to_string(error).c_str(), static_cast<int32_t>(error));
    getBE().compositionInfo.hwc.z = z;

    int type = s.metadata.getInt32(METADATA_WINDOW_TYPE, 0);
    int appId = s.metadata.getInt32(METADATA_OWNER_UID, 0);
    sp<Layer> parent = mDrawingParent.promote();
    if (parent.get()) {
        auto& parentState = parent->getDrawingState();
        const int parentType = parentState.metadata.getInt32(METADATA_WINDOW_TYPE, 0);
        const int parentAppId = parentState.metadata.getInt32(METADATA_OWNER_UID, 0);
        if (parentType >= 0 || parentAppId >= 0) {
            type = parentType;
            appId = parentAppId;
        }
    }

    error = hwcLayer->setInfo(type, appId);
    ALOGE_IF(error != HWC2::Error::None, "[%s] Failed to set info (%d)", mName.string(),
             static_cast<int32_t>(error));

    getBE().compositionInfo.hwc.type = type;
    getBE().compositionInfo.hwc.appId = appId;

    /*
     * Transformations are applied in this order:
     * 1) buffer orientation/flip/mirror
     * 2) state transformation (window manager)
     * 3) layer orientation (screen orientation)
     * (NOTE: the matrices are multiplied in reverse order)
     */

    const ui::Transform bufferOrientation(mCurrentTransform);
    ui::Transform transform(tr * t * bufferOrientation);

    if (getTransformToDisplayInverse()) {
        /*
         * the code below applies the primary display's inverse transform to the
         * buffer
         */
        uint32_t invTransform = DisplayDevice::getPrimaryDisplayOrientationTransform();
        // calculate the inverse transform
        if (invTransform & NATIVE_WINDOW_TRANSFORM_ROT_90) {
            invTransform ^= NATIVE_WINDOW_TRANSFORM_FLIP_V | NATIVE_WINDOW_TRANSFORM_FLIP_H;
        }

        /*
         * Here we cancel out the orientation component of the WM transform.
         * The scaling and translate components are already included in our bounds
         * computation so it's enough to just omit it in the composition.
         * See comment in onDraw with ref to b/36727915 for why.
         */
        transform = ui::Transform(invTransform) * tr * bufferOrientation;
    }

    // this gives us only the "orientation" component of the transform
    const uint32_t orientation = transform.getOrientation();
    if (orientation & ui::Transform::ROT_INVALID) {
        // we can only handle simple transformation
        hwcInfo.forceClientComposition = true;
        getBE().mHwcLayers[*displayId].compositionType = HWC2::Composition::Client;
    } else {
        auto transform = static_cast<HWC2::Transform>(orientation);
        hwcInfo.transform = transform;
        auto error = hwcLayer->setTransform(transform);
        ALOGE_IF(error != HWC2::Error::None,
                 "[%s] Failed to set transform %s: "
                 "%s (%d)",
                 mName.string(), to_string(transform).c_str(), to_string(error).c_str(),
                 static_cast<int32_t>(error));
        getBE().compositionInfo.hwc.transform = transform;
    }
}

void Layer::forceClientComposition(DisplayId displayId) {
    RETURN_IF_NO_HWC_LAYER(displayId);
    getBE().mHwcLayers[displayId].forceClientComposition = true;
}

bool Layer::getForceClientComposition(DisplayId displayId) {
    RETURN_IF_NO_HWC_LAYER(displayId, false);
    return getBE().mHwcLayers[displayId].forceClientComposition;
}

void Layer::updateCursorPosition(const sp<const DisplayDevice>& display) {
    const auto displayId = display->getId();
    LOG_ALWAYS_FATAL_IF(!displayId);
    if (!hasHwcLayer(*displayId) || getCompositionType(displayId) != HWC2::Composition::Cursor) {
        return;
    }

    // This gives us only the "orientation" component of the transform
    const State& s(getCurrentState());

    // Apply the layer's transform, followed by the display's global transform
    // Here we're guaranteed that the layer's transform preserves rects
    Rect win = getCroppedBufferSize(s);
    // Subtract the transparent region and snap to the bounds
    Rect bounds = reduce(win, getActiveTransparentRegion(s));
    Rect frame(getTransform().transform(bounds));
    frame.intersect(display->getViewport(), &frame);
    auto& displayTransform = display->getTransform();
    auto position = displayTransform.transform(frame);

    auto error =
            getBE().mHwcLayers[*displayId].layer->setCursorPosition(position.left, position.top);

    ALOGE_IF(error != HWC2::Error::None,
             "[%s] Failed to set cursor position "
             "to (%d, %d): %s (%d)",
             mName.string(), position.left, position.top, to_string(error).c_str(),
             static_cast<int32_t>(error));
}

// ---------------------------------------------------------------------------
// drawing...
// ---------------------------------------------------------------------------

void Layer::draw(const RenderArea& renderArea, const Region& clip) {
    onDraw(renderArea, clip, false);
}

void Layer::draw(const RenderArea& renderArea, bool useIdentityTransform) {
    onDraw(renderArea, Region(renderArea.getBounds()), useIdentityTransform);
}

void Layer::clearWithOpenGL(const RenderArea& renderArea, float red, float green, float blue,
                            float alpha) const {
    auto& engine(mFlinger->getRenderEngine());
    computeGeometry(renderArea, getBE().mMesh, false);
    engine.setupFillWithColor(red, green, blue, alpha);
    engine.drawMesh(getBE().mMesh);
}

void Layer::clearWithOpenGL(const RenderArea& renderArea) const {
    clearWithOpenGL(renderArea, 0, 0, 0, 0);
}

void Layer::setCompositionType(DisplayId displayId, HWC2::Composition type, bool callIntoHwc) {
    if (getBE().mHwcLayers.count(displayId) == 0) {
        ALOGE("setCompositionType called without a valid HWC layer");
        return;
    }
    auto& hwcInfo = getBE().mHwcLayers[displayId];
    auto& hwcLayer = hwcInfo.layer;
    ALOGV("setCompositionType(%" PRIx64 ", %s, %d)", (hwcLayer)->getId(), to_string(type).c_str(),
          static_cast<int>(callIntoHwc));
    if (hwcInfo.compositionType != type) {
        ALOGV("    actually setting");
        hwcInfo.compositionType = type;
        if (callIntoHwc) {
            auto error = (hwcLayer)->setCompositionType(type);
            ALOGE_IF(error != HWC2::Error::None,
                     "[%s] Failed to set "
                     "composition type %s: %s (%d)",
                     mName.string(), to_string(type).c_str(), to_string(error).c_str(),
                     static_cast<int32_t>(error));
        }
    }
}

HWC2::Composition Layer::getCompositionType(const std::optional<DisplayId>& displayId) const {
    if (!displayId) {
        // If we're querying the composition type for a display that does not
        // have a HWC counterpart, then it will always be Client
        return HWC2::Composition::Client;
    }
    if (getBE().mHwcLayers.count(*displayId) == 0) {
        ALOGE("getCompositionType called with an invalid HWC layer");
        return HWC2::Composition::Invalid;
    }
    return getBE().mHwcLayers.at(*displayId).compositionType;
}

void Layer::setClearClientTarget(DisplayId displayId, bool clear) {
    if (getBE().mHwcLayers.count(displayId) == 0) {
        ALOGE("setClearClientTarget called without a valid HWC layer");
        return;
    }
    getBE().mHwcLayers[displayId].clearClientTarget = clear;
}

bool Layer::getClearClientTarget(DisplayId displayId) const {
    if (getBE().mHwcLayers.count(displayId) == 0) {
        ALOGE("getClearClientTarget called without a valid HWC layer");
        return false;
    }
    return getBE().mHwcLayers.at(displayId).clearClientTarget;
}

bool Layer::addSyncPoint(const std::shared_ptr<SyncPoint>& point) {
    if (point->getFrameNumber() <= mCurrentFrameNumber) {
        // Don't bother with a SyncPoint, since we've already latched the
        // relevant frame
        return false;
    }
    if (isRemovedFromCurrentState()) {
        return false;
    }

    Mutex::Autolock lock(mLocalSyncPointMutex);
    mLocalSyncPoints.push_back(point);
    return true;
}

// ----------------------------------------------------------------------------
// local state
// ----------------------------------------------------------------------------

void Layer::computeGeometry(const RenderArea& renderArea,
                            renderengine::Mesh& mesh,
                            bool useIdentityTransform) const {
    const ui::Transform renderAreaTransform(renderArea.getTransform());
    FloatRect win = computeBounds();

    vec2 lt = vec2(win.left, win.top);
    vec2 lb = vec2(win.left, win.bottom);
    vec2 rb = vec2(win.right, win.bottom);
    vec2 rt = vec2(win.right, win.top);

    ui::Transform layerTransform = getTransform();
    if (!useIdentityTransform) {
        lt = layerTransform.transform(lt);
        lb = layerTransform.transform(lb);
        rb = layerTransform.transform(rb);
        rt = layerTransform.transform(rt);
    }

    renderengine::Mesh::VertexArray<vec2> position(mesh.getPositionArray<vec2>());
    position[0] = renderAreaTransform.transform(lt);
    position[1] = renderAreaTransform.transform(lb);
    position[2] = renderAreaTransform.transform(rb);
    position[3] = renderAreaTransform.transform(rt);
}

bool Layer::isSecure() const {
    const State& s(mDrawingState);
    return (s.flags & layer_state_t::eLayerSecure);
}

void Layer::setVisibleRegion(const Region& visibleRegion) {
    // always called from main thread
    this->visibleRegion = visibleRegion;
}

void Layer::setCoveredRegion(const Region& coveredRegion) {
    // always called from main thread
    this->coveredRegion = coveredRegion;
}

void Layer::setVisibleNonTransparentRegion(const Region& setVisibleNonTransparentRegion) {
    // always called from main thread
    this->visibleNonTransparentRegion = setVisibleNonTransparentRegion;
}

void Layer::clearVisibilityRegions() {
    visibleRegion.clear();
    visibleNonTransparentRegion.clear();
    coveredRegion.clear();
}

// ----------------------------------------------------------------------------
// transaction
// ----------------------------------------------------------------------------

void Layer::pushPendingState() {
    if (!mCurrentState.modified) {
        return;
    }

    // If this transaction is waiting on the receipt of a frame, generate a sync
    // point and send it to the remote layer.
    // We don't allow installing sync points after we are removed from the current state
    // as we won't be able to signal our end.
    if (mCurrentState.barrierLayer_legacy != nullptr && !isRemovedFromCurrentState()) {
        sp<Layer> barrierLayer = mCurrentState.barrierLayer_legacy.promote();
        if (barrierLayer == nullptr) {
            ALOGE("[%s] Unable to promote barrier Layer.", mName.string());
            // If we can't promote the layer we are intended to wait on,
            // then it is expired or otherwise invalid. Allow this transaction
            // to be applied as per normal (no synchronization).
            mCurrentState.barrierLayer_legacy = nullptr;
        } else {
            auto syncPoint = std::make_shared<SyncPoint>(mCurrentState.frameNumber_legacy);
            if (barrierLayer->addSyncPoint(syncPoint)) {
                mRemoteSyncPoints.push_back(std::move(syncPoint));
            } else {
                // We already missed the frame we're supposed to synchronize
                // on, so go ahead and apply the state update
                mCurrentState.barrierLayer_legacy = nullptr;
            }
        }

        // Wake us up to check if the frame has been received
        setTransactionFlags(eTransactionNeeded);
        mFlinger->setTransactionFlags(eTraversalNeeded);
    }
    mPendingStates.push_back(mCurrentState);
    ATRACE_INT(mTransactionName.string(), mPendingStates.size());
}

void Layer::popPendingState(State* stateToCommit) {
    *stateToCommit = mPendingStates[0];

    mPendingStates.removeAt(0);
    ATRACE_INT(mTransactionName.string(), mPendingStates.size());
}

bool Layer::applyPendingStates(State* stateToCommit) {
    bool stateUpdateAvailable = false;
    while (!mPendingStates.empty()) {
        if (mPendingStates[0].barrierLayer_legacy != nullptr) {
            if (mRemoteSyncPoints.empty()) {
                // If we don't have a sync point for this, apply it anyway. It
                // will be visually wrong, but it should keep us from getting
                // into too much trouble.
                ALOGE("[%s] No local sync point found", mName.string());
                popPendingState(stateToCommit);
                stateUpdateAvailable = true;
                continue;
            }

            if (mRemoteSyncPoints.front()->getFrameNumber() !=
                mPendingStates[0].frameNumber_legacy) {
                ALOGE("[%s] Unexpected sync point frame number found", mName.string());

                // Signal our end of the sync point and then dispose of it
                mRemoteSyncPoints.front()->setTransactionApplied();
                mRemoteSyncPoints.pop_front();
                continue;
            }

            if (mRemoteSyncPoints.front()->frameIsAvailable()) {
                // Apply the state update
                popPendingState(stateToCommit);
                stateUpdateAvailable = true;

                // Signal our end of the sync point and then dispose of it
                mRemoteSyncPoints.front()->setTransactionApplied();
                mRemoteSyncPoints.pop_front();
            } else {
                break;
            }
        } else {
            popPendingState(stateToCommit);
            stateUpdateAvailable = true;
        }
    }

    // If we still have pending updates, wake SurfaceFlinger back up and point
    // it at this layer so we can process them
    if (!mPendingStates.empty()) {
        setTransactionFlags(eTransactionNeeded);
        mFlinger->setTransactionFlags(eTraversalNeeded);
    }

    mCurrentState.modified = false;
    return stateUpdateAvailable;
}

uint32_t Layer::doTransactionResize(uint32_t flags, State* stateToCommit) {
    const State& s(getDrawingState());

    const bool sizeChanged = (stateToCommit->requested_legacy.w != s.requested_legacy.w) ||
            (stateToCommit->requested_legacy.h != s.requested_legacy.h);

    if (sizeChanged) {
        // the size changed, we need to ask our client to request a new buffer
        ALOGD_IF(DEBUG_RESIZE,
                 "doTransaction: geometry (layer=%p '%s'), tr=%02x, scalingMode=%d\n"
                 "  current={ active   ={ wh={%4u,%4u} crop={%4d,%4d,%4d,%4d} (%4d,%4d) }\n"
                 "            requested={ wh={%4u,%4u} }}\n"
                 "  drawing={ active   ={ wh={%4u,%4u} crop={%4d,%4d,%4d,%4d} (%4d,%4d) }\n"
                 "            requested={ wh={%4u,%4u} }}\n",
                 this, getName().string(), mCurrentTransform, getEffectiveScalingMode(),
                 stateToCommit->active_legacy.w, stateToCommit->active_legacy.h,
                 stateToCommit->crop_legacy.left, stateToCommit->crop_legacy.top,
                 stateToCommit->crop_legacy.right, stateToCommit->crop_legacy.bottom,
                 stateToCommit->crop_legacy.getWidth(), stateToCommit->crop_legacy.getHeight(),
                 stateToCommit->requested_legacy.w, stateToCommit->requested_legacy.h,
                 s.active_legacy.w, s.active_legacy.h, s.crop_legacy.left, s.crop_legacy.top,
                 s.crop_legacy.right, s.crop_legacy.bottom, s.crop_legacy.getWidth(),
                 s.crop_legacy.getHeight(), s.requested_legacy.w, s.requested_legacy.h);
    }

    // Don't let Layer::doTransaction update the drawing state
    // if we have a pending resize, unless we are in fixed-size mode.
    // the drawing state will be updated only once we receive a buffer
    // with the correct size.
    //
    // In particular, we want to make sure the clip (which is part
    // of the geometry state) is latched together with the size but is
    // latched immediately when no resizing is involved.
    //
    // If a sideband stream is attached, however, we want to skip this
    // optimization so that transactions aren't missed when a buffer
    // never arrives
    //
    // In the case that we don't have a buffer we ignore other factors
    // and avoid entering the resizePending state. At a high level the
    // resizePending state is to avoid applying the state of the new buffer
    // to the old buffer. However in the state where we don't have an old buffer
    // there is no such concern but we may still be being used as a parent layer.
    const bool resizePending =
            ((stateToCommit->requested_legacy.w != stateToCommit->active_legacy.w) ||
             (stateToCommit->requested_legacy.h != stateToCommit->active_legacy.h)) &&
            (getBE().compositionInfo.mBuffer != nullptr);
    if (!isFixedSize()) {
        if (resizePending && getBE().compositionInfo.hwc.sidebandStream == nullptr) {
            flags |= eDontUpdateGeometryState;
        }
    }

    // Here we apply various requested geometry states, depending on our
    // latching configuration. See Layer.h for a detailed discussion of
    // how geometry latching is controlled.
    if (!(flags & eDontUpdateGeometryState)) {
        State& editCurrentState(getCurrentState());

        // If mFreezeGeometryUpdates is true we are in the setGeometryAppliesWithResize
        // mode, which causes attributes which normally latch regardless of scaling mode,
        // to be delayed. We copy the requested state to the active state making sure
        // to respect these rules (again see Layer.h for a detailed discussion).
        //
        // There is an awkward asymmetry in the handling of the crop states in the position
        // states, as can be seen below. Largely this arises from position and transform
        // being stored in the same data structure while having different latching rules.
        // b/38182305
        //
        // Careful that "stateToCommit" and editCurrentState may not begin as equivalent due to
        // applyPendingStates in the presence of deferred transactions.
        if (mFreezeGeometryUpdates) {
            float tx = stateToCommit->active_legacy.transform.tx();
            float ty = stateToCommit->active_legacy.transform.ty();
            stateToCommit->active_legacy = stateToCommit->requested_legacy;
            stateToCommit->active_legacy.transform.set(tx, ty);
            editCurrentState.active_legacy = stateToCommit->active_legacy;
        } else {
            editCurrentState.active_legacy = editCurrentState.requested_legacy;
            stateToCommit->active_legacy = stateToCommit->requested_legacy;
        }
    }

    return flags;
}

uint32_t Layer::doTransaction(uint32_t flags) {
    ATRACE_CALL();

    if (mLayerDetached) {
        return 0;
    }

    pushPendingState();
    State c = getCurrentState();
    if (!applyPendingStates(&c)) {
        return 0;
    }

    flags = doTransactionResize(flags, &c);

    const State& s(getDrawingState());

    if (getActiveGeometry(c) != getActiveGeometry(s)) {
        // invalidate and recompute the visible regions if needed
        flags |= Layer::eVisibleRegion;
    }

    if (c.sequence != s.sequence) {
        // invalidate and recompute the visible regions if needed
        flags |= eVisibleRegion;
        this->contentDirty = true;

        // we may use linear filtering, if the matrix scales us
        const uint8_t type = getActiveTransform(c).getType();
        mNeedsFiltering = (!getActiveTransform(c).preserveRects() || type >= ui::Transform::SCALE);
    }

    if (mChildrenChanged) {
        flags |= eVisibleRegion;
        mChildrenChanged = false;
    }

    // If the layer is hidden, signal and clear out all local sync points so
    // that transactions for layers depending on this layer's frames becoming
    // visible are not blocked
    if (c.flags & layer_state_t::eLayerHidden) {
        clearSyncPoints();
    }

    if (mCurrentState.inputInfoChanged) {
        flags |= eInputInfoChanged;
        mCurrentState.inputInfoChanged = false;
    }

    // Commit the transaction
    commitTransaction(c);
    mCurrentState.callbackHandles = {};
    return flags;
}

void Layer::commitTransaction(const State& stateToCommit) {
    mDrawingState = stateToCommit;
}

uint32_t Layer::getTransactionFlags(uint32_t flags) {
    return mTransactionFlags.fetch_and(~flags) & flags;
}

uint32_t Layer::setTransactionFlags(uint32_t flags) {
    return mTransactionFlags.fetch_or(flags);
}

bool Layer::setPosition(float x, float y, bool immediate) {
    if (mCurrentState.requested_legacy.transform.tx() == x &&
        mCurrentState.requested_legacy.transform.ty() == y)
        return false;
    mCurrentState.sequence++;

    // We update the requested and active position simultaneously because
    // we want to apply the position portion of the transform matrix immediately,
    // but still delay scaling when resizing a SCALING_MODE_FREEZE layer.
    mCurrentState.requested_legacy.transform.set(x, y);
    if (immediate && !mFreezeGeometryUpdates) {
        // Here we directly update the active state
        // unlike other setters, because we store it within
        // the transform, but use different latching rules.
        // b/38182305
        mCurrentState.active_legacy.transform.set(x, y);
    }
    mFreezeGeometryUpdates = mFreezeGeometryUpdates || !immediate;

    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setChildLayer(const sp<Layer>& childLayer, int32_t z) {
    ssize_t idx = mCurrentChildren.indexOf(childLayer);
    if (idx < 0) {
        return false;
    }
    if (childLayer->setLayer(z)) {
        mCurrentChildren.removeAt(idx);
        mCurrentChildren.add(childLayer);
        return true;
    }
    return false;
}

bool Layer::setChildRelativeLayer(const sp<Layer>& childLayer,
        const sp<IBinder>& relativeToHandle, int32_t relativeZ) {
    ssize_t idx = mCurrentChildren.indexOf(childLayer);
    if (idx < 0) {
        return false;
    }
    if (childLayer->setRelativeLayer(relativeToHandle, relativeZ)) {
        mCurrentChildren.removeAt(idx);
        mCurrentChildren.add(childLayer);
        return true;
    }
    return false;
}

bool Layer::setLayer(int32_t z) {
    if (mCurrentState.z == z && !usingRelativeZ(LayerVector::StateSet::Current)) return false;
    mCurrentState.sequence++;
    mCurrentState.z = z;
    mCurrentState.modified = true;

    // Discard all relative layering.
    if (mCurrentState.zOrderRelativeOf != nullptr) {
        sp<Layer> strongRelative = mCurrentState.zOrderRelativeOf.promote();
        if (strongRelative != nullptr) {
            strongRelative->removeZOrderRelative(this);
        }
        mCurrentState.zOrderRelativeOf = nullptr;
    }
    setTransactionFlags(eTransactionNeeded);
    return true;
}

void Layer::removeZOrderRelative(const wp<Layer>& relative) {
    mCurrentState.zOrderRelatives.remove(relative);
    mCurrentState.sequence++;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
}

void Layer::addZOrderRelative(const wp<Layer>& relative) {
    mCurrentState.zOrderRelatives.add(relative);
    mCurrentState.modified = true;
    mCurrentState.sequence++;
    setTransactionFlags(eTransactionNeeded);
}

bool Layer::setRelativeLayer(const sp<IBinder>& relativeToHandle, int32_t relativeZ) {
    sp<Handle> handle = static_cast<Handle*>(relativeToHandle.get());
    if (handle == nullptr) {
        return false;
    }
    sp<Layer> relative = handle->owner.promote();
    if (relative == nullptr) {
        return false;
    }

    if (mCurrentState.z == relativeZ && usingRelativeZ(LayerVector::StateSet::Current) &&
        mCurrentState.zOrderRelativeOf == relative) {
        return false;
    }

    mCurrentState.sequence++;
    mCurrentState.modified = true;
    mCurrentState.z = relativeZ;

    auto oldZOrderRelativeOf = mCurrentState.zOrderRelativeOf.promote();
    if (oldZOrderRelativeOf != nullptr) {
        oldZOrderRelativeOf->removeZOrderRelative(this);
    }
    mCurrentState.zOrderRelativeOf = relative;
    relative->addZOrderRelative(this);

    setTransactionFlags(eTransactionNeeded);

    return true;
}

bool Layer::setSize(uint32_t w, uint32_t h) {
    if (mCurrentState.requested_legacy.w == w && mCurrentState.requested_legacy.h == h)
        return false;
    mCurrentState.requested_legacy.w = w;
    mCurrentState.requested_legacy.h = h;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);

    // record the new size, from this point on, when the client request
    // a buffer, it'll get the new size.
    setDefaultBufferSize(mCurrentState.requested_legacy.w, mCurrentState.requested_legacy.h);
    return true;
}
bool Layer::setAlpha(float alpha) {
    if (mCurrentState.color.a == alpha) return false;
    mCurrentState.sequence++;
    mCurrentState.color.a = alpha;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setBackgroundColor(const half3& color, float alpha, ui::Dataspace dataspace) {
    if (!mCurrentState.bgColorLayer && alpha == 0) {
        return false;
    } else if (!mCurrentState.bgColorLayer && alpha != 0) {
        // create background color layer if one does not yet exist
        uint32_t flags = ISurfaceComposerClient::eFXSurfaceColor;
        const String8& name = mName + "BackgroundColorLayer";
        mCurrentState.bgColorLayer =
                new ColorLayer(LayerCreationArgs(mFlinger.get(), nullptr, name, 0, 0, flags));

        // add to child list
        addChild(mCurrentState.bgColorLayer);
        mFlinger->mLayersAdded = true;
        // set up SF to handle added color layer
        if (isRemovedFromCurrentState()) {
            mCurrentState.bgColorLayer->onRemovedFromCurrentState();
        }
        mFlinger->setTransactionFlags(eTransactionNeeded);
    } else if (mCurrentState.bgColorLayer && alpha == 0) {
        mCurrentState.bgColorLayer->reparent(nullptr);
        mCurrentState.bgColorLayer = nullptr;
        return true;
    }

    mCurrentState.bgColorLayer->setColor(color);
    mCurrentState.bgColorLayer->setLayer(std::numeric_limits<int32_t>::min());
    mCurrentState.bgColorLayer->setAlpha(alpha);
    mCurrentState.bgColorLayer->setDataspace(dataspace);

    return true;
}

bool Layer::setCornerRadius(float cornerRadius) {
    if (mCurrentState.cornerRadius == cornerRadius) return false;

    mCurrentState.sequence++;
    mCurrentState.cornerRadius = cornerRadius;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setMatrix(const layer_state_t::matrix22_t& matrix,
        bool allowNonRectPreservingTransforms) {
    ui::Transform t;
    t.set(matrix.dsdx, matrix.dtdy, matrix.dtdx, matrix.dsdy);

    if (!allowNonRectPreservingTransforms && !t.preserveRects()) {
        ALOGW("Attempt to set rotation matrix without permission ACCESS_SURFACE_FLINGER ignored");
        return false;
    }
    mCurrentState.sequence++;
    mCurrentState.requested_legacy.transform.set(matrix.dsdx, matrix.dtdy, matrix.dtdx,
                                                 matrix.dsdy);
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setTransparentRegionHint(const Region& transparent) {
    mCurrentState.requestedTransparentRegion_legacy = transparent;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setFlags(uint8_t flags, uint8_t mask) {
    const uint32_t newFlags = (mCurrentState.flags & ~mask) | (flags & mask);
    if (mCurrentState.flags == newFlags) return false;
    mCurrentState.sequence++;
    mCurrentState.flags = newFlags;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setCrop_legacy(const Rect& crop, bool immediate) {
    if (mCurrentState.requestedCrop_legacy == crop) return false;
    mCurrentState.sequence++;
    mCurrentState.requestedCrop_legacy = crop;
    if (immediate && !mFreezeGeometryUpdates) {
        mCurrentState.crop_legacy = crop;
    }
    mFreezeGeometryUpdates = mFreezeGeometryUpdates || !immediate;

    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setOverrideScalingMode(int32_t scalingMode) {
    if (scalingMode == mOverrideScalingMode) return false;
    mOverrideScalingMode = scalingMode;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setMetadata(LayerMetadata data) {
    bool changed = data.mMap != mCurrentState.metadata.mMap;
    if (!changed) return false;
    mCurrentState.metadata = std::move(data);
    mCurrentState.sequence++;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setLayerStack(uint32_t layerStack) {
    if (mCurrentState.layerStack == layerStack) return false;
    mCurrentState.sequence++;
    mCurrentState.layerStack = layerStack;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

uint32_t Layer::getLayerStack() const {
    auto p = mDrawingParent.promote();
    if (p == nullptr) {
        return getDrawingState().layerStack;
    }
    return p->getLayerStack();
}

void Layer::deferTransactionUntil_legacy(const sp<Layer>& barrierLayer, uint64_t frameNumber) {
    mCurrentState.barrierLayer_legacy = barrierLayer;
    mCurrentState.frameNumber_legacy = frameNumber;
    // We don't set eTransactionNeeded, because just receiving a deferral
    // request without any other state updates shouldn't actually induce a delay
    mCurrentState.modified = true;
    pushPendingState();
    mCurrentState.barrierLayer_legacy = nullptr;
    mCurrentState.frameNumber_legacy = 0;
    mCurrentState.modified = false;
}

void Layer::deferTransactionUntil_legacy(const sp<IBinder>& barrierHandle, uint64_t frameNumber) {
    sp<Handle> handle = static_cast<Handle*>(barrierHandle.get());
    deferTransactionUntil_legacy(handle->owner.promote(), frameNumber);
}

// ----------------------------------------------------------------------------
// pageflip handling...
// ----------------------------------------------------------------------------

bool Layer::isHiddenByPolicy() const {
    const State& s(mDrawingState);
    const auto& parent = mDrawingParent.promote();
    if (parent != nullptr && parent->isHiddenByPolicy()) {
        return true;
    }
    return s.flags & layer_state_t::eLayerHidden;
}

uint32_t Layer::getEffectiveUsage(uint32_t usage) const {
    // TODO: should we do something special if mSecure is set?
    if (mProtectedByApp) {
        // need a hardware-protected path to external video sink
        usage |= GraphicBuffer::USAGE_PROTECTED;
    }
    if (mPotentialCursor) {
        usage |= GraphicBuffer::USAGE_CURSOR;
    }
    usage |= GraphicBuffer::USAGE_HW_COMPOSER;
    return usage;
}

void Layer::updateTransformHint(const sp<const DisplayDevice>& display) const {
    uint32_t orientation = 0;
    // Disable setting transform hint if the debug flag is set or if the
    // getTransformToDisplayInverse flag is set and the client wants to submit buffers
    // in one orientation.
    if (!mFlinger->mDebugDisableTransformHint && !getTransformToDisplayInverse()) {
        // The transform hint is used to improve performance, but we can
        // only have a single transform hint, it cannot
        // apply to all displays.
        const ui::Transform& planeTransform = display->getTransform();
        orientation = planeTransform.getOrientation();
        if (orientation & ui::Transform::ROT_INVALID) {
            orientation = 0;
        }
    }
    setTransformHint(orientation);
}

// ----------------------------------------------------------------------------
// debugging
// ----------------------------------------------------------------------------

// TODO(marissaw): add new layer state info to layer debugging
LayerDebugInfo Layer::getLayerDebugInfo() const {
    LayerDebugInfo info;
    const State& ds = getDrawingState();
    info.mName = getName();
    sp<Layer> parent = getParent();
    info.mParentName = (parent == nullptr ? std::string("none") : parent->getName().string());
    info.mType = std::string(getTypeId());
    info.mTransparentRegion = ds.activeTransparentRegion_legacy;
    info.mVisibleRegion = visibleRegion;
    info.mSurfaceDamageRegion = surfaceDamageRegion;
    info.mLayerStack = getLayerStack();
    info.mX = ds.active_legacy.transform.tx();
    info.mY = ds.active_legacy.transform.ty();
    info.mZ = ds.z;
    info.mWidth = ds.active_legacy.w;
    info.mHeight = ds.active_legacy.h;
    info.mCrop = ds.crop_legacy;
    info.mColor = ds.color;
    info.mFlags = ds.flags;
    info.mPixelFormat = getPixelFormat();
    info.mDataSpace = static_cast<android_dataspace>(mCurrentDataSpace);
    info.mMatrix[0][0] = ds.active_legacy.transform[0][0];
    info.mMatrix[0][1] = ds.active_legacy.transform[0][1];
    info.mMatrix[1][0] = ds.active_legacy.transform[1][0];
    info.mMatrix[1][1] = ds.active_legacy.transform[1][1];
    {
        sp<const GraphicBuffer> buffer = mActiveBuffer;
        if (buffer != 0) {
            info.mActiveBufferWidth = buffer->getWidth();
            info.mActiveBufferHeight = buffer->getHeight();
            info.mActiveBufferStride = buffer->getStride();
            info.mActiveBufferFormat = buffer->format;
        } else {
            info.mActiveBufferWidth = 0;
            info.mActiveBufferHeight = 0;
            info.mActiveBufferStride = 0;
            info.mActiveBufferFormat = 0;
        }
    }
    info.mNumQueuedFrames = getQueuedFrameCount();
    info.mRefreshPending = isBufferLatched();
    info.mIsOpaque = isOpaque(ds);
    info.mContentDirty = contentDirty;
    return info;
}

void Layer::miniDumpHeader(std::string& result) {
    result.append("-------------------------------");
    result.append("-------------------------------");
    result.append("-----------------------------\n");
    result.append(" Layer name\n");
    result.append("           Z | ");
    result.append(" Comp Type | ");
    result.append(" Transform | ");
    result.append("  Disp Frame (LTRB) | ");
    result.append("         Source Crop (LTRB)\n");
    result.append("-------------------------------");
    result.append("-------------------------------");
    result.append("-----------------------------\n");
}

void Layer::miniDump(std::string& result, DisplayId displayId) const {
    if (!hasHwcLayer(displayId)) {
        return;
    }

    std::string name;
    if (mName.length() > 77) {
        std::string shortened;
        shortened.append(mName.string(), 36);
        shortened.append("[...]");
        shortened.append(mName.string() + (mName.length() - 36), 36);
        name = shortened;
    } else {
        name = std::string(mName.string(), mName.size());
    }

    StringAppendF(&result, " %s\n", name.c_str());

    const State& layerState(getDrawingState());
    const LayerBE::HWCInfo& hwcInfo = getBE().mHwcLayers.at(displayId);
    if (layerState.zOrderRelativeOf != nullptr || mDrawingParent != nullptr) {
        StringAppendF(&result, "  rel %6d | ", layerState.z);
    } else {
        StringAppendF(&result, "  %10d | ", layerState.z);
    }
    StringAppendF(&result, "%10s | ", to_string(getCompositionType(displayId)).c_str());
    StringAppendF(&result, "%10s | ", to_string(hwcInfo.transform).c_str());
    const Rect& frame = hwcInfo.displayFrame;
    StringAppendF(&result, "%4d %4d %4d %4d | ", frame.left, frame.top, frame.right, frame.bottom);
    const FloatRect& crop = hwcInfo.sourceCrop;
    StringAppendF(&result, "%6.1f %6.1f %6.1f %6.1f\n", crop.left, crop.top, crop.right,
                  crop.bottom);

    result.append("- - - - - - - - - - - - - - - -\n");

    std::string compositionInfoStr;
    getBE().compositionInfo.dump(compositionInfoStr, "compositionInfo");
    result.append(compositionInfoStr);

    result.append("- - - - - - - - - - - - - - - -");
    result.append("- - - - - - - - - - - - - - - -");
    result.append("- - - - - - - - - - - - - - -\n");
}

void Layer::dumpFrameStats(std::string& result) const {
    mFrameTracker.dumpStats(result);
}

void Layer::clearFrameStats() {
    mFrameTracker.clearStats();
}

void Layer::logFrameStats() {
    mFrameTracker.logAndResetStats(mName);
}

void Layer::getFrameStats(FrameStats* outStats) const {
    mFrameTracker.getStats(outStats);
}

void Layer::dumpFrameEvents(std::string& result) {
    StringAppendF(&result, "- Layer %s (%s, %p)\n", getName().string(), getTypeId(), this);
    Mutex::Autolock lock(mFrameEventHistoryMutex);
    mFrameEventHistory.checkFencesForCompletion();
    mFrameEventHistory.dump(result);
}

void Layer::onDisconnect() {
    Mutex::Autolock lock(mFrameEventHistoryMutex);
    mFrameEventHistory.onDisconnect();
    mFlinger->mTimeStats->onDestroy(getSequence());
}

void Layer::addAndGetFrameTimestamps(const NewFrameEventsEntry* newTimestamps,
                                     FrameEventHistoryDelta* outDelta) {
    if (newTimestamps) {
        mFlinger->mTimeStats->setPostTime(getSequence(), newTimestamps->frameNumber,
                                          getName().c_str(), newTimestamps->postedTime);
    }

    Mutex::Autolock lock(mFrameEventHistoryMutex);
    if (newTimestamps) {
        // If there are any unsignaled fences in the aquire timeline at this
        // point, the previously queued frame hasn't been latched yet. Go ahead
        // and try to get the signal time here so the syscall is taken out of
        // the main thread's critical path.
        mAcquireTimeline.updateSignalTimes();
        // Push the new fence after updating since it's likely still pending.
        mAcquireTimeline.push(newTimestamps->acquireFence);
        mFrameEventHistory.addQueue(*newTimestamps);
    }

    if (outDelta) {
        mFrameEventHistory.getAndResetDelta(outDelta);
    }
}

size_t Layer::getChildrenCount() const {
    size_t count = 0;
    for (const sp<Layer>& child : mCurrentChildren) {
        count += 1 + child->getChildrenCount();
    }
    return count;
}

void Layer::addChild(const sp<Layer>& layer) {
    mChildrenChanged = true;

    mCurrentChildren.add(layer);
    layer->setParent(this);
}

ssize_t Layer::removeChild(const sp<Layer>& layer) {
    mChildrenChanged = true;

    layer->setParent(nullptr);
    return mCurrentChildren.remove(layer);
}

bool Layer::reparentChildren(const sp<IBinder>& newParentHandle) {
    sp<Handle> handle = nullptr;
    sp<Layer> newParent = nullptr;
    if (newParentHandle == nullptr) {
        return false;
    }
    handle = static_cast<Handle*>(newParentHandle.get());
    newParent = handle->owner.promote();
    if (newParent == nullptr) {
        ALOGE("Unable to promote Layer handle");
        return false;
    }

    if (attachChildren()) {
        setTransactionFlags(eTransactionNeeded);
    }
    for (const sp<Layer>& child : mCurrentChildren) {
        newParent->addChild(child);
    }
    mCurrentChildren.clear();

    return true;
}

void Layer::setChildrenDrawingParent(const sp<Layer>& newParent) {
    for (const sp<Layer>& child : mDrawingChildren) {
        child->mDrawingParent = newParent;
    }
}

bool Layer::reparent(const sp<IBinder>& newParentHandle) {
    bool callSetTransactionFlags = false;

    // While layers are detached, we allow most operations
    // and simply halt performing the actual transaction. However
    // for reparent != null we would enter the mRemovedFromCurrentState
    // state, regardless of whether doTransaction was called, and
    // so we need to prevent the update here.
    if (mLayerDetached && newParentHandle == nullptr) {
        return false;
    }

    sp<Layer> newParent;
    if (newParentHandle != nullptr) {
        auto handle = static_cast<Handle*>(newParentHandle.get());
        newParent = handle->owner.promote();
        if (newParent == nullptr) {
            ALOGE("Unable to promote Layer handle");
            return false;
        }
        if (newParent == this) {
            ALOGE("Invalid attempt to reparent Layer (%s) to itself", getName().c_str());
            return false;
        }
    }

    sp<Layer> parent = getParent();
    if (parent != nullptr) {
        parent->removeChild(this);
    }

    if (newParentHandle != nullptr) {
        newParent->addChild(this);
        if (!newParent->isRemovedFromCurrentState()) {
            addToCurrentState();
        } else {
            onRemovedFromCurrentState();
        }

        if (mLayerDetached) {
            mLayerDetached = false;
            callSetTransactionFlags = true;
        }
    } else {
        onRemovedFromCurrentState();
    }

    if (callSetTransactionFlags || attachChildren()) {
        setTransactionFlags(eTransactionNeeded);
    }
    return true;
}

bool Layer::detachChildren() {
    for (const sp<Layer>& child : mCurrentChildren) {
        sp<Client> parentClient = mClientRef.promote();
        sp<Client> client(child->mClientRef.promote());
        if (client != nullptr && parentClient != client) {
            child->mLayerDetached = true;
            child->detachChildren();
        }
    }

    return true;
}

bool Layer::attachChildren() {
    bool changed = false;
    for (const sp<Layer>& child : mCurrentChildren) {
        sp<Client> parentClient = mClientRef.promote();
        sp<Client> client(child->mClientRef.promote());
        if (client != nullptr && parentClient != client) {
            if (child->mLayerDetached) {
                child->mLayerDetached = false;
                changed = true;
            }
            changed |= child->attachChildren();
        }
    }

    return changed;
}

bool Layer::setColorTransform(const mat4& matrix) {
    static const mat4 identityMatrix = mat4();

    if (mCurrentState.colorTransform == matrix) {
        return false;
    }
    ++mCurrentState.sequence;
    mCurrentState.colorTransform = matrix;
    mCurrentState.hasColorTransform = matrix != identityMatrix;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

mat4 Layer::getColorTransform() const {
    mat4 colorTransform = mat4(getDrawingState().colorTransform);
    if (sp<Layer> parent = mDrawingParent.promote(); parent != nullptr) {
        colorTransform = parent->getColorTransform() * colorTransform;
    }
    return colorTransform;
}

bool Layer::hasColorTransform() const {
    bool hasColorTransform = getDrawingState().hasColorTransform;
    if (sp<Layer> parent = mDrawingParent.promote(); parent != nullptr) {
        hasColorTransform = hasColorTransform || parent->hasColorTransform();
    }
    return hasColorTransform;
}

bool Layer::isLegacyDataSpace() const {
    // return true when no higher bits are set
    return !(mCurrentDataSpace & (ui::Dataspace::STANDARD_MASK |
                ui::Dataspace::TRANSFER_MASK | ui::Dataspace::RANGE_MASK));
}

void Layer::setParent(const sp<Layer>& layer) {
    mCurrentParent = layer;
}

void Layer::clearSyncPoints() {
    for (const auto& child : mCurrentChildren) {
        child->clearSyncPoints();
    }

    Mutex::Autolock lock(mLocalSyncPointMutex);
    for (auto& point : mLocalSyncPoints) {
        point->setFrameAvailable();
    }
    mLocalSyncPoints.clear();
}

int32_t Layer::getZ() const {
    return mDrawingState.z;
}

bool Layer::usingRelativeZ(LayerVector::StateSet stateSet) {
    const bool useDrawing = stateSet == LayerVector::StateSet::Drawing;
    const State& state = useDrawing ? mDrawingState : mCurrentState;
    return state.zOrderRelativeOf != nullptr;
}

__attribute__((no_sanitize("unsigned-integer-overflow"))) LayerVector Layer::makeTraversalList(
        LayerVector::StateSet stateSet, bool* outSkipRelativeZUsers) {
    LOG_ALWAYS_FATAL_IF(stateSet == LayerVector::StateSet::Invalid,
                        "makeTraversalList received invalid stateSet");
    const bool useDrawing = stateSet == LayerVector::StateSet::Drawing;
    const LayerVector& children = useDrawing ? mDrawingChildren : mCurrentChildren;
    const State& state = useDrawing ? mDrawingState : mCurrentState;

    if (state.zOrderRelatives.size() == 0) {
        *outSkipRelativeZUsers = true;
        return children;
    }

    LayerVector traverse(stateSet);
    for (const wp<Layer>& weakRelative : state.zOrderRelatives) {
        sp<Layer> strongRelative = weakRelative.promote();
        if (strongRelative != nullptr) {
            traverse.add(strongRelative);
        }
    }

    for (const sp<Layer>& child : children) {
        const State& childState = useDrawing ? child->mDrawingState : child->mCurrentState;
        if (childState.zOrderRelativeOf != nullptr) {
            continue;
        }
        traverse.add(child);
    }

    return traverse;
}

/**
 * Negatively signed relatives are before 'this' in Z-order.
 */
void Layer::traverseInZOrder(LayerVector::StateSet stateSet, const LayerVector::Visitor& visitor) {
    // In the case we have other layers who are using a relative Z to us, makeTraversalList will
    // produce a new list for traversing, including our relatives, and not including our children
    // who are relatives of another surface. In the case that there are no relative Z,
    // makeTraversalList returns our children directly to avoid significant overhead.
    // However in this case we need to take the responsibility for filtering children which
    // are relatives of another surface here.
    bool skipRelativeZUsers = false;
    const LayerVector list = makeTraversalList(stateSet, &skipRelativeZUsers);

    size_t i = 0;
    for (; i < list.size(); i++) {
        const auto& relative = list[i];
        if (skipRelativeZUsers && relative->usingRelativeZ(stateSet)) {
            continue;
        }

        if (relative->getZ() >= 0) {
            break;
        }
        relative->traverseInZOrder(stateSet, visitor);
    }

    visitor(this);
    for (; i < list.size(); i++) {
        const auto& relative = list[i];

        if (skipRelativeZUsers && relative->usingRelativeZ(stateSet)) {
            continue;
        }
        relative->traverseInZOrder(stateSet, visitor);
    }
}

/**
 * Positively signed relatives are before 'this' in reverse Z-order.
 */
void Layer::traverseInReverseZOrder(LayerVector::StateSet stateSet,
                                    const LayerVector::Visitor& visitor) {
    // See traverseInZOrder for documentation.
    bool skipRelativeZUsers = false;
    LayerVector list = makeTraversalList(stateSet, &skipRelativeZUsers);

    int32_t i = 0;
    for (i = int32_t(list.size()) - 1; i >= 0; i--) {
        const auto& relative = list[i];

        if (skipRelativeZUsers && relative->usingRelativeZ(stateSet)) {
            continue;
        }

        if (relative->getZ() < 0) {
            break;
        }
        relative->traverseInReverseZOrder(stateSet, visitor);
    }
    visitor(this);
    for (; i >= 0; i--) {
        const auto& relative = list[i];

        if (skipRelativeZUsers && relative->usingRelativeZ(stateSet)) {
            continue;
        }

        relative->traverseInReverseZOrder(stateSet, visitor);
    }
}

LayerVector Layer::makeChildrenTraversalList(LayerVector::StateSet stateSet,
                                             const std::vector<Layer*>& layersInTree) {
    LOG_ALWAYS_FATAL_IF(stateSet == LayerVector::StateSet::Invalid,
                        "makeTraversalList received invalid stateSet");
    const bool useDrawing = stateSet == LayerVector::StateSet::Drawing;
    const LayerVector& children = useDrawing ? mDrawingChildren : mCurrentChildren;
    const State& state = useDrawing ? mDrawingState : mCurrentState;

    LayerVector traverse(stateSet);
    for (const wp<Layer>& weakRelative : state.zOrderRelatives) {
        sp<Layer> strongRelative = weakRelative.promote();
        // Only add relative layers that are also descendents of the top most parent of the tree.
        // If a relative layer is not a descendent, then it should be ignored.
        if (std::binary_search(layersInTree.begin(), layersInTree.end(), strongRelative.get())) {
            traverse.add(strongRelative);
        }
    }

    for (const sp<Layer>& child : children) {
        const State& childState = useDrawing ? child->mDrawingState : child->mCurrentState;
        // If a layer has a relativeOf layer, only ignore if the layer it's relative to is a
        // descendent of the top most parent of the tree. If it's not a descendent, then just add
        // the child here since it won't be added later as a relative.
        if (std::binary_search(layersInTree.begin(), layersInTree.end(),
                               childState.zOrderRelativeOf.promote().get())) {
            continue;
        }
        traverse.add(child);
    }

    return traverse;
}

void Layer::traverseChildrenInZOrderInner(const std::vector<Layer*>& layersInTree,
                                          LayerVector::StateSet stateSet,
                                          const LayerVector::Visitor& visitor) {
    const LayerVector list = makeChildrenTraversalList(stateSet, layersInTree);

    size_t i = 0;
    for (; i < list.size(); i++) {
        const auto& relative = list[i];
        if (relative->getZ() >= 0) {
            break;
        }
        relative->traverseChildrenInZOrderInner(layersInTree, stateSet, visitor);
    }

    visitor(this);
    for (; i < list.size(); i++) {
        const auto& relative = list[i];
        relative->traverseChildrenInZOrderInner(layersInTree, stateSet, visitor);
    }
}

std::vector<Layer*> Layer::getLayersInTree(LayerVector::StateSet stateSet) {
    const bool useDrawing = stateSet == LayerVector::StateSet::Drawing;
    const LayerVector& children = useDrawing ? mDrawingChildren : mCurrentChildren;

    std::vector<Layer*> layersInTree = {this};
    for (size_t i = 0; i < children.size(); i++) {
        const auto& child = children[i];
        std::vector<Layer*> childLayers = child->getLayersInTree(stateSet);
        layersInTree.insert(layersInTree.end(), childLayers.cbegin(), childLayers.cend());
    }

    return layersInTree;
}

void Layer::traverseChildrenInZOrder(LayerVector::StateSet stateSet,
                                     const LayerVector::Visitor& visitor) {
    std::vector<Layer*> layersInTree = getLayersInTree(stateSet);
    std::sort(layersInTree.begin(), layersInTree.end());
    traverseChildrenInZOrderInner(layersInTree, stateSet, visitor);
}

ui::Transform Layer::getTransform() const {
    ui::Transform t;
    const auto& p = mDrawingParent.promote();
    if (p != nullptr) {
        t = p->getTransform();

        // If the parent is not using NATIVE_WINDOW_SCALING_MODE_FREEZE (e.g.
        // it isFixedSize) then there may be additional scaling not accounted
        // for in the transform. We need to mirror this scaling in child surfaces
        // or we will break the contract where WM can treat child surfaces as
        // pixels in the parent surface.
        if (p->isFixedSize() && p->getBE().compositionInfo.mBuffer != nullptr) {
            int bufferWidth;
            int bufferHeight;
            if ((p->mCurrentTransform & NATIVE_WINDOW_TRANSFORM_ROT_90) == 0) {
                bufferWidth = p->getBE().compositionInfo.mBuffer->getWidth();
                bufferHeight = p->getBE().compositionInfo.mBuffer->getHeight();
            } else {
                bufferHeight = p->getBE().compositionInfo.mBuffer->getWidth();
                bufferWidth = p->getBE().compositionInfo.mBuffer->getHeight();
            }
            float sx = p->getActiveWidth(p->getDrawingState()) / static_cast<float>(bufferWidth);
            float sy = p->getActiveHeight(p->getDrawingState()) / static_cast<float>(bufferHeight);
            ui::Transform extraParentScaling;
            extraParentScaling.set(sx, 0, 0, sy);
            t = t * extraParentScaling;
        }
    }
    return t * getActiveTransform(getDrawingState());
}

half Layer::getAlpha() const {
    const auto& p = mDrawingParent.promote();

    half parentAlpha = (p != nullptr) ? p->getAlpha() : 1.0_hf;
    return parentAlpha * getDrawingState().color.a;
}

half4 Layer::getColor() const {
    const half4 color(getDrawingState().color);
    return half4(color.r, color.g, color.b, getAlpha());
}

Layer::RoundedCornerState Layer::getRoundedCornerState() const {
    const auto& p = mDrawingParent.promote();
    if (p != nullptr) {
        RoundedCornerState parentState = p->getRoundedCornerState();
        if (parentState.radius > 0) {
            ui::Transform t = getActiveTransform(getDrawingState());
            t = t.inverse();
            parentState.cropRect = t.transform(parentState.cropRect);
            // The rounded corners shader only accepts 1 corner radius for performance reasons,
            // but a transform matrix can define horizontal and vertical scales.
            // Let's take the average between both of them and pass into the shader, practically we
            // never do this type of transformation on windows anyway.
            parentState.radius *= (t[0][0] + t[1][1]) / 2.0f;
            return parentState;
        }
    }
    const float radius = getDrawingState().cornerRadius;
    return radius > 0 ? RoundedCornerState(computeBounds(), radius) : RoundedCornerState();
}

void Layer::commitChildList() {
    for (size_t i = 0; i < mCurrentChildren.size(); i++) {
        const auto& child = mCurrentChildren[i];
        child->commitChildList();
    }
    mDrawingChildren = mCurrentChildren;
    mDrawingParent = mCurrentParent;
}

void Layer::setInputInfo(const InputWindowInfo& info) {
    mCurrentState.inputInfo = info;
    mCurrentState.modified = true;
    mCurrentState.inputInfoChanged = true;
    setTransactionFlags(eTransactionNeeded);
}

void Layer::writeToProto(LayerProto* layerInfo, LayerVector::StateSet stateSet) {
    const bool useDrawing = stateSet == LayerVector::StateSet::Drawing;
    const LayerVector& children = useDrawing ? mDrawingChildren : mCurrentChildren;
    const State& state = useDrawing ? mDrawingState : mCurrentState;

    ui::Transform requestedTransform = state.active_legacy.transform;
    ui::Transform transform = getTransform();

    layerInfo->set_id(sequence);
    layerInfo->set_name(getName().c_str());
    layerInfo->set_type(String8(getTypeId()));

    for (const auto& child : children) {
        layerInfo->add_children(child->sequence);
    }

    for (const wp<Layer>& weakRelative : state.zOrderRelatives) {
        sp<Layer> strongRelative = weakRelative.promote();
        if (strongRelative != nullptr) {
            layerInfo->add_relatives(strongRelative->sequence);
        }
    }

    LayerProtoHelper::writeToProto(state.activeTransparentRegion_legacy,
                                   layerInfo->mutable_transparent_region());
    LayerProtoHelper::writeToProto(visibleRegion, layerInfo->mutable_visible_region());
    LayerProtoHelper::writeToProto(surfaceDamageRegion, layerInfo->mutable_damage_region());

    layerInfo->set_layer_stack(getLayerStack());
    layerInfo->set_z(state.z);

    PositionProto* position = layerInfo->mutable_position();
    position->set_x(transform.tx());
    position->set_y(transform.ty());

    PositionProto* requestedPosition = layerInfo->mutable_requested_position();
    requestedPosition->set_x(requestedTransform.tx());
    requestedPosition->set_y(requestedTransform.ty());

    SizeProto* size = layerInfo->mutable_size();
    size->set_w(state.active_legacy.w);
    size->set_h(state.active_legacy.h);

    LayerProtoHelper::writeToProto(state.crop_legacy, layerInfo->mutable_crop());
    layerInfo->set_corner_radius(getRoundedCornerState().radius);

    layerInfo->set_is_opaque(isOpaque(state));
    layerInfo->set_invalidate(contentDirty);

    // XXX (b/79210409) mCurrentDataSpace is not protected
    layerInfo->set_dataspace(dataspaceDetails(static_cast<android_dataspace>(mCurrentDataSpace)));

    layerInfo->set_pixel_format(decodePixelFormat(getPixelFormat()));
    LayerProtoHelper::writeToProto(getColor(), layerInfo->mutable_color());
    LayerProtoHelper::writeToProto(state.color, layerInfo->mutable_requested_color());
    layerInfo->set_flags(state.flags);

    LayerProtoHelper::writeToProto(transform, layerInfo->mutable_transform());
    LayerProtoHelper::writeToProto(requestedTransform, layerInfo->mutable_requested_transform());

    auto parent = useDrawing ? mDrawingParent.promote() : mCurrentParent.promote();
    if (parent != nullptr) {
        layerInfo->set_parent(parent->sequence);
    }

    auto zOrderRelativeOf = state.zOrderRelativeOf.promote();
    if (zOrderRelativeOf != nullptr) {
        layerInfo->set_z_order_relative_of(zOrderRelativeOf->sequence);
    }

    // XXX getBE().compositionInfo.mBuffer is not protected
    auto buffer = getBE().compositionInfo.mBuffer;
    if (buffer != nullptr) {
        LayerProtoHelper::writeToProto(buffer, layerInfo->mutable_active_buffer());
        LayerProtoHelper::writeToProto(ui::Transform(mCurrentTransform),
                                       layerInfo->mutable_buffer_transform());
    }

    layerInfo->set_queued_frames(getQueuedFrameCount());
    layerInfo->set_refresh_pending(isBufferLatched());
    layerInfo->set_curr_frame(mCurrentFrameNumber);
    layerInfo->set_effective_scaling_mode(getEffectiveScalingMode());

    for (const auto& pendingState : mPendingStates) {
        auto barrierLayer = pendingState.barrierLayer_legacy.promote();
        if (barrierLayer != nullptr) {
            BarrierLayerProto* barrierLayerProto = layerInfo->add_barrier_layer();
            barrierLayerProto->set_id(barrierLayer->sequence);
            barrierLayerProto->set_frame_number(pendingState.frameNumber_legacy);
        }
    }

    auto protoMap = layerInfo->mutable_metadata();
    for (const auto& entry : state.metadata.mMap) {
        (*protoMap)[entry.first] = std::string(entry.second.cbegin(), entry.second.cend());
    }
}

void Layer::writeToProto(LayerProto* layerInfo, DisplayId displayId) {
    if (!hasHwcLayer(displayId)) {
        return;
    }

    writeToProto(layerInfo, LayerVector::StateSet::Drawing);

    const auto& hwcInfo = getBE().mHwcLayers.at(displayId);

    const Rect& frame = hwcInfo.displayFrame;
    LayerProtoHelper::writeToProto(frame, layerInfo->mutable_hwc_frame());

    const FloatRect& crop = hwcInfo.sourceCrop;
    LayerProtoHelper::writeToProto(crop, layerInfo->mutable_hwc_crop());

    const int32_t transform = static_cast<int32_t>(hwcInfo.transform);
    layerInfo->set_hwc_transform(transform);

    const int32_t compositionType = static_cast<int32_t>(hwcInfo.compositionType);
    layerInfo->set_hwc_composition_type(compositionType);

    if (std::strcmp(getTypeId(), "BufferLayer") == 0 &&
        static_cast<BufferLayer*>(this)->isProtected()) {
        layerInfo->set_is_protected(true);
    } else {
        layerInfo->set_is_protected(false);
    }
}

bool Layer::isRemovedFromCurrentState() const  {
    return mRemovedFromCurrentState;
}

InputWindowInfo Layer::fillInputInfo() {
    InputWindowInfo info = mDrawingState.inputInfo;

    if (info.displayId == ADISPLAY_ID_NONE) {
        info.displayId = mDrawingState.layerStack;
    }

    ui::Transform t = getTransform();
    const float xScale = t.sx();
    const float yScale = t.sy();
    if (xScale != 1.0f || yScale != 1.0f) {
        info.windowXScale *= 1.0f / xScale;
        info.windowYScale *= 1.0f / yScale;
        info.touchableRegion.scaleSelf(xScale, yScale);
    }

    // Transform layer size to screen space and inset it by surface insets.
    // If this is a portal window, set the touchableRegion to the layerBounds.
    Rect layerBounds = info.portalToDisplayId == ADISPLAY_ID_NONE
            ? getBufferSize(getDrawingState())
            : info.touchableRegion.getBounds();
    if (!layerBounds.isValid()) {
        layerBounds = getCroppedBufferSize(getDrawingState());
    }
    layerBounds = t.transform(layerBounds);
    layerBounds.inset(info.surfaceInset, info.surfaceInset, info.surfaceInset, info.surfaceInset);

    // Input coordinate should match the layer bounds.
    info.frameLeft = layerBounds.left;
    info.frameTop = layerBounds.top;
    info.frameRight = layerBounds.right;
    info.frameBottom = layerBounds.bottom;

    // Position the touchable region relative to frame screen location and restrict it to frame
    // bounds.
    info.touchableRegion = info.touchableRegion.translate(info.frameLeft, info.frameTop);
    info.visible = isVisible();
    return info;
}

bool Layer::hasInput() const {
    return mDrawingState.inputInfo.token != nullptr;
}

std::shared_ptr<compositionengine::Layer> Layer::getCompositionLayer() const {
    return nullptr;
}

// ---------------------------------------------------------------------------

}; // namespace android

#if defined(__gl_h_)
#error "don't include gl/gl.h in this file"
#endif

#if defined(__gl2_h_)
#error "don't include gl2/gl2.h in this file"
#endif
