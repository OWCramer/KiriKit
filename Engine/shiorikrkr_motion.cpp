/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// emoteplayer.dll's script surface for ShioriKrkr: the Motion class games test and read
// constants from (NEKOPARA's MainWindow.tjs sets drawDevice.maskMode = Motion.MaskModeAlpha
// before any character is drawn). The constants' values are the plug-in's public interface,
// as the games' scripts expect them. Characters themselves go through D3DEmotePlayer
// (shiorikrkr_emote.cpp).
#include "tjsCommHead.h"
#include "ncbind/ncbind.hpp"

#define NCB_MODULE_NAME TJS_N("emoteplayer.dll")

// Motion.ResourceManager: the games' cache of motion files. Files are read when a player
// loads them, so this only keeps the game's bookkeeping working.
class ShioriMotionResources {
public:
    ShioriMotionResources(iTJSDispatch2 *, tjs_int) {}
    tTJSVariant load(ttstr) { return tTJSVariant(); }
    void unload(ttstr) {}
    void unloadAll() {}
    void clearCache() {}
    void setEmotePSBDecryptSeed(tjs_int) {}
    void setEmotePSBDecryptFunc(tTJSVariant) {}
};

NCB_REGISTER_SUBCLASS(ShioriMotionResources) {
    NCB_CONSTRUCTOR((iTJSDispatch2 *, tjs_int));
    NCB_METHOD(load);
    NCB_METHOD(unload);
    NCB_METHOD(unloadAll);
    NCB_METHOD(clearCache);
    NCB_METHOD(setEmotePSBDecryptSeed);
    NCB_METHOD(setEmotePSBDecryptFunc);
}

// Motion.Player / Motion.EmotePlayer on plain layers: not drawn yet (NEKOPARA Vol. 0–2 draw
// their characters through D3DEmotePlayer); they answer as idle players so scripts go on.
class ShioriMotionPlayerStub {
public:
    explicit ShioriMotionPlayerStub(ShioriMotionResources *) {}
    void play(ttstr, tjs_int) {}
    void progress(tjs_real) {}
    void skip() {}
    void skipToSync() {}
    void pass() {}
    void stop() {}
    void clear() {}
    void initPhysics() {}
    void setVariable(ttstr, tjs_real, tjs_real, tjs_real) {}
    tjs_real getVariable(ttstr) { return 0; }
    void setCoord(tjs_real, tjs_real, tjs_real, tjs_real) {}
    void setScale(tjs_real, tjs_real, tjs_real) {}
    void setRotate(tjs_real, tjs_real, tjs_real) {}
    void setColor(tjs_int, tjs_real, tjs_real) {}
    void playTimeline(ttstr, tjs_int) {}
    void stopTimeline(ttstr) {}
    bool getTimelinePlaying(ttstr) { return false; }
    bool getAnimating() { return false; }
    void setAnimating(bool) {}
    bool getPlaying() { return false; }
    void setPlaying(bool) {}
    bool getAllplaying() { return false; }
    void setAllplaying(bool) {}
    ttstr getMotion() { return motion_; }
    void setMotion(ttstr m) { motion_ = m; }
    ttstr getChara() { return chara_; }
    void setChara(ttstr c) { chara_ = c; }

private:
    ttstr motion_, chara_;
};

class ShioriEmotePlayerStub : public ShioriMotionPlayerStub {
public:
    explicit ShioriEmotePlayerStub(ShioriMotionResources *r) : ShioriMotionPlayerStub(r) {}
};

#define SHIORI_MOTION_STUB_METHODS \
    NCB_CONSTRUCTOR((ShioriMotionResources *)); \
    NCB_METHOD(play); NCB_METHOD(progress); NCB_METHOD(skip); NCB_METHOD(skipToSync); NCB_METHOD(pass); \
    NCB_METHOD(stop); NCB_METHOD(clear); NCB_METHOD(initPhysics); NCB_METHOD(setVariable); NCB_METHOD(getVariable); \
    NCB_METHOD(setCoord); NCB_METHOD(setScale); NCB_METHOD(setRotate); NCB_METHOD(setColor); \
    NCB_METHOD(playTimeline); NCB_METHOD(stopTimeline); NCB_METHOD(getTimelinePlaying); \
    NCB_PROPERTY(animating, getAnimating, setAnimating); NCB_PROPERTY(playing, getPlaying, setPlaying); \
    NCB_PROPERTY(allplaying, getAllplaying, setAllplaying); NCB_PROPERTY(motion, getMotion, setMotion); \
    NCB_PROPERTY(chara, getChara, setChara)

NCB_REGISTER_SUBCLASS(ShioriMotionPlayerStub) { SHIORI_MOTION_STUB_METHODS; }
NCB_REGISTER_SUBCLASS(ShioriEmotePlayerStub) { SHIORI_MOTION_STUB_METHODS; }

struct ShioriMotion {
    static bool getD3DAvailable() { return true; }
    static bool enableD3D;
    static bool getEnableD3D() { return enableD3D; }
    static void setEnableD3D(bool v) { enableD3D = v; }
};
bool ShioriMotion::enableD3D = true;

NCB_REGISTER_CLASS_DIFFER(Motion, ShioriMotion) {
    NCB_METHOD(getD3DAvailable);
    NCB_PROPERTY(enableD3D, getEnableD3D, setEnableD3D);
    NCB_SUBCLASS(ResourceManager, ShioriMotionResources);
    NCB_SUBCLASS(Player, ShioriMotionPlayerStub);
    NCB_SUBCLASS(EmotePlayer, ShioriEmotePlayerStub);
    Variant("MaskModeStencil", (tjs_int)0);
    Variant("MaskModeAlpha", (tjs_int)1);
    Variant("PlayFlagForce", (tjs_int)1);
    Variant("PlayFlagChain", (tjs_int)2);
    Variant("PlayFlagAsCan", (tjs_int)4);
    Variant("PlayFlagJoin", (tjs_int)8);
    Variant("PlayFlagStealth", (tjs_int)16);
    Variant("ShapeTypePoint", (tjs_int)0);
    Variant("ShapeTypeCircle", (tjs_int)1);
    Variant("ShapeTypeRect", (tjs_int)2);
    Variant("ShapeTypeQuad", (tjs_int)3);
    Variant("TimelinePlayFlagParallel", (tjs_int)1);
    Variant("TimelinePlayFlagDifference", (tjs_int)2);
}
