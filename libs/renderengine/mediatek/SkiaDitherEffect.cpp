/*
 * Copyright 2026 The LineageOS Project
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

#include "SkiaDitherEffect.h"
#include <SkCanvas.h>
#include <SkTileMode.h>
#include <SkData.h>
#include <SkPaint.h>
#include <SkRRect.h>
#include <SkRuntimeEffect.h>
#include <SkSize.h>
#include <SkString.h>
#include <SkSurface.h>
#include <log/log.h>
#include <utils/Trace.h>

using namespace android;
using android::hardware::graphics::common::V1_1::BufferUsage;

namespace android {
namespace renderengine {
namespace skia {

bool SkiaDitherEffect::isInitOK() {
    return mInitOK;
}

SkiaDitherEffect::~SkiaDitherEffect(){
}

SkiaDitherEffect::SkiaDitherEffect() {
    bool initOK = true;
    static unsigned char ditherTable[8*8] = {0};

    // following dither table is from Skia dither
    for (int x = 0; x < 8; ++x) {
        for (int y = 0; y < 8; ++y) {
            unsigned int m = (y & 1) << 5 | (x & 1) << 4 |
                             (y & 2) << 2 | (x & 2) << 1 |
                             (y & 4) >> 1 | (x & 4) >> 2;
            float value = float(m) * 1.0 / 64.0 - 63.0 / 128.0;
            ditherTable[y * 8 + x] = (uint8_t)((value + 0.5) * 255.f + 0.5f);
        }
    }
    mDitherBmp.setInfo(SkImageInfo::MakeA8(8, 8));
    mDitherBmp.setPixels(const_cast<uint8_t*>(ditherTable));
    mDitherBmp.setImmutable();

    mInitOK = initOK;
}


sk_sp<SkShader> SkiaDitherEffect::makeDitherTableShader(SkiaGpuContext* context){
    static SkiaGpuContext* current_context = nullptr;
    if (current_context != context) {
        mDitherTableShader = mDitherBmp.makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat,
                    SkSamplingOptions({SkFilterMode::kNearest, SkMipmapMode::kNone}));
        current_context = context;
    }
    return mDitherTableShader;
}

sk_sp<SkShader> SkiaDitherEffect::createDitherShader(SkiaGpuContext* context, sk_sp<SkShader> input, const float ditherAlpha) {
    sk_sp<SkShader> tableShader = makeDitherTableShader(context);
    if (mDitherTableShader == nullptr){
        ALOGE("%s(), makeDitherTableShader fail", __FUNCTION__);
    }
    SkString ditherString(R"(
        uniform shader DataInput;
        uniform shader table;
        uniform float ditherAlpha;
        half4 main(float2 xy) {
            float4 c = float4(DataInput.eval(xy));
            float dither = ((table.eval(xy).a) - 0.5)/256.0;
            return float4(clamp(c.rgb*ditherAlpha+dither,0.0,1.0), c.a);
        }

    )");
    auto [ditherEffect, error] = SkRuntimeEffect::MakeForShader(ditherString);
    if (!ditherEffect) {
       ALOGE("%s(), RuntimeShader error: %s", __FUNCTION__, error.c_str());
       return nullptr;
    }
    mDitherEffect = std::move(ditherEffect);
    SkRuntimeShaderBuilder ditherBuilder(mDitherEffect);
    ditherBuilder.child("DataInput") =  input;
    ditherBuilder.child("table") = tableShader;
    ditherBuilder.uniform("ditherAlpha") = ditherAlpha;
    return ditherBuilder.makeShader(nullptr);
}

} // namespace skia
} // namespace renderengine
} // namespace android
