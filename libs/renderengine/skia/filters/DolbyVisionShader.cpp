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

#undef LOG_TAG
#define LOG_TAG "RenderEngine"

#include "DolbyVisionShader.h"

#include <SkBitmap.h>
#include <SkData.h>
#include <SkImage.h>
#include <SkImageInfo.h>
#include <SkMatrix.h>
#include <SkSamplingOptions.h>
#include <SkSpan.h>
#include <common/trace.h>
#include <cutils/properties.h>
#include <dlfcn.h>
#include <log/log.h>
#include <renderengine/ColorSpaces.h>

#include <cinttypes>
#include <cstring>
#include <vector>

namespace android {
namespace renderengine {
namespace skia {

namespace {

// Color description passed to the effect library. Codes follow ITU-T H.273.
struct DvColor {
    int32_t primaries = 1;
    int32_t range = 255;
    int32_t transfer = 1;
    int32_t matrix = 0;
    float maxLuminance = 0.f;
    int32_t reserved[3] = {};
};
static_assert(sizeof(DvColor) == 32);

struct DvUniform {
    std::string name;
    std::vector<uint8_t> data;
};
static_assert(sizeof(DvUniform) == 0x30);

// Virtual interface of the effect object returned by liboplusdolbyeffect_5.0's instantiate().
// The order and signatures must match the library's vtable exactly.
class DvEffect {
public:
    virtual void setUseAlternateShader(bool) = 0;                                     // 0
    virtual void setTargetLuminance(float nits) = 0;                                  // 1
    virtual int setRpu(const uint8_t* rpu, int length, uint64_t timestamp,
                       uint32_t layerId) = 0;                                         // 2
    virtual void setSourceColor(const DvColor* color, uint32_t layerId) = 0;          // 3
    virtual void setTargetColor(const DvColor* color) = 0;                            // 4
    virtual const void* getLut(int* lutSize, uint32_t layerId) = 0;                   // 5
    virtual const char* getShaderSource() = 0;                                        // 6
    virtual std::vector<DvUniform> getUniforms(uint32_t layerId) = 0;                 // 7
    virtual void enableInverseMatrix() = 0;                                           // 8
    virtual void setSkiaBackend(int api) = 0;                                         // 9
    virtual bool reserved10() = 0;                                                    // 10
    virtual void setDimmingRatio(float ratio, uint32_t layerId) = 0;                  // 11
    virtual void reserved12(int) = 0;                                                 // 12
    virtual ~DvEffect() = 0;                                                          // 13, 14
};

constexpr int kSkiaBackendGl = 0;
constexpr int kSkiaBackendVk = 1;

constexpr uint32_t kLayerIdHevc = 1;
constexpr uint32_t kLayerIdAv1 = 2;

constexpr const char* kEffectLib = "liboplusdolbyeffect_5.0.so";
constexpr const char* kMapperLib = "libdolbyui.so";

// dolbyvision::DVGraphicBufferMapper (libdolbyui)
constexpr const char* kMapperCtor = "_ZN11dolbyvision21DVGraphicBufferMapperC1Ev";
constexpr const char* kMapperGetMetadata =
        "_ZN11dolbyvision21DVGraphicBufferMapper22getDolbyVisionMetadataEPK13native_handlePNSt3__"
        "18optionalINS4_6vectorIhNS4_9allocatorIhEEEEEERb";
constexpr const char* kMapperGetTimestamp =
        "_ZN11dolbyvision21DVGraphicBufferMapper17getVideoTimestampEPK13native_handleRm";

using InstantiateFn = DvEffect* (*)();
using MapperCtorFn = void (*)(void* self);
using MapperGetMetadataFn = int (*)(void* self, const native_handle_t* handle,
                                    std::optional<std::vector<uint8_t>>* out, bool* hasHeader);
using MapperGetTimestampFn = int (*)(void* self, const native_handle_t* handle,
                                     uint64_t* timestamp);

int32_t toRange(ui::Dataspace dataspace) {
    switch (static_cast<int32_t>(dataspace & ui::Dataspace::RANGE_MASK)) {
        case static_cast<int32_t>(ui::Dataspace::RANGE_FULL):
            return 1;
        case static_cast<int32_t>(ui::Dataspace::RANGE_EXTENDED):
            return 2;
        case static_cast<int32_t>(ui::Dataspace::RANGE_LIMITED):
            return 0;
        default:
            return 255;
    }
}

int32_t toPrimaries(ui::Dataspace dataspace) {
    switch (static_cast<int32_t>(dataspace & ui::Dataspace::STANDARD_MASK)) {
        case static_cast<int32_t>(ui::Dataspace::STANDARD_BT2020):
        case static_cast<int32_t>(ui::Dataspace::STANDARD_BT2020_CONSTANT_LUMINANCE):
            return 9;
        case static_cast<int32_t>(ui::Dataspace::STANDARD_DCI_P3):
            return 12;
        default:
            return 1;
    }
}

int32_t toSourceTransfer(ui::Dataspace dataspace) {
    switch (static_cast<int32_t>(dataspace & ui::Dataspace::TRANSFER_MASK)) {
        case static_cast<int32_t>(ui::Dataspace::TRANSFER_SMPTE_170M):
            return 6;
        case static_cast<int32_t>(ui::Dataspace::TRANSFER_GAMMA2_2):
            return 4;
        case static_cast<int32_t>(ui::Dataspace::TRANSFER_GAMMA2_6):
            return 3;
        case static_cast<int32_t>(ui::Dataspace::TRANSFER_GAMMA2_8):
            return 5;
        case static_cast<int32_t>(ui::Dataspace::TRANSFER_ST2084):
            return 16;
        case static_cast<int32_t>(ui::Dataspace::TRANSFER_HLG):
            return 18;
        default:
            return 1;
    }
}

int32_t toTargetTransfer(ui::Dataspace dataspace) {
    switch (static_cast<int32_t>(dataspace & ui::Dataspace::TRANSFER_MASK)) {
        case static_cast<int32_t>(ui::Dataspace::TRANSFER_LINEAR):
            return 8;
        case static_cast<int32_t>(ui::Dataspace::TRANSFER_SRGB):
            return 1;
        case static_cast<int32_t>(ui::Dataspace::TRANSFER_SMPTE_170M):
            return 6;
        case static_cast<int32_t>(ui::Dataspace::TRANSFER_GAMMA2_2):
            return 4;
        case static_cast<int32_t>(ui::Dataspace::TRANSFER_GAMMA2_6):
            return 3;
        case static_cast<int32_t>(ui::Dataspace::TRANSFER_GAMMA2_8):
            return 5;
        case static_cast<int32_t>(ui::Dataspace::TRANSFER_ST2084):
            return 16;
        case static_cast<int32_t>(ui::Dataspace::TRANSFER_HLG):
            return 18;
        default:
            return 1;
    }
}

bool isHdrTransfer(ui::Dataspace dataspace) {
    const auto transfer = static_cast<int32_t>(dataspace & ui::Dataspace::TRANSFER_MASK);
    return transfer == static_cast<int32_t>(ui::Dataspace::TRANSFER_ST2084) ||
            transfer == static_cast<int32_t>(ui::Dataspace::TRANSFER_HLG);
}

} // namespace

DvEffect::~DvEffect() {}

struct DolbyVisionShader::Api {
    DvEffect* effect = nullptr;
    void* mapper = nullptr;
    MapperGetMetadataFn getMetadata = nullptr;
    MapperGetTimestampFn getTimestamp = nullptr;
};

DolbyVisionShader& DolbyVisionShader::getInstance() {
    static DolbyVisionShader* instance = new DolbyVisionShader();
    return *instance;
}

bool DolbyVisionShader::loadMapper() {
    if (mApi) return true;
    if (mMapperLoadAttempted) return false;
    mMapperLoadAttempted = true;

    void* mapperLib = dlopen(kMapperLib, RTLD_NOW | RTLD_LOCAL);
    if (!mapperLib) {
        ALOGI("Dolby Vision: %s unavailable (%s)", kMapperLib, dlerror());
        return false;
    }
    auto ctor = reinterpret_cast<MapperCtorFn>(dlsym(mapperLib, kMapperCtor));
    auto getMetadata = reinterpret_cast<MapperGetMetadataFn>(dlsym(mapperLib, kMapperGetMetadata));
    auto getTimestamp =
            reinterpret_cast<MapperGetTimestampFn>(dlsym(mapperLib, kMapperGetTimestamp));
    if (!ctor || !getMetadata || !getTimestamp) {
        ALOGE("Dolby Vision: %s is missing expected symbols", kMapperLib);
        return false;
    }

    // DVGraphicBufferMapper holds a single pointer; leave room in case that grows.
    void* mapper = ::operator new(64);
    std::memset(mapper, 0, 64);
    ctor(mapper);

    auto* api = new Api();
    api->mapper = mapper;
    api->getMetadata = getMetadata;
    api->getTimestamp = getTimestamp;
    mApi = api;
    return true;
}

// Only called once a layer carries a valid Dolby Vision RPU, so other HDR content never runs
// the vendor library.
bool DolbyVisionShader::loadEffect(bool vulkan) {
    if (mApi->effect) return true;
    if (mEffectLoadAttempted) return false;
    mEffectLoadAttempted = true;

    void* effectLib = dlopen(kEffectLib, RTLD_NOW | RTLD_LOCAL);
    if (!effectLib) {
        ALOGI("Dolby Vision: %s unavailable (%s)", kEffectLib, dlerror());
        return false;
    }
    auto instantiate = reinterpret_cast<InstantiateFn>(dlsym(effectLib, "instantiate"));
    if (!instantiate) {
        ALOGE("Dolby Vision: %s has no instantiate()", kEffectLib);
        return false;
    }

    DvEffect* effect = instantiate();
    if (!effect) {
        ALOGE("Dolby Vision: instantiate() failed");
        return false;
    }
    // Our Skia samples YUV buffers with the regular YCbCr-to-RGB conversion, so let the effect
    // undo that conversion (stock's DolbyRenderer enables this by default as well). In this mode
    // the effect uses one shader for both GL and VK.
    const bool inverseMatrix =
            property_get_bool("debug.renderengine.dolby_vision.inverse_matrix", true);
    if (inverseMatrix) {
        effect->enableInverseMatrix();
    }
    effect->setSkiaBackend(vulkan ? kSkiaBackendVk : kSkiaBackendGl);

    mApi->effect = effect;
    ALOGI("Dolby Vision: GPU reshaping enabled (%s, inverse matrix %d, working cs %d)",
          vulkan ? "VK" : "GL", inverseMatrix, mUseWorkingColorSpace);
    return true;
}

bool DolbyVisionShader::readRpu(const sp<GraphicBuffer>& buffer, std::vector<uint8_t>* storage,
                                const uint8_t** rpu, int* rpuLength, uint32_t* layerId,
                                uint64_t* timestamp) {
    const native_handle_t* handle = buffer->getNativeBuffer()->handle;
    if (!handle) return false;

    std::optional<std::vector<uint8_t>> metadata;
    bool hasHeader = false;
    const int status = mApi->getMetadata(mApi->mapper, handle, &metadata, &hasHeader);
    if (status != 0 || !metadata) {
        if (mVerbose) ALOGD("Dolby Vision: no metadata (status %d)", status);
        return false;
    }
    *storage = std::move(*metadata);

    const uint8_t* data = storage->data();
    size_t size = storage->size();
    if (hasHeader) {
        // Qualcomm's mapper prefixes the payload with its length (u32) and padding.
        if (size < 8) return false;
        uint32_t length;
        std::memcpy(&length, data, sizeof(length));
        data += 8;
        size -= 8;
        if (length > size) {
            if (mVerbose) ALOGD("Dolby Vision: bad length %u > %zu", length, size);
            return false;
        }
        size = length;
    }
    if (size < 7) {
        if (mVerbose) ALOGD("Dolby Vision: metadata too short (%zu, header %d)", size, hasHeader);
        return false;
    }

    static constexpr uint8_t kHevcRpu[] = {0x00, 0x00, 0x00, 0x01, 0x7c, 0x01, 0x19};
    static constexpr uint8_t kAv1Rpu[] = {0x05, 0x20, 0xb6, 0x01, 0x32, 0x7d, 0x02};
    if (std::memcmp(data, kHevcRpu, sizeof(kHevcRpu)) == 0) {
        *layerId = kLayerIdHevc;
    } else if (std::memcmp(data, kAv1Rpu, sizeof(kAv1Rpu)) == 0) {
        *layerId = kLayerIdAv1;
    } else {
        if (mVerbose) {
            ALOGD("Dolby Vision: no RPU signature (%zu bytes, header %d): %02x %02x %02x %02x "
                  "%02x %02x %02x",
                  size, hasHeader, data[0], data[1], data[2], data[3], data[4], data[5], data[6]);
        }
        return false;
    }

    *timestamp = 0;
    mApi->getTimestamp(mApi->mapper, handle, timestamp);
    *rpu = data;
    *rpuLength = static_cast<int>(size);
    return true;
}

sk_sp<SkRuntimeEffect> DolbyVisionShader::runtimeEffect() {
    const char* source = mApi->effect->getShaderSource();
    if (!source || !*source) {
        ALOGE("Dolby Vision: effect returned no shader");
        mDisabled = true;
        return nullptr;
    }
    if (mRuntimeEffect && mShaderSource == source) return mRuntimeEffect;

    ALOGI("Dolby Vision: shader:\n%s", source);
    auto [effect, error] = SkRuntimeEffect::MakeForShader(SkString(source));
    if (!effect) {
        // Latch: a broken program would fail the same way on every frame.
        ALOGE("Dolby Vision: shader compilation failed, disabling: %s", error.c_str());
        mDisabled = true;
        return nullptr;
    }
    mShaderSource = source;
    mRuntimeEffect = std::move(effect);
    return mRuntimeEffect;
}

sk_sp<SkShader> DolbyVisionShader::create(const sk_sp<SkShader>& input, const LayerSettings& layer,
                                          const DisplaySettings& display,
                                          const sp<GraphicBuffer>& buffer,
                                          ui::Dataspace outputDataspace, float dimmingRatio,
                                          bool vulkan,
                                          ftl::Flags<ColorSpaceOptions> colorSpaceOptions) {
    if (!input || !buffer || !isHdrTransfer(layer.sourceDataspace)) return nullptr;

    std::lock_guard lock(mMutex);
    if (mDisabled) return nullptr;
    if (!mMapperLoadAttempted) {
        if (!property_get_bool("debug.renderengine.dolby_vision", true)) {
            mDisabled = true;
            return nullptr;
        }
        mVerbose = property_get_bool("debug.renderengine.dolby_vision.log", false);
        mUseWorkingColorSpace =
                property_get_bool("debug.renderengine.dolby_vision.working_cs", true);
    }
    if (!loadMapper()) {
        mDisabled = true;
        return nullptr;
    }

    std::vector<uint8_t> storage;
    const uint8_t* rpu = nullptr;
    int rpuLength = 0;
    uint32_t layerId = 0;
    uint64_t timestamp = 0;
    if (!readRpu(buffer, &storage, &rpu, &rpuLength, &layerId, &timestamp)) return nullptr;

    if (!loadEffect(vulkan)) {
        mDisabled = true;
        return nullptr;
    }

    SFTRACE_NAME("DolbyVisionShader");
    DvEffect* effect = mApi->effect;

    DvColor source;
    source.primaries = toPrimaries(layer.sourceDataspace);
    source.range = toRange(layer.sourceDataspace);
    source.transfer = toSourceTransfer(layer.sourceDataspace);
    source.maxLuminance = layer.source.buffer.maxLuminanceNits;
    effect->setSourceColor(&source, layerId);
    const int rpuStatus = effect->setRpu(rpu, rpuLength, timestamp, layerId);
    effect->setUseAlternateShader(false);
    if (mVerbose) {
        ALOGD("Dolby Vision: layer %u '%s' rpu %d bytes ts %" PRIu64 " -> %d, src ds 0x%x "
              "(prim %d range %d tf %d) out ds 0x%x dim %.3f nits %.1f",
              layerId, layer.name.c_str(), rpuLength, timestamp, rpuStatus,
              static_cast<int32_t>(layer.sourceDataspace), source.primaries, source.range,
              source.transfer, static_cast<int32_t>(outputDataspace), dimmingRatio,
              display.currentLuminanceNits);
    }

    sk_sp<SkRuntimeEffect> runtime = runtimeEffect();
    if (!runtime) return nullptr;

    DvColor target;
    target.primaries = toPrimaries(outputDataspace);
    target.range = toRange(outputDataspace);
    target.transfer = toTargetTransfer(outputDataspace);
    effect->setTargetColor(&target);
    effect->setDimmingRatio(dimmingRatio, layerId);
    effect->setTargetLuminance(display.currentLuminanceNits);

    int lutSize = 0;
    const void* lut = effect->getLut(&lutSize, layerId);
    if (!lut || lutSize <= 0 || lutSize > 256) {
        ALOGW("Dolby Vision: no LUT for layer %u (size %d)", layerId, lutSize);
        return nullptr;
    }

    sk_sp<SkData> uniforms = SkData::MakeZeroInitialized(runtime->uniformSize());
    for (const DvUniform& uniform : effect->getUniforms(layerId)) {
        const SkRuntimeEffect::Uniform* slot = runtime->findUniform(uniform.name);
        if (slot && slot->sizeInBytes() == uniform.data.size()) {
            std::memcpy(static_cast<uint8_t*>(uniforms->writable_data()) + slot->offset,
                        uniform.data.data(), uniform.data.size());
        } else if (mVerbose) {
            ALOGD("Dolby Vision: uniform '%s' (%zu bytes) not matched", uniform.name.c_str(),
                  uniform.data.size());
        }
    }

    // The effect works on the values as they come out of the YCbCr sampler. By default run it
    // in the layer's own color space (no conversion of the input or of the LUT) and treat its
    // output as encoded for the output dataspace. With working_cs=0 the shader is used as is,
    // like stock does.
    const sk_sp<SkColorSpace> outputSpace = toSkColorSpace(outputDataspace, colorSpaceOptions);
    const sk_sp<SkColorSpace> workingSpace = mUseWorkingColorSpace
            ? toSkColorSpace(layer.sourceDataspace, colorSpaceOptions)
            : outputSpace;

    // The 3D LUT is packed as N slices of N x N texels side by side.
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(SkImageInfo::Make(lutSize * lutSize, lutSize,
                                                 kRGB_101010x_SkColorType, kPremul_SkAlphaType,
                                                 workingSpace))) {
        return nullptr;
    }
    std::memcpy(bitmap.getPixels(), lut,
                static_cast<size_t>(lutSize) * lutSize * lutSize * sizeof(uint32_t));
    bitmap.setImmutable();
    sk_sp<SkImage> lutImage = bitmap.asImage();
    if (!lutImage) return nullptr;
    const SkMatrix lutMatrix =
            SkMatrix::Scale(1.f / static_cast<float>(lutSize * lutSize), 1.f / lutSize);
    sk_sp<SkShader> lutShader =
            lutImage->makeShader(SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
                                 lutMatrix);

    std::vector<SkRuntimeEffect::ChildPtr> children(runtime->children().size());
    const SkRuntimeEffect::Child* lutChild = runtime->findChild("lut_cube");
    const SkRuntimeEffect::Child* inputChild = runtime->findChild("child");
    if (!inputChild || !lutChild) {
        ALOGE("Dolby Vision: shader children missing, disabling");
        mDisabled = true;
        return nullptr;
    }
    children[lutChild->index] = lutShader;
    children[inputChild->index] = input;

    sk_sp<SkShader> shader = runtime->makeShader(std::move(uniforms), SkSpan(children));
    if (!shader) {
        ALOGE("Dolby Vision: makeShader failed");
        return nullptr;
    }
    if (!mUseWorkingColorSpace) return shader;
    return shader->makeWithWorkingColorSpace(workingSpace, outputSpace);
}

} // namespace skia
} // namespace renderengine
} // namespace android
