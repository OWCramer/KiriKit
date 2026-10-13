/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// krkrsteam.dll: the `Steam` class Steam editions of KiriKiri games link. On the phone there is
// no Steam client beside the game, so it answers what the games ask at start-up: the game's
// language (Steam's names, from the device's preferred language), no achievements reached and
// Steam Cloud off. The games then pick their first language as they do on Steam (NEKOPARA
// Extra opened its language screen in Japanese without it).
//
// Members are the ones the games' own scripts call (NEKOPARA's custom.tjs and steam.tjs):
// getLanguage(), setAchievement(name), clearAchievement(name), getAchievement(name),
// achievementsCount, cloudEnabled. Written from those calls; no other plug-in's code read.
#include "ncbind/ncbind.hpp"

#include <cstdlib>
#include <string>

#include <CoreFoundation/CoreFoundation.h>

#define NCB_MODULE_NAME TJS_W("krkrsteam.dll")

namespace {

// Steam's language API names for the device's first preferred language.
std::string steamLanguage() {
    if (const char *forced = std::getenv("SHIORI_STEAM_LANGUAGE"); forced && *forced) return forced;
    std::string tag;
    if (CFArrayRef languages = CFLocaleCopyPreferredLanguages()) {
        if (CFArrayGetCount(languages) > 0) {
            auto first = static_cast<CFStringRef>(CFArrayGetValueAtIndex(languages, 0));
            char buffer[64] = {};
            if (CFStringGetCString(first, buffer, sizeof(buffer), kCFStringEncodingUTF8)) tag = buffer;
        }
        CFRelease(languages);
    }
    auto starts = [&](const char *prefix) { return tag.rfind(prefix, 0) == 0; };
    if (starts("ja")) return "japanese";
    if (starts("zh-Hant") || starts("zh-TW") || starts("zh-HK")) return "tchinese";
    if (starts("zh")) return "schinese";
    if (starts("ko")) return "koreana";
    if (starts("fr")) return "french";
    if (starts("de")) return "german";
    if (starts("es")) return "spanish";
    if (starts("it")) return "italian";
    if (starts("ru")) return "russian";
    if (starts("pt-BR")) return "brazilian";
    if (starts("pt")) return "portuguese";
    return "english";
}

} // namespace

struct ShioriSteam {
    static tjs_error getLanguage(tTJSVariant *r, tjs_int, tTJSVariant **, iTJSDispatch2 *) {
        if (r) *r = ttstr(steamLanguage().c_str());
        if (std::getenv("SHIORIKRKR_TRACE_STEAM")) fprintf(stderr, "steam: getLanguage -> %s\n", steamLanguage().c_str());
        return TJS_S_OK;
    }
    // Achievements: none reached, none recorded.
    static tjs_error getAchievement(tTJSVariant *r, tjs_int, tTJSVariant **, iTJSDispatch2 *) {
        if (r) *r = false;
        return TJS_S_OK;
    }
    static tjs_error setAchievement(tTJSVariant *r, tjs_int, tTJSVariant **, iTJSDispatch2 *) {
        if (r) *r = false;
        return TJS_S_OK;
    }
    static tjs_error clearAchievement(tTJSVariant *r, tjs_int, tTJSVariant **, iTJSDispatch2 *) {
        if (r) *r = false;
        return TJS_S_OK;
    }
    static tjs_error zero(tTJSVariant *r, tjs_int, tTJSVariant **, iTJSDispatch2 *) {
        if (r) *r = tjs_int(0);
        return TJS_S_OK;
    }
    static tjs_error no(tTJSVariant *r, tjs_int, tTJSVariant **, iTJSDispatch2 *) {
        if (r) *r = false;
        return TJS_S_OK;
    }
};

NCB_REGISTER_CLASS_DIFFER(Steam, ShioriSteam) {
    Constructor();
    RawCallback("getLanguage", &ShioriSteam::getLanguage, TJS_STATICMEMBER);
    RawCallback("getAchievement", &ShioriSteam::getAchievement, TJS_STATICMEMBER);
    RawCallback("setAchievement", &ShioriSteam::setAchievement, TJS_STATICMEMBER);
    RawCallback("clearAchievement", &ShioriSteam::clearAchievement, TJS_STATICMEMBER);
    RawCallback("achievementsCount", &ShioriSteam::zero, 0, TJS_STATICMEMBER);
    // Steam Cloud: off (Shiori syncs saves itself).
    RawCallback("cloudEnabled", &ShioriSteam::no, 0, TJS_STATICMEMBER);
    RawCallback("cloudFileCount", &ShioriSteam::zero, 0, TJS_STATICMEMBER);
    RawCallback("getCloudFileInfo", &ShioriSteam::no, TJS_STATICMEMBER);
    RawCallback("copyCloudFile", &ShioriSteam::no, TJS_STATICMEMBER);
    RawCallback("deleteCloudFile", &ShioriSteam::no, TJS_STATICMEMBER);
    // Screenshots and broadcasting: neither happens here.
    RawCallback("hookScreenshots", &ShioriSteam::no, TJS_STATICMEMBER);
    RawCallback("writeScreenshot", &ShioriSteam::no, TJS_STATICMEMBER);
    RawCallback("hookBroadcasting", &ShioriSteam::no, TJS_STATICMEMBER);
    RawCallback("isBroadcasting", &ShioriSteam::no, TJS_STATICMEMBER);
}
