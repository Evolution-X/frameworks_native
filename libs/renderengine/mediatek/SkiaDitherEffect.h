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

#ifndef __SKIA_DITHER_EFFECT__H__
#define __SKIA_DITHER_EFFECT__H__

#include <SkCanvas.h>
#include <SkBitmap.h>
#include <SkImage.h>
#include <SkRuntimeEffect.h>
#include <SkSurface.h>
#include <GrAHardwareBufferUtils.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <sys/types.h>
#include <ui/GraphicBuffer.h>
#include "../skia/compat/SkiaGpuContext.h"

using namespace std;

namespace android {
namespace renderengine {
namespace skia {

class SkiaDitherEffect {
public:
    explicit SkiaDitherEffect();
    virtual ~SkiaDitherEffect();
    bool isInitOK();
    sk_sp<SkShader> createDitherShader(SkiaGpuContext* context, sk_sp<SkShader> input, const float ditherAlpha);

private:
    bool mInitOK = false;
    sk_sp<SkShader> makeDitherTableShader(SkiaGpuContext* context);
    sk_sp<SkRuntimeEffect> mDitherEffect = nullptr;
    SkBitmap mDitherBmp;
    sk_sp<SkShader> mDitherTableShader = nullptr;
};

} // namespace skia
} // namespace renderengine
} // namespace android

#endif
