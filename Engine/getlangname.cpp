/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// getLangName.dll, which games call to pick their UI language from the system's (NEKOPARA,
// Ninja Girl, Letters From a Rainy Day: System.getCurrentUILangName()). It answers the user's UI
// language by its English name ("English", "Japanese", "Chinese (Simplified)"): the games lower
// the case and look for "japanese", "chinese" and "english" in it (NEKOPARA Vol. 0's
// getSystemLanguageId, Extra's language alias map), so a locale name such as "ja-JP" counted as
// English. the user's own plug-in (AetherKrkr patch 0021, Shiori-written), re-homed for ShioriKrkr.
#include "ncbind/ncbind.hpp"

#include <cstdlib>
#include <string>

#include <CoreFoundation/CoreFoundation.h>

#define NCB_MODULE_NAME TJS_W("getLangName.dll")

namespace {

std::string currentUILanguage() {
    if (const char *forced = std::getenv("SHIORI_UI_LANGUAGE"); forced && *forced) return forced;
    std::string result;
    if (CFArrayRef languages = CFLocaleCopyPreferredLanguages()) {
        if (CFArrayGetCount(languages) > 0) {
            auto first = static_cast<CFStringRef>(CFArrayGetValueAtIndex(languages, 0));
            char buffer[64] = {};
            if (CFStringGetCString(first, buffer, sizeof(buffer), kCFStringEncodingUTF8)) result = buffer;
        }
        CFRelease(languages);
    }
    auto starts = [&](const char *prefix) { return result.rfind(prefix, 0) == 0; };
    if (starts("ja")) return "Japanese";
    if (starts("zh-Hant") || starts("zh-TW") || starts("zh-HK")) return "Chinese (Traditional)";
    if (starts("zh")) return "Chinese (Simplified)";
    if (starts("ko")) return "Korean";
    if (starts("fr")) return "French";
    if (starts("de")) return "German";
    if (starts("es")) return "Spanish";
    if (starts("it")) return "Italian";
    if (starts("ru")) return "Russian";
    if (starts("pt")) return "Portuguese";
    return "English";
}

} // namespace

struct ShioriLangName {
    static tjs_error getCurrentUILangName(tTJSVariant *result, tjs_int, tTJSVariant **, iTJSDispatch2 *) {
        if (result) *result = ttstr(currentUILanguage().c_str());
        return TJS_S_OK;
    }
};

NCB_ATTACH_CLASS(ShioriLangName, System) {
    RawCallback("getCurrentUILangName", &ShioriLangName::getCurrentUILangName, TJS_STATICMEMBER);
}
