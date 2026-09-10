/*
 * Copyright 2025 The Android Open Source Project
 * Copyright 2025-2026 AxionOS
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

#include <SkCanvas.h>
#include <SkImage.h>
#include <SkRuntimeEffect.h>
#include <SkSurface.h>

#include <array>

#include "BlurFilter.h"

#include "RuntimeEffectManager.h"

namespace android {
namespace renderengine {
namespace skia {

class GlassBlurFilter : public BlurFilter {
public:
    explicit GlassBlurFilter(RuntimeEffectManager& effectManager);
    virtual ~GlassBlurFilter() {}

    // Execute blur, saving it to a TEMPORARY texture. WARNING: the returned SkImage is only
    // guaranteed to be valid until the next call to generateTemporaryImage on this BlurFilter.
    // This limitation is to avoid unnecessary allocations. (Matches BlurFilter's base contract;
    // see BlurFilter.h.)
    sk_sp<SkImage> generateTemporaryImage(SkiaGpuContext* context, const DisplaySettings& display,
                                          const uint32_t radius, const sk_sp<SkImage> blurInput,
                                          const SkRect& blurRect) const override;
    uint32_t effectiveRadius(uint32_t radius) const override;

private:
    sk_sp<SkRuntimeEffect> mQuarterResDownSampleBlurEffect;
    sk_sp<SkRuntimeEffect> mHalfResDownSampleBlurEffect;
    sk_sp<SkRuntimeEffect> mUpSampleBlurEffect;
    sk_sp<SkRuntimeEffect> mRotatedUpSampleBlurEffect;

    static constexpr int kMaxSurfaces = 6;
    static constexpr int kSurfaceRingSize = 5;

    struct SurfaceSlot {
        SkImageInfo info;
        SkiaGpuContext* context = nullptr;
        sk_sp<SkSurface> surface;
    };

    // Mutable: generateTemporaryImage() is logically const (matches the base class contract used
    // by KawaseBlurDualFilterV2), but internally cycles through a small ring of reusable surfaces
    // per resolution level to avoid reallocating every call.
    mutable std::array<std::array<SurfaceSlot, kSurfaceRingSize>, kMaxSurfaces> mSurfaces;
    mutable std::array<int, kMaxSurfaces> mNextSurface = {};

    sk_sp<SkSurface> obtainSurface(SkiaGpuContext* context, const SkImageInfo& info,
                                   int index) const;

    void blurInto(const sk_sp<SkSurface>& drawSurface, const sk_sp<SkImage>& readImage,
                  const float radius, const float alpha, const sk_sp<SkRuntimeEffect>&) const;

    void blurInto(const sk_sp<SkSurface>& drawSurface, sk_sp<SkShader> input, const float radius,
                  const float alpha, const sk_sp<SkRuntimeEffect>&) const;
};

} // namespace skia
} // namespace renderengine
} // namespace android
