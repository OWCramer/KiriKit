/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// getLangName.dll, which games call to pick their UI language from the system's (NEKOPARA,
// Ninja Girl, Letters From a Rainy Day: System.getCurrentUILangName()). On Windows it returns
// the user's UI language as a locale name such as "en-US" or "ja-JP". the user's own plug-in
// (AetherKrkr patch 0021, Shiori-written), re-homed for ShioriKrkr.
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
    // "en-US", or "zh-Hans-CN": keep the language and the region, as Windows names them.
    if (const auto dash = result.find('-'); dash != std::string::npos) {
        const auto last = result.rfind('-');
        if (last != dash) result = result.substr(0, dash) + result.substr(last);
    }
    return result.empty() ? "en-US" : result;
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
