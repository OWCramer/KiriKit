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
