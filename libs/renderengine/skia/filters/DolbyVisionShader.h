/*
 * Copyright 2026 The Penguin Project
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

#include <SkRuntimeEffect.h>
#include <SkShader.h>
#include <ftl/flags.h>
#include <renderengine/ColorSpaces.h>
#include <renderengine/DisplaySettings.h>
#include <renderengine/LayerSettings.h>
#include <ui/GraphicTypes.h>
#include <ui/GraphicBuffer.h>

#include <mutex>
#include <string>

namespace android {
namespace renderengine {
namespace skia {

// Reshapes Dolby Vision video layers during client composition.
//
// The display hardware reshapes Dolby Vision frames when a layer goes to device composition.
// When RenderEngine composes such a layer (PiP, shade, animations), the raw frame (for
// profile 5 an IPTPQc2 signal) would be drawn as plain YUV and show up green. This class
// drives the vendor Dolby Vision effect library (libdolbyeffect_5.0, loaded at runtime):
// it feeds the frame's RPU metadata, takes the SkSL program, LUT and uniforms the library
// produces, and builds the shader that replaces the linear (tone mapping) effect.
//
// Everything is optional: when the libraries are missing, the layer has no Dolby Vision
// metadata or anything fails, create() returns nullptr and the caller keeps its normal path.
// debug.renderengine.dolby_vision=0 disables it; .log=1 logs per frame; .inverse_matrix and
// .working_cs (both default 1) switch the two color-handling choices for testing.
class DolbyVisionShader {
public:
    static DolbyVisionShader& getInstance();

    // Returns the reshaped shader for a Dolby Vision layer, or nullptr if the layer is not
    // Dolby Vision or the effect is unavailable.
    sk_sp<SkShader> create(const sk_sp<SkShader>& input, const LayerSettings& layer,
                           const DisplaySettings& display, const sp<GraphicBuffer>& buffer,
                           ui::Dataspace outputDataspace, float dimmingRatio, bool vulkan,
                           ftl::Flags<ColorSpaceOptions> colorSpaceOptions);

private:
    DolbyVisionShader() = default;

    struct Api;
    bool loadMapper();
    bool loadEffect(bool vulkan);
    bool readRpu(const sp<GraphicBuffer>& buffer, std::vector<uint8_t>* storage,
                 const uint8_t** rpu, int* rpuLength, uint32_t* layerId, uint64_t* timestamp);
    sk_sp<SkRuntimeEffect> runtimeEffect();

    std::mutex mMutex;
    bool mMapperLoadAttempted = false;
    bool mEffectLoadAttempted = false;
    bool mDisabled = false;
    bool mVerbose = false;
    bool mUseWorkingColorSpace = true;
    Api* mApi = nullptr;
    std::string mShaderSource;
    sk_sp<SkRuntimeEffect> mRuntimeEffect;
};

} // namespace skia
} // namespace renderengine
} // namespace android
