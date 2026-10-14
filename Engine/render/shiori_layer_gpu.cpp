/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Plug-ins that work on a layer's pixels by pointer (mainImageBuffer) make the CPU wait for the
// GPU's work on that layer every time; these do the same work on the GPU instead, given the
// layers' TJS objects.
#include "tjsCommHead.h"
#include "LayerIntf.h"
#include "LayerBitmapIntf.h"
#include "shiori_metal_render.h"
#include <cstring>
#include <cstdlib>

static tTJSNI_BaseLayer *ShioriLayerOf(iTJSDispatch2 *object) {
    if (!object) return nullptr;
    tTJSNI_BaseLayer *layer = nullptr;
    if (TJS_FAILED(object->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
                                                 reinterpret_cast<iTJSNativeInstance **>(&layer))))
        return nullptr;
    return layer;
}

// layerExRaster's copyRaster: `offsets` has one entry per row of the clip. False leaves it to the
// plug-in's own loop.
bool ShioriLayerRasterCopy(iTJSDispatch2 *target, iTJSDispatch2 *source, int left, int top, int width, int height,
                           const int *offsets) {
    tTJSNI_BaseLayer *tar = ShioriLayerOf(target), *src = ShioriLayerOf(source);
    if (!tar || !src || tar == src || !tar->GetMainImage() || !src->GetMainImage()) return false;
    iTVPTexture2D *sourceTexture = src->GetMainImage()->GetTexture();
    // The target's own image (copied first when it shares one), as writing its pixels does.
    iTVPTexture2D *targetTexture = tar->GetMainImage()->GetTextureForRender(false, nullptr);
    if (!sourceTexture || !targetTexture || sourceTexture == targetTexture) return false;
    if (!shiori::metal::rasterCopy(targetTexture, sourceTexture, left, top, width, height, offsets)) return false;
    tar->SetImageModified(true);
    return true;
}

// The layer's own image for a GPU operation on its pixels (copied first when it shares one, as
// writing its pixels does); nullptr when it isn't the renderer's.
static iTVPTexture2D *ShioriLayerTextureForWrite(iTJSDispatch2 *object) {
    // SHIORIKRKR_LAYEREX_CPU=1 leaves these to the plug-ins' CPU loops (for comparing).
    static const bool cpu = getenv("SHIORIKRKR_LAYEREX_CPU") != nullptr;
    if (cpu) return nullptr;
    tTJSNI_BaseLayer *layer = ShioriLayerOf(object);
    if (!layer || !layer->GetMainImage()) return nullptr;
    return layer->GetMainImage()->GetTextureForRender(false, nullptr);
}

// layerExImage's per-pixel operations (shiori::metal::ImageOp's codes) on the layer's clip.
// False leaves it to the plug-in's own loop.
bool ShioriLayerImageOp(iTJSDispatch2 *object, int left, int top, int width, int height, unsigned op, const int *ints,
                        const float *floats, const unsigned char *lut) {
    if (width <= 0 || height <= 0) return true;
    iTVPTexture2D *texture = ShioriLayerTextureForWrite(object);
    if (!texture) return false;
    shiori::metal::ImageOp o;
    o.op = op;
    o.left = left, o.top = top, o.width = width, o.height = height;
    for (int i = 0; i < 4 && ints; ++i) o.i[i] = ints[i];
    for (int i = 0; i < 3 && floats; ++i) o.f[i] = floats[i];
    static unsigned calls = 0;
    o.seed = 0x9E3779B9u * ++calls;
    if (lut) memcpy(o.lut, lut, 256);
    if (!shiori::metal::imageOp(texture, o)) return false;
    ShioriLayerOf(object)->SetImageModified(true);
    return true;
}

// layerExImage's gaussianBlur on the layer's clip, with the plug-in's weights.
bool ShioriLayerGaussianBlur(iTJSDispatch2 *object, int left, int top, int width, int height, const float *matrix, int length) {
    if (width <= 0 || height <= 0) return true;
    iTVPTexture2D *texture = ShioriLayerTextureForWrite(object);
    if (!texture) return false;
    if (!shiori::metal::gaussianBlur(texture, left, top, width, height, matrix, length)) return false;
    ShioriLayerOf(object)->SetImageModified(true);
    return true;
}
