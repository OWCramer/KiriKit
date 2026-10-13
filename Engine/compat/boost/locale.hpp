/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// The few Boost.Locale conversions krkr2's core calls (utf_to_utf between UTF-8, UTF-16 and
// UTF-32, and to_utf from a named charset), so the engine needs no Boost. Written for Shiori;
// charset conversion goes through the system's iconv.
#pragma once

#include <iconv.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace boost {
namespace locale {
namespace conv {

enum method_type { skip = 0, stop = 1, default_method = skip };

namespace shiori_detail {

template <typename C>
inline const C *end_of(const C *s) {
    while (*s) ++s;
    return s;
}

// Reads one code point from [p, e); invalid input becomes U+FFFD.
template <typename C>
inline char32_t decode(const C *&p, const C *e) {
    if constexpr (sizeof(C) == 1) {
        const unsigned char c = static_cast<unsigned char>(*p++);
        if (c < 0x80) return c;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
        if (extra < 0) return 0xFFFD;
        char32_t cp = c & (0x3F >> extra);
        for (int i = 0; i < extra; ++i) {
            if (p >= e || (static_cast<unsigned char>(*p) & 0xC0) != 0x80) return 0xFFFD;
            cp = (cp << 6) | (static_cast<unsigned char>(*p++) & 0x3F);
        }
        return cp;
    } else if constexpr (sizeof(C) == 2) {
        const char32_t c = static_cast<char16_t>(*p++);
        if (c >= 0xD800 && c < 0xDC00 && p < e) {
            const char32_t d = static_cast<char16_t>(*p);
            if (d >= 0xDC00 && d < 0xE000) {
                ++p;
                return 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
            }
        }
        return c;
    } else {
        return static_cast<char32_t>(*p++);
    }
}

template <typename C>
inline void encode(std::basic_string<C> &out, char32_t cp) {
    if constexpr (sizeof(C) == 1) {
        if (cp < 0x80) {
            out += static_cast<C>(cp);
        } else if (cp < 0x800) {
            out += static_cast<C>(0xC0 | (cp >> 6));
            out += static_cast<C>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<C>(0xE0 | (cp >> 12));
            out += static_cast<C>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<C>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<C>(0xF0 | (cp >> 18));
            out += static_cast<C>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<C>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<C>(0x80 | (cp & 0x3F));
        }
    } else if constexpr (sizeof(C) == 2) {
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<C>(0xD800 + (cp >> 10));
            out += static_cast<C>(0xDC00 + (cp & 0x3FF));
        } else {
            out += static_cast<C>(cp);
        }
    } else {
        out += static_cast<C>(cp);
    }
}

} // namespace shiori_detail

template <typename To, typename From>
inline std::basic_string<To> utf_to_utf(const From *begin, const From *end, method_type = default_method) {
    std::basic_string<To> out;
    out.reserve(static_cast<size_t>(end - begin));
    const From *p = begin;
    while (p < end) shiori_detail::encode(out, shiori_detail::decode(p, end));
    return out;
}

template <typename To, typename From>
inline std::basic_string<To> utf_to_utf(const From *text, method_type how = default_method) {
    return utf_to_utf<To>(text, shiori_detail::end_of(text), how);
}

template <typename To, typename From>
inline std::basic_string<To> utf_to_utf(const std::basic_string<From> &text, method_type how = default_method) {
    return utf_to_utf<To>(text.data(), text.data() + text.size(), how);
}

// Text in a named charset (as iconv names it: "cp932", "GBK" ...) to UTF.
template <typename To>
inline std::basic_string<To> to_utf(const char *begin, const char *end, const std::string &charset,
                                    method_type = default_method) {
    std::basic_string<To> out;
    iconv_t cd = iconv_open("UTF-32LE", charset.c_str());
    if (cd == reinterpret_cast<iconv_t>(-1)) return utf_to_utf<To>(begin, end);
    std::string buffer((end - begin) * 4 + 16, '\0');
    char *in = const_cast<char *>(begin);
    size_t inLeft = static_cast<size_t>(end - begin);
    char *outp = buffer.data();
    size_t outLeft = buffer.size();
    while (inLeft > 0) {
        if (iconv(cd, &in, &inLeft, &outp, &outLeft) == static_cast<size_t>(-1)) {
            if (inLeft == 0 || outLeft < 4) break;
            // Skip a byte that can't be converted.
            ++in;
            --inLeft;
        }
    }
    iconv_close(cd);
    const size_t produced = buffer.size() - outLeft;
    for (size_t i = 0; i + 3 < produced; i += 4) {
        char32_t cp;
        std::memcpy(&cp, buffer.data() + i, 4);
        shiori_detail::encode(out, cp);
    }
    return out;
}

template <typename To>
inline std::basic_string<To> to_utf(const std::string &text, const std::string &charset,
                                    method_type how = default_method) {
    return to_utf<To>(text.data(), text.data() + text.size(), charset, how);
}

} // namespace conv
} // namespace locale
} // namespace boost
