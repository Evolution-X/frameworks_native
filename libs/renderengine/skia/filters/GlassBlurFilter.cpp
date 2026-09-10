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

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "GlassBlurFilter.h"
#include <SkAlphaType.h>
#include <SkBlendMode.h>
#include <SkCanvas.h>
#include <SkData.h>
#include <SkPaint.h>
#include <SkRRect.h>
#include <SkRuntimeEffect.h>
#include <SkShader.h>
#include <SkSize.h>
#include <SkString.h>
#include <SkSurface.h>
#include <SkTileMode.h>
#include <SkSamplingOptions.h>
#include <android-base/properties.h>
#include <common/ThreadStateCrashLogger.h>
#include <common/trace.h>
#include <include/gpu/GpuTypes.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <log/log.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

#include "RuntimeEffectManager.h"

namespace android {
namespace renderengine {
namespace skia {
namespace {

constexpr const char* kGlassInputScaleProperty = "persist.sys.sf.gb_scale";

float readGlassInputScale() {
    const std::string value = base::GetProperty(kGlassInputScaleProperty, "");
    if (value.empty()) {
        return BlurFilter::kInputScale;
    }
    char* end = nullptr;
    const float scale = std::strtof(value.c_str(), &end);
    if (end == value.c_str() || !std::isfinite(scale)) {
        return BlurFilter::kInputScale;
    }
    return std::clamp(scale, 0.10f, 0.25f);
}

} // namespace

const SkString kEffectSource_GlassBlurFilter_UpSampleEffect(R"(
    uniform shader child;
    uniform float in_blurOffset;
    uniform float in_crossFade;
    uniform float in_weightedCrossFade;

    const float2 STEP_0 = float2( 1.0, 0.0);
    const float2 STEP_1 = float2( 0.623489802,  0.781831482);
    const float2 STEP_2 = float2(-0.222520934,  0.974927912);
    const float2 STEP_3 = float2(-0.900968868,  0.433883739);
    const float2 STEP_4 = float2(-0.900968868, -0.433883739);
    const float2 STEP_5 = float2(-0.222520934, -0.974927912);
    const float2 STEP_6 = float2( 0.623489802, -0.781831482);

    half4 main(float2 xy) {
        half3 c = child.eval(xy).rgb;
        c += child.eval(xy + STEP_0 * in_blurOffset).rgb;
        c += child.eval(xy + STEP_1 * in_blurOffset).rgb;
        c += child.eval(xy + STEP_2 * in_blurOffset).rgb;
        c += child.eval(xy + STEP_3 * in_blurOffset).rgb;
        c += child.eval(xy + STEP_4 * in_blurOffset).rgb;
        c += child.eval(xy + STEP_5 * in_blurOffset).rgb;
        c += child.eval(xy + STEP_6 * in_blurOffset).rgb;
        return half4(c * in_weightedCrossFade, in_crossFade);
    }
)");

const SkString kEffectSource_GlassBlurFilter_FinalUpSampleEffect(R"(
    uniform shader child;
    uniform float in_blurOffset;
    uniform float in_crossFade;
    uniform float in_weightedCrossFade;

    const float2 STEP_0 = float2( 0.900968868,  0.433883739);
    const float2 STEP_1 = float2( 0.222520934,  0.974927912);
    const float2 STEP_2 = float2(-0.623489802,  0.781831482);
    const float2 STEP_3 = float2(-1.0, 0.0);
    const float2 STEP_4 = float2(-0.623489802, -0.781831482);
    const float2 STEP_5 = float2( 0.222520934, -0.974927912);
    const float2 STEP_6 = float2( 0.900968868, -0.433883739);

    half4 main(float2 xy) {
        half3 c = child.eval(xy).rgb;
        c += child.eval(xy + STEP_0 * in_blurOffset).rgb;
        c += child.eval(xy + STEP_1 * in_blurOffset).rgb;
        c += child.eval(xy + STEP_2 * in_blurOffset).rgb;
        c += child.eval(xy + STEP_3 * in_blurOffset).rgb;
        c += child.eval(xy + STEP_4 * in_blurOffset).rgb;
        c += child.eval(xy + STEP_5 * in_blurOffset).rgb;
        c += child.eval(xy + STEP_6 * in_blurOffset).rgb;
        return half4(c * in_weightedCrossFade, in_crossFade);
    }
)");

GlassBlurFilter::GlassBlurFilter(RuntimeEffectManager& effectManager)
      : BlurFilter(effectManager, 0.0f, readGlassInputScale()) {
    mQuarterResDownSampleBlurEffect =
            effectManager.mKnownEffects[kKawaseBlurDualFilterV2_QuarterResDownSampleBlurEffect];
    mHalfResDownSampleBlurEffect =
            effectManager.mKnownEffects[kKawaseBlurDualFilterV2_HalfResDownSampleBlurEffect];
    mUpSampleBlurEffect = effectManager.mKnownEffects[kGlassBlurFilter_UpSampleEffect];
    mRotatedUpSampleBlurEffect = effectManager.mKnownEffects[kGlassBlurFilter_FinalUpSampleEffect];
}

uint32_t GlassBlurFilter::effectiveRadius(uint32_t radius) const {
    if (radius < 8) {
        return radius;
    }
    return (radius + 3u) & ~3u;
}

sk_sp<SkSurface> GlassBlurFilter::obtainSurface(SkiaGpuContext* context, const SkImageInfo& info,
                                                 int index) const {
    // First-fit: check every slot in this index's ring for a reusable surface
    // before creating one, instead of only checking whatever slot the
    // unconditional round-robin counter happens to point at. Fixes stutter
    // where a stable blurRect size still has to "wait out" stale entries left
    // by earlier passes with different sizes (e.g. during a launch anim) before
    // the ring happens to land on it again.
    auto& ring = mSurfaces[index];
    for (int i = 0; i < kSurfaceRingSize; i++) {
        SurfaceSlot& slot = ring[i];
        if (slot.surface && slot.context == context && slot.info == info) {
            return slot.surface;
        }
    }

    // Miss: evict via round-robin. mNextSurface[index] now advances only on
    // miss (was: every call), so it tracks "next slot to overwrite" rather
    // than "next slot to try".
    const int target = mNextSurface[index];
    mNextSurface[index] = (target + 1) % kSurfaceRingSize;
    SurfaceSlot& slot = ring[target];

    SFTRACE_NAME("GlassBlurSurfaceCreate");
    sk_sp<SkSurface> surface = context->createRenderTarget(info);
    LOG_THREAD_STATE_AND_CRASH_IF(!surface, "%s: Failed to create surface for blurring!", __func__);
    slot.context = context;
    slot.info = info;
    slot.surface = surface;
    return surface;
}

void GlassBlurFilter::blurInto(const sk_sp<SkSurface>& drawSurface,
                                const sk_sp<SkImage>& readImage, const float radius,
                                const float alpha,
                                const sk_sp<SkRuntimeEffect>& blurEffect) const {
    SkMatrix blurMatrix =
            SkMatrix::Scale(static_cast<float>(drawSurface->width()) / readImage->width(),
                            static_cast<float>(drawSurface->height()) / readImage->height());
    blurInto(drawSurface,
             readImage->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                   SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
                                   blurMatrix),
             radius, alpha, blurEffect);
}

void GlassBlurFilter::blurInto(const sk_sp<SkSurface>& drawSurface, sk_sp<SkShader> input,
                               const float radius, const float alpha,
                               const sk_sp<SkRuntimeEffect>& blurEffect) const {
    SkPaint paint;
    const bool isUpsample =
            blurEffect == mUpSampleBlurEffect || blurEffect == mRotatedUpSampleBlurEffect;
    if (isUpsample) {
        if (radius == 0) {
            paint.setShader(std::move(input));
            paint.setAlphaf(alpha);
        } else {
            SkRuntimeShaderBuilder blurBuilder(blurEffect);
            blurBuilder.child("child") = std::move(input);
            blurBuilder.uniform("in_crossFade") = alpha;
            blurBuilder.uniform("in_weightedCrossFade") = alpha * 0.125f;
            blurBuilder.uniform("in_blurOffset") = radius;
            paint.setShader(blurBuilder.makeShader(nullptr));
        }
    } else {
        SkRuntimeShaderBuilder blurBuilder(blurEffect);
        blurBuilder.child("child") = std::move(input);
        paint.setShader(blurBuilder.makeShader(nullptr));
    }
    paint.setBlendMode(alpha == 1.0f ? SkBlendMode::kSrc : SkBlendMode::kSrcOver);
    drawSurface->getCanvas()->drawPaint(paint);
}

sk_sp<SkImage> GlassBlurFilter::generateTemporaryImage(SkiaGpuContext* context,
                                                        const DisplaySettings& display,
                                                        const uint32_t blurRadius,
                                                        const sk_sp<SkImage> input,
                                                        const SkRect& blurRect) const {
    const float radius = blurRadius * 0.57735f;
    const float scale = inputScale();
    const float inverseScale = inverseInputScale();

    const float scaledRadius = radius * scale;
    const float baseDepth = std::min(2.0f, scaledRadius / 3.0f);
    constexpr float kTargetBlurOffset = 7.0f;
    constexpr float kRadiusVarianceRatio = 0.5f;
    const float requiredStepWeight = scaledRadius * scaledRadius /
            (kTargetBlurOffset * kTargetBlurOffset + kRadiusVarianceRatio);
    float requiredDepth = 0.0f;
    float accumulatedStepWeight = 0.0f;
    float passStepWeight = 1.0f;
    while (requiredDepth < kMaxSurfaces - 1 &&
           accumulatedStepWeight + passStepWeight < requiredStepWeight) {
        accumulatedStepWeight += passStepWeight;
        passStepWeight *= 4.0f;
        requiredDepth += 1.0f;
    }
    if (requiredDepth < kMaxSurfaces - 1 && requiredStepWeight > accumulatedStepWeight) {
        requiredDepth += sqrtf((requiredStepWeight - accumulatedStepWeight) / passStepWeight);
    }
    const float filterDepth = std::min(kMaxSurfaces - 1.0f, std::max(baseDepth, requiredDepth));
    const int filterPasses = std::min(kMaxSurfaces - 1, static_cast<int>(ceil(filterDepth)));

    SkIRect targetBlurRect;
    blurRect.roundOut(&targetBlurRect);

    auto makeSurface = [&](float scale, int index) -> sk_sp<SkSurface> {
        const int newW = std::max(1,
                                  static_cast<int>(ceilf(
                                          static_cast<float>(targetBlurRect.width()) / scale)));
        const int newH = std::max(1,
                                  static_cast<int>(ceilf(
                                          static_cast<float>(targetBlurRect.height()) / scale)));
        return obtainSurface(context, input->imageInfo().makeWH(newW, newH), index);
    };

    std::array<sk_sp<SkSurface>, kMaxSurfaces> surfaces = {};
    for (int i = 0; i <= filterPasses; i++) {
        surfaces[i] = makeSurface(static_cast<float>(1 << i) * inverseScale, i);
    }

    float sumSquaredR = 0;
    float sumSquaredStep = 0;
    for (int i = 0; i < filterPasses; i++) {
        const float alpha = std::min(1.0f, filterDepth - i);
        const float passScale = static_cast<float>(1 << i);
        const float radiusContribution = passScale * 0.5f * alpha;
        const float stepContribution = passScale * alpha;
        sumSquaredR += radiusContribution * radiusContribution * 2.0f;
        sumSquaredStep += stepContribution * stepContribution;
    }
    const float step = sqrtf(std::max(0.0f, scaledRadius * scaledRadius - sumSquaredR) /
                             (sumSquaredStep == 0 ? 1.0f : sumSquaredStep));

    {
        SkMatrix blurMatrix = SkMatrix::Translate(-blurRect.fLeft, -blurRect.fTop);
        blurMatrix.postScale(static_cast<float>(surfaces[0]->width()) / blurRect.width(),
                             static_cast<float>(surfaces[0]->height()) / blurRect.height());
        const auto sourceShader =
                input->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                  SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
                                  blurMatrix);
        blurInto(surfaces[0], std::move(sourceShader), 0, 1.0f,
                 mQuarterResDownSampleBlurEffect);
    }

    for (int i = 0; i < filterPasses; i++) {
        blurInto(surfaces[i + 1], surfaces[i]->makeTemporaryImage(), 0, 1.0f,
                 mHalfResDownSampleBlurEffect);
    }

    for (int i = filterPasses - 1; i >= 0; i--) {
        const sk_sp<SkRuntimeEffect>& upEffect =
                (i % 2 == 0) ? mRotatedUpSampleBlurEffect : mUpSampleBlurEffect;
        blurInto(surfaces[i], surfaces[i + 1]->makeTemporaryImage(), step,
                 std::min(1.0f, filterDepth - i), upEffect);
    }

    // Matches KawaseBlurDualFilterV2's contract: makeTemporaryImage(), not makeImageSnapshot().
    // Valid only until the next call to generateTemporaryImage on this filter instance.
    return surfaces[0]->makeTemporaryImage();
}

} // namespace skia
} // namespace renderengine
} // namespace android
