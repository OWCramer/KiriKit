/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// textrender.dll for ShioriKrkr: the TextRenderBase class NEKO WORKs' games (and others on the
// same KAG) lay their message text out with. Written for Shiori from the games' own side of the
// interface: TextRender.tjs documents the markup (\n \t \i \r \w \k \x, [ruby], [ruby,n], %f %b
// %i %s %e %<size>; %B %S #rrggbb; %r, %C %R %L %p, %d %w %D, $var; &graph;) and calls back into
// the script for what the engine can't know by itself: onGetTextWidth(text, size) for widths,
// onEval(name) for $var;, onLabel(name) for %D$label;, onGetGraphSize(file) for &graph; and
// onFontChange(elm) when the font changes. The script draws the characters it gets back from
// getCharacters() and paces them with each one's delay (calcShowCount, getKeyWait).
//
// 3c1u/TextRender (Apache-2.0 / MIT) was read for the shape of the result dictionaries; no
// code from krkrsdl3's, krkr2's or AetherKrkr's textrender is in here.
#include "tjsCommHead.h"
#include <ncbind.hpp>
#include "tjsArray.h"
#include "tjsDictionary.h"
#include "MsgIntf.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#define NCB_MODULE_NAME TJS_W("textrender.dll")

namespace {

using u16 = std::u16string;

struct Style {
    bool bold = false, italic = false, shadow = true, edge = false;
    u16 face = u"user";
    tjs_int fontSize = 24, rubySize = 10, rubyOffset = -2;
    tjs_uint32 chColor = 0xffffff, shadowColor = 0x000000, edgeColor = 0x0080ff;
    tjs_int lineSpacing = 6, pitch = 0, lineSize = 0;
};

struct Ruby {
    u16 text;
    double x = 0, y = 0;
    tjs_int size = 0;
};

struct Char {
    u16 text;
    bool graph = false, vertical = false, bold = false, italic = false, shadow = false, edge = false;
    u16 face;
    double x = 0, y = 0, cw = 0;
    tjs_int size = 0;
    tjs_uint32 color = 0, shadowColor = 0, edgeColor = 0;
    tjs_int delay = 0;
    std::vector<Ruby> ruby;
    int line = 0;
    u16 link; // link name (%L... not used) kept for getLinkNames
};

tjs_int intOf(iTJSDispatch2 *d, const tjs_char *name, tjs_int fallback) {
    tTJSVariant v;
    if (TJS_SUCCEEDED(d->PropGet(0, name, nullptr, &v, d)) && v.Type() != tvtVoid) return tjs_int(v.AsInteger());
    return fallback;
}

bool has(iTJSDispatch2 *d, const tjs_char *name, tTJSVariant &v) {
    return TJS_SUCCEEDED(d->PropGet(0, name, nullptr, &v, d)) && v.Type() != tvtVoid;
}

void put(iTJSDispatch2 *d, const tjs_char *name, const tTJSVariant &value) {
    tTJSVariant v = value;
    d->PropSet(TJS_MEMBERENSURE, name, nullptr, &v, d);
}

u16 str(const tTJSVariant &v) {
    ttstr s(v);
    return u16(s.c_str(), s.GetLen());
}

} // namespace

class ShioriTextRender {
public:
    explicit ShioriTextRender(iTJSDispatch2 *objthis) : self_(objthis) {}

    static tjs_error factory(ShioriTextRender **result, tjs_int, tTJSVariant **, iTJSDispatch2 *objthis) {
        *result = new ShioriTextRender(objthis);
        return TJS_S_OK;
    }

    // ---- setup ----
    void setRenderSize(tjs_int w, tjs_int h) {
        boxWidth_ = w;
        boxHeight_ = h;
    }

    void setDefault(tTJSVariant v) {
        iTJSDispatch2 *d = v.AsObjectNoAddRef();
        if (!d) return;
        readStyle(d, default_);
    }

    void setOption(tTJSVariant v) {
        iTJSDispatch2 *d = v.AsObjectNoAddRef();
        if (!d) return;
        tTJSVariant x;
        if (has(d, TJS_W("following"), x)) following_ = str(x);
        if (has(d, TJS_W("leading"), x)) leading_ = str(x);
        if (has(d, TJS_W("begin"), x)) begin_ = str(x);
        if (has(d, TJS_W("end"), x)) end_ = str(x);
        if (has(d, TJS_W("kinsoku_max"), x)) kinsokuMax_ = tjs_int(x.AsInteger());
        if (has(d, TJS_W("vertical"), x)) vertical_ = x.operator bool();
        if (has(d, TJS_W("word_break"), x)) wordBreak_ = x.operator bool();
        if (has(d, TJS_W("width_time_scale"), x)) widthTimeScale_ = x.AsReal();
    }

    // ---- rendering ----
    void clear() {
        chars_.clear();
        keyWait_.clear();
        state_ = default_;
        x_ = 0;
        y_ = 0;
        indent_ = 0;
        line_ = 0;
        lineStart_ = 0;
        lineTop_ = 0;
        lineHeight_ = 0;
        time_ = 0;
        align_ = -1;
        overflow_ = false;
        pendingIndent_ = false;
        widthCache_.clear();
        fontChanged();
    }

    void resetFont() {
        state_ = default_;
        fontChanged();
    }

    // render(text, autoIndent, diff (ms a character), all (ms for everything, 0: by diff), same)
    bool render(tTJSVariant textValue, tjs_int autoIndent, tjs_int diff, tjs_int all, tTJSVariant) {
        const u16 text = str(textValue);
        autoIndent_ = autoIndent;
        diff_ = std::max<tjs_int>(diff, 0);
        speed_ = 100;
        const size_t firstChar = chars_.size();
        const tjs_int startTime = time_;
        parse(text, 0);
        flushLine();
        if (all > 0 && chars_.size() > firstChar) {
            // Spread the given total over the characters, keeping waits' proportions.
            const tjs_int used = time_ - startTime;
            const double scale = used > 0 ? double(all) / used : 0;
            for (size_t i = firstChar; i < chars_.size(); ++i)
                chars_[i].delay = startTime + tjs_int(std::lround((chars_[i].delay - startTime) * scale));
            for (auto &k : keyWait_)
                if (k.pos >= tjs_int(firstChar)) k.time = startTime + tjs_int(std::lround((k.time - startTime) * scale));
            time_ = startTime + all;
        }
        return !overflow_;
    }

    void done() { flushLine(); }

    void newline() {
        flushLine();
        breakLine();
    }

    // ---- results ----
    tTJSVariant getCharacters(tjs_int start, tjs_int end) {
        iTJSDispatch2 *array = TJSCreateArrayObject();
        tjs_int from = 0, to = tjs_int(chars_.size());
        if (!(start == 0 && end == 0) && end >= start) {
            from = std::clamp<tjs_int>(start, 0, to);
            to = std::clamp<tjs_int>(end, from, to);
        }
        for (tjs_int i = from, n = 0; i < to; ++i, ++n) {
            tTJSVariant ch = serialize(chars_[size_t(i)]);
            array->PropSetByNum(TJS_MEMBERENSURE, n, &ch, array);
        }
        tTJSVariant result(array, array);
        array->Release();
        return result;
    }

    // How many characters show at time ms.
    tjs_int calcShowCount(tjs_int time) {
        tjs_int n = 0;
        for (const Char &c : chars_) {
            if (c.delay > time) break;
            ++n;
        }
        return n;
    }

    tTJSVariant getKeyWait() {
        iTJSDispatch2 *array = TJSCreateArrayObject();
        for (size_t i = 0; i < keyWait_.size(); ++i) {
            iTJSDispatch2 *d = TJSCreateDictionaryObject();
            put(d, TJS_W("pos"), tTJSVariant(keyWait_[i].pos));
            put(d, TJS_W("time"), tTJSVariant(keyWait_[i].time));
            tTJSVariant v(d, d);
            d->Release();
            array->PropSetByNum(TJS_MEMBERENSURE, tjs_int(i), &v, array);
        }
        tTJSVariant result(array, array);
        array->Release();
        return result;
    }

    tjs_int getRenderCount() const { return tjs_int(chars_.size()); }

    // The rendered text as plain text, lines joined with \n (for the backlog).
    tTJSVariant getRenderText() const {
        u16 out;
        int line = chars_.empty() ? 0 : chars_.front().line;
        for (const Char &c : chars_) {
            for (; line < c.line; ++line) out += u'\n';
            if (!c.graph) out += c.text;
        }
        return tTJSVariant(ttstr(out.c_str()));
    }
    tjs_int getRenderDelay() const { return chars_.empty() ? 0 : std::max(time_, chars_.back().delay); }

    tjs_int getRenderLeft() const { return bounds(0); }
    tjs_int getRenderTop() const { return bounds(1); }
    tjs_int getRenderRight() const { return bounds(2); }
    tjs_int getRenderBottom() const { return bounds(3); }

    // ---- properties ----
#define SHIORI_TR_BOOL(name, field)                                                                  \
    bool get_##name() const { return field; }                                                        \
    void set_##name(bool v) { field = v; }
#define SHIORI_TR_INT(name, field)                                                                   \
    tjs_int get_##name() const { return tjs_int(field); }                                            \
    void set_##name(tjs_int v) { field = decltype(field)(v); }
#define SHIORI_TR_FACE(name, field)                                                                  \
    tTJSVariant get_##name() const { return tTJSVariant(ttstr(field.c_str())); }                     \
    void set_##name(tTJSVariant v) { field = str(v); }

    SHIORI_TR_BOOL(vertical, vertical_)
    SHIORI_TR_BOOL(bold, state_.bold)
    SHIORI_TR_BOOL(italic, state_.italic)
    SHIORI_TR_FACE(face, state_.face)
    SHIORI_TR_INT(fontSize, state_.fontSize)
    SHIORI_TR_INT(chColor, state_.chColor)
    SHIORI_TR_INT(rubySize, state_.rubySize)
    SHIORI_TR_INT(rubyOffset, state_.rubyOffset)
    SHIORI_TR_BOOL(shadow, state_.shadow)
    SHIORI_TR_INT(shadowColor, state_.shadowColor)
    SHIORI_TR_BOOL(edge, state_.edge)
    SHIORI_TR_INT(edgeColor, state_.edgeColor)
    SHIORI_TR_INT(lineSpacing, state_.lineSpacing)
    SHIORI_TR_INT(pitch, state_.pitch)
    SHIORI_TR_INT(lineSize, state_.lineSize)
    SHIORI_TR_BOOL(defaultBold, default_.bold)
    SHIORI_TR_BOOL(defaultItalic, default_.italic)
    SHIORI_TR_FACE(defaultFace, default_.face)
    SHIORI_TR_INT(defaultFontSize, default_.fontSize)
    SHIORI_TR_INT(defaultChColor, default_.chColor)
    SHIORI_TR_INT(defaultRubySize, default_.rubySize)
    SHIORI_TR_INT(defaultRubyOffset, default_.rubyOffset)
    SHIORI_TR_BOOL(defaultShadow, default_.shadow)
    SHIORI_TR_INT(defaultShadowColor, default_.shadowColor)
    SHIORI_TR_BOOL(defaultEdge, default_.edge)
    SHIORI_TR_INT(defaultEdgeColor, default_.edgeColor)
    SHIORI_TR_INT(defaultLineSpacing, default_.lineSpacing)
    SHIORI_TR_INT(defaultPitch, default_.pitch)
    SHIORI_TR_INT(defaultLineSize, default_.lineSize)
    tjs_real get_fontScale() const { return fontScale_; }
    void set_fontScale(tjs_real v) { fontScale_ = v > 0 ? v : 1; }

private:
    struct KeyWait {
        tjs_int pos, time;
    };

    // ---- the script's side ----
    double textWidth(const u16 &text, tjs_int size) {
        const u16 key = text + u'\x01' + state_.face + (state_.bold ? u"b" : u"") + std::u16string(1, char16_t(size));
        auto found = widthCache_.find(key);
        if (found != widthCache_.end()) return found->second;
        double w = double(size);
        tTJSVariant name, sz(size), result;
        name = ttstr(text.c_str());
        tTJSVariant *params[] = {&name, &sz};
        if (TJS_SUCCEEDED(self_->FuncCall(0, TJS_W("onGetTextWidth"), nullptr, &result, 2, params, self_)) &&
            result.Type() != tvtVoid)
            w = result.AsReal();
        widthCache_[key] = w;
        return w;
    }

    u16 evalText(const u16 &name) {
        tTJSVariant n(ttstr(name.c_str())), result;
        tTJSVariant *params[] = {&n};
        if (TJS_SUCCEEDED(self_->FuncCall(0, TJS_W("onEval"), nullptr, &result, 1, params, self_)) &&
            result.Type() != tvtVoid)
            return str(result);
        return u16();
    }

    tjs_int labelTime(const u16 &name) {
        tTJSVariant n(ttstr(name.c_str())), result;
        tTJSVariant *params[] = {&n};
        if (TJS_SUCCEEDED(self_->FuncCall(0, TJS_W("onLabel"), nullptr, &result, 1, params, self_)) &&
            result.Type() != tvtVoid)
            return tjs_int(result.AsInteger());
        return 0;
    }

    bool graphSize(const u16 &file, double &w, double &h) {
        tTJSVariant n(ttstr(file.c_str())), result;
        tTJSVariant *params[] = {&n};
        if (TJS_FAILED(self_->FuncCall(0, TJS_W("onGetGraphSize"), nullptr, &result, 1, params, self_)) ||
            result.Type() != tvtObject)
            return false;
        iTJSDispatch2 *d = result.AsObjectNoAddRef();
        w = intOf(d, TJS_W("width"), 0);
        h = intOf(d, TJS_W("height"), 0);
        return true;
    }

    void fontChanged() {
        iTJSDispatch2 *d = TJSCreateDictionaryObject();
        put(d, TJS_W("bold"), tTJSVariant(state_.bold ? tjs_int(1) : tjs_int(0)));
        put(d, TJS_W("italic"), tTJSVariant(state_.italic ? tjs_int(1) : tjs_int(0)));
        put(d, TJS_W("face"), tTJSVariant(ttstr(state_.face.c_str())));
        put(d, TJS_W("size"), tTJSVariant(scaled(state_.fontSize)));
        tTJSVariant arg(d, d);
        d->Release();
        tTJSVariant *params[] = {&arg};
        self_->FuncCall(0, TJS_W("onFontChange"), nullptr, nullptr, 1, params, self_);
    }

    static void readStyle(iTJSDispatch2 *d, Style &s) {
        tTJSVariant x;
        if (has(d, TJS_W("bold"), x)) s.bold = x.operator bool();
        if (has(d, TJS_W("italic"), x)) s.italic = x.operator bool();
        if (has(d, TJS_W("face"), x)) s.face = str(x);
        if (has(d, TJS_W("fontSize"), x)) s.fontSize = tjs_int(x.AsInteger());
        if (has(d, TJS_W("chColor"), x)) s.chColor = tjs_uint32(x.AsInteger());
        if (has(d, TJS_W("rubySize"), x)) s.rubySize = tjs_int(x.AsInteger());
        if (has(d, TJS_W("rubyOffset"), x)) s.rubyOffset = tjs_int(x.AsInteger());
        if (has(d, TJS_W("shadow"), x)) s.shadow = x.operator bool();
        if (has(d, TJS_W("shadowColor"), x)) s.shadowColor = tjs_uint32(x.AsInteger());
        if (has(d, TJS_W("edge"), x)) s.edge = x.operator bool();
        if (has(d, TJS_W("edgeColor"), x)) s.edgeColor = tjs_uint32(x.AsInteger());
        if (has(d, TJS_W("lineSpacing"), x)) s.lineSpacing = tjs_int(x.AsInteger());
        if (has(d, TJS_W("pitch"), x)) s.pitch = tjs_int(x.AsInteger());
        if (has(d, TJS_W("lineSize"), x)) s.lineSize = tjs_int(x.AsInteger());
    }

    tjs_int scaled(tjs_int size) const { return tjs_int(std::lround(size * fontScale_)); }

    // ---- markup ----
    static bool readUntil(const u16 &t, size_t &i, char16_t stop, u16 &out) {
        out.clear();
        while (++i < t.size()) {
            if (t[i] == stop) return true;
            out += t[i];
        }
        return false;
    }

    static bool readNumber(const u16 &t, size_t &i, tjs_int &value) {
        // i is on the first digit (or sign); reads up to ';'.
        u16 digits;
        --i;
        if (!readUntil(t, i, u';', digits)) return false;
        value = 0;
        bool negative = false;
        for (char16_t c : digits) {
            if (c == u'-') negative = !negative;
            else if (c >= u'0' && c <= u'9') value = value * 10 + (c - u'0');
        }
        if (negative) value = -value;
        return true;
    }

    void parse(const u16 &t, int depth) {
        for (size_t i = 0; i < t.size(); ++i) {
            const char16_t c = t[i];
            switch (c) {
            case u'\\': {
                if (++i >= t.size()) return;
                switch (t[i]) {
                case u'n': flushLine(); breakLine(); break;
                case u't': pushChar(u"\t"); break;
                case u'i': flushLine(); pendingIndent_ = true; break;
                case u'r': indent_ = 0; break;
                case u'w': pushChar(u" "); break;
                case u'k': flushLine(); keyWait_.push_back({tjs_int(chars_.size() + pending_.size()), time_}); break;
                case u'x': break;
                default: pushChar(u16(1, t[i])); break;
                }
                break;
            }
            case u'[': {
                u16 body;
                if (!readUntil(t, i, u']', body)) return;
                size_t comma = body.rfind(u',');
                int count = 1;
                u16 ruby = body;
                if (comma != u16::npos) {
                    bool digits = comma + 1 < body.size();
                    for (size_t k = comma + 1; k < body.size(); ++k) digits = digits && body[k] >= u'0' && body[k] <= u'9';
                    if (digits) {
                        count = 0;
                        for (size_t k = comma + 1; k < body.size(); ++k) count = count * 10 + (body[k] - u'0');
                        ruby = body.substr(0, comma);
                    }
                }
                rubyText_ = ruby;
                rubyCount_ = std::max(count, 1);
                break;
            }
            case u'%': {
                if (++i >= t.size()) return;
                const char16_t k = t[i];
                switch (k) {
                case u'f': {
                    u16 face;
                    if (!readUntil(t, i, u';', face)) return;
                    state_.face = face.empty() ? default_.face : face; // %f; is the default face
                    fontChanged();
                    break;
                }
                case u'b': case u'i': case u's': case u'e': {
                    if (++i >= t.size()) return;
                    const char16_t v = t[i];
                    bool *field = k == u'b' ? &state_.bold : k == u'i' ? &state_.italic : k == u's' ? &state_.shadow : &state_.edge;
                    const bool *def = k == u'b' ? &default_.bold : k == u'i' ? &default_.italic : k == u's' ? &default_.shadow : &default_.edge;
                    *field = v == u'1' ? true : v == u'0' ? false : *def;
                    if (k == u'b' || k == u'i') fontChanged();
                    break;
                }
                case u'B': state_.fontSize = default_.fontSize * 3 / 2; fontChanged(); break;
                case u'S': state_.fontSize = default_.fontSize * 2 / 3; fontChanged(); break;
                case u'r': state_ = default_; fontChanged(); break;
                case u'C': align_ = 0; break;
                case u'R': align_ = 1; break;
                case u'L': align_ = -1; break;
                case u'p': {
                    ++i;
                    tjs_int v;
                    if (!readNumber(t, i, v)) return;
                    state_.pitch = v;
                    break;
                }
                case u'd': {
                    ++i;
                    tjs_int v;
                    if (!readNumber(t, i, v)) return;
                    speed_ = v;
                    break;
                }
                case u'w': {
                    ++i;
                    tjs_int v;
                    if (!readNumber(t, i, v)) return;
                    time_ += diff_ * v / 100;
                    break;
                }
                case u'D': {
                    if (i + 1 < t.size() && t[i + 1] == u'$') {
                        ++i;
                        u16 label;
                        if (!readUntil(t, i, u';', label)) return;
                        time_ = std::max(time_, labelTime(label));
                    } else {
                        ++i;
                        tjs_int v;
                        if (!readNumber(t, i, v)) return;
                        time_ = std::max(time_, v);
                    }
                    break;
                }
                default:
                    if ((k >= u'0' && k <= u'9') || k == u'-') {
                        tjs_int v;
                        if (!readNumber(t, i, v)) return;
                        state_.fontSize = default_.fontSize * v / 100;
                        fontChanged();
                    } else {
                        pushChar(u16(1, k));
                    }
                    break;
                }
                break;
            }
            case u'#': {
                u16 hex;
                if (!readUntil(t, i, u';', hex)) return;
                tjs_uint32 color = 0;
                for (char16_t h : hex) {
                    color <<= 4;
                    if (h >= u'0' && h <= u'9') color |= h - u'0';
                    else if (h >= u'a' && h <= u'f') color |= 10 + h - u'a';
                    else if (h >= u'A' && h <= u'F') color |= 10 + h - u'A';
                }
                state_.chColor = hex.size() > 6 ? color : (color & 0xffffff);
                break;
            }
            case u'&': {
                u16 file;
                if (!readUntil(t, i, u';', file)) return;
                pushGraph(file);
                break;
            }
            case u'$': {
                u16 name;
                if (!readUntil(t, i, u';', name)) return;
                if (depth < 4) parse(evalText(name), depth + 1);
                break;
            }
            case u'\n':
                flushLine();
                breakLine();
                break;
            case u'\r':
                break;
            default: {
                // A surrogate pair is one character.
                if (c >= 0xD800 && c <= 0xDBFF && i + 1 < t.size()) {
                    pushChar(t.substr(i, 2));
                    ++i;
                } else {
                    pushChar(u16(1, c));
                }
                break;
            }
            }
        }
    }

    // ---- layout ----
    // Characters go into pending_ for the current line; a line is placed (aligned, its
    // characters' line top fixed) when it ends.
    void pushChar(const u16 &text) {
        Char c;
        c.text = text;
        c.size = scaled(state_.fontSize);
        c.cw = text == u"\t" ? c.size * 2 : textWidth(text, c.size);
        place(c);
    }

    void pushGraph(const u16 &file) {
        Char c;
        c.text = file;
        c.graph = true;
        double w = 0, h = 0;
        if (!graphSize(file, w, h)) {
            w = scaled(state_.fontSize);
            h = w;
        }
        c.cw = w;
        c.size = tjs_int(h);
        place(c);
    }

    bool isIn(const u16 &set, const u16 &text) const { return text.size() == 1 && set.find(text[0]) != u16::npos; }

    void place(Char &c) {
        c.bold = state_.bold;
        c.italic = state_.italic;
        c.face = state_.face;
        c.vertical = vertical_;
        c.color = state_.chColor;
        c.shadow = state_.shadow;
        c.shadowColor = state_.shadowColor;
        c.edge = state_.edge;
        c.edgeColor = state_.edgeColor;
        if (rubyCount_ > 0 && !rubyText_.empty()) {
            // The ruby rides on the first of the characters it spans; widened when there.
            Ruby r;
            r.text = rubyText_;
            r.size = scaled(state_.rubySize);
            c.ruby.push_back(r);
            rubySpan_ = rubyCount_;
            rubyText_.clear();
            rubyCharIndex_ = pending_.size();
        }
        if (rubySpan_ > 0) {
            rubySpanChars_.push_back(pending_.size());
            if (--rubySpan_ == 0) layoutRuby();
        }

        if (pendingIndent_) {
            indent_ = x_;
            pendingIndent_ = false;
        }

        const double advance = c.cw + state_.pitch;
        const bool atLineStart = pending_.empty() && x_ <= indent_;
        // Hanging indent for an opening bracket at the start of a line (negative autoIndent).
        if (atLineStart && autoIndent_ < 0 && isIn(begin_, c.text)) x_ -= advance;

        if (boxWidth_ > 0 && x_ + c.cw > boxWidth_ && !pending_.empty()) {
            // Doesn't fit: break the line, keeping kinsoku and words together.
            wrapBefore(c);
        }
        c.x = x_;
        c.delay = time_;
        time_ += charTime(c);
        c.line = line_;
        lineHeight_ = std::max<double>(lineHeight_, c.size);
        x_ += advance;
        pending_.push_back(c);

        if (autoIndent_ && isIn(begin_, c.text)) indent_ = x_;
        if (isIn(end_, c.text)) indent_ = 0;
    }

    tjs_int charTime(const Char &c) const {
        double t = double(diff_) * speed_ / 100.0;
        if (widthTimeScale_ > 0 && c.size > 0) t *= std::max(0.25, c.cw / c.size) * widthTimeScale_;
        return tjs_int(std::lround(t));
    }

    void layoutRuby() {
        if (rubyCharIndex_ >= pending_.size() || pending_[rubyCharIndex_].ruby.empty()) {
            rubySpanChars_.clear();
            return;
        }
        Char &first = pending_[rubyCharIndex_];
        Ruby &r = first.ruby.back();
        double span = 0;
        for (size_t k : rubySpanChars_) span += pending_[k].cw;
        const double width = textWidth(r.text, r.size);
        // Centred over the base characters, from the first one's position.
        r.x = (span - width) / 2;
        r.y = -(r.size) + state_.rubyOffset;
        rubySpanChars_.clear();
    }

    // Moves the tail of the line that must go with c to the next line.
    void wrapBefore(Char &c) {
        size_t keep = pending_.size(); // characters that stay on this line
        int limit = std::max<tjs_int>(kinsokuMax_, 1);
        // A line may not end with a leading (opening) character, and the next may not begin
        // with a following (closing) one.
        while (keep > 0 && limit-- > 0) {
            const bool closesNext = keep == pending_.size() ? isIn(following_, c.text) : isIn(following_, pending_[keep].text);
            const bool opensHere = isIn(leading_, pending_[keep - 1].text);
            if (!closesNext && !opensHere) break;
            --keep;
        }
        if (wordBreak_) {
            // Western text breaks at spaces: carry the current word over.
            size_t k = keep;
            while (k > 0 && pending_[k - 1].text != u" ") --k;
            if (k > 0) keep = k;
        }
        if (keep == 0) keep = pending_.size(); // nothing can move: break here
        std::vector<Char> carry(pending_.begin() + keep, pending_.end());
        pending_.erase(pending_.begin() + keep, pending_.end());
        // Trailing spaces don't count at a soft break.
        flushLine();
        breakLine();
        for (Char &m : carry) {
            if (m.text == u" " && pending_.empty() && wordBreak_) continue;
            m.x = x_;
            x_ += m.cw + state_.pitch;
            lineHeight_ = std::max<double>(lineHeight_, m.size);
            m.line = line_;
            pending_.push_back(m);
        }
        if (wordBreak_ && c.text == u" " && pending_.empty()) c.cw = c.cw; // a space may lead; kept simple
    }

    void flushLine() {
        if (pending_.empty()) return;
        double height = lineHeight_;
        if (state_.lineSize > 0) height = std::max<double>(height, state_.lineSize);
        double shift = 0;
        if (align_ >= 0 && boxWidth_ > 0) {
            const Char &last = pending_.back();
            const double width = last.x + last.cw;
            shift = align_ == 0 ? (boxWidth_ - width) / 2 : boxWidth_ - width;
        }
        for (Char &c : pending_) {
            c.x += shift;
            // Characters sit on the line's baseline: smaller ones lower.
            c.y = lineTop_ + (height - c.size);
            if (vertical_) {
                const double x = c.x;
                c.x = boxWidth_ - c.y - height;
                c.y = x;
            }
            for (Ruby &r : c.ruby) {
                if (vertical_) std::swap(r.x, r.y);
            }
            chars_.push_back(c);
        }
        pending_.clear();
        lineHeight_ = std::max<double>(height, 0);
        lastLineHeight_ = lineHeight_;
    }

    void breakLine() {
        const double height = lastLineHeight_ > 0 ? lastLineHeight_ : scaled(state_.fontSize);
        lineTop_ += height + state_.lineSpacing;
        if (boxHeight_ > 0 && lineTop_ + height > boxHeight_) overflow_ = true;
        x_ = indent_;
        lineHeight_ = 0;
        lastLineHeight_ = 0;
        ++line_;
    }

    tjs_int bounds(int which) const {
        if (chars_.empty()) return 0;
        double l = 1e9, t = 1e9, r = -1e9, b = -1e9;
        for (const Char &c : chars_) {
            l = std::min(l, c.x);
            t = std::min(t, c.y);
            r = std::max(r, c.x + c.cw);
            b = std::max(b, c.y + c.size);
        }
        const double v[4] = {l, t, r, b};
        return tjs_int(std::lround(v[which]));
    }

    tTJSVariant serialize(const Char &c) const {
        iTJSDispatch2 *d = TJSCreateDictionaryObject();
        put(d, TJS_W("text"), tTJSVariant(ttstr(c.text.c_str())));
        put(d, TJS_W("x"), tTJSVariant(tjs_int(std::lround(c.x))));
        put(d, TJS_W("y"), tTJSVariant(tjs_int(std::lround(c.y))));
        put(d, TJS_W("cw"), tTJSVariant(tjs_int(std::lround(c.cw))));
        put(d, TJS_W("size"), tTJSVariant(c.size));
        put(d, TJS_W("face"), tTJSVariant(ttstr(c.face.c_str())));
        put(d, TJS_W("bold"), tTJSVariant(tjs_int(c.bold)));
        put(d, TJS_W("italic"), tTJSVariant(tjs_int(c.italic)));
        put(d, TJS_W("graph"), tTJSVariant(tjs_int(c.graph)));
        put(d, TJS_W("vertical"), tTJSVariant(tjs_int(c.vertical)));
        put(d, TJS_W("color"), tTJSVariant(tjs_int(c.color)));
        put(d, TJS_W("shadow"), tTJSVariant(tjs_int(c.shadow)));
        put(d, TJS_W("shadowColor"), tTJSVariant(tjs_int(c.shadowColor)));
        put(d, TJS_W("edge"), tTJSVariant(tjs_int(c.edge)));
        put(d, TJS_W("edgeColor"), tTJSVariant(tjs_int(c.edgeColor)));
        put(d, TJS_W("delay"), tTJSVariant(c.delay));
        if (!c.ruby.empty()) {
            const Ruby &r = c.ruby.front();
            iTJSDispatch2 *rd = TJSCreateDictionaryObject();
            put(rd, TJS_W("text"), tTJSVariant(ttstr(r.text.c_str())));
            put(rd, TJS_W("x"), tTJSVariant(tjs_int(std::lround(r.x))));
            put(rd, TJS_W("y"), tTJSVariant(tjs_int(std::lround(r.y))));
            put(rd, TJS_W("size"), tTJSVariant(r.size));
            tTJSVariant rv(rd, rd);
            rd->Release();
            put(d, TJS_W("ruby"), rv);
        }
        tTJSVariant v(d, d);
        d->Release();
        return v;
    }

    iTJSDispatch2 *self_ = nullptr;
    Style default_, state_;
    tjs_int boxWidth_ = 0, boxHeight_ = 0;
    u16 following_, leading_, begin_ = u"「『（‘“〔［｛〈《", end_ = u"」』）’”〕］｝〉》";
    tjs_int kinsokuMax_ = 3;
    bool vertical_ = false, wordBreak_ = false;
    double widthTimeScale_ = 0, fontScale_ = 1;

    std::vector<Char> chars_, pending_;
    std::vector<KeyWait> keyWait_;
    double x_ = 0, y_ = 0, indent_ = 0, lineTop_ = 0, lineHeight_ = 0, lastLineHeight_ = 0;
    int line_ = 0;
    size_t lineStart_ = 0;
    tjs_int autoIndent_ = 0, diff_ = 0, speed_ = 100, time_ = 0, align_ = -1;
    bool overflow_ = false, pendingIndent_ = false;
    u16 rubyText_;
    int rubyCount_ = 0, rubySpan_ = 0;
    size_t rubyCharIndex_ = 0;
    std::vector<size_t> rubySpanChars_;
    std::map<u16, double> widthCache_;
};

static tjs_error ShioriTextRenderFactory(ShioriTextRender **result, tjs_int count, tTJSVariant **param,
                                         iTJSDispatch2 *objthis) {
    return ShioriTextRender::factory(result, count, param, objthis);
}

#define SHIORI_TR_PROPERTY(name) NCB_PROPERTY(name, get_##name, set_##name)

NCB_REGISTER_CLASS_DIFFER(TextRenderBase, ShioriTextRender) {
    RawCallback(ShioriTextRenderFactory);
    NCB_METHOD(render);
    NCB_METHOD(setRenderSize);
    NCB_METHOD(setDefault);
    NCB_METHOD(setOption);
    NCB_METHOD(getCharacters);
    NCB_METHOD(clear);
    NCB_METHOD(done);
    NCB_METHOD(newline);
    NCB_METHOD(resetFont);
    NCB_METHOD(calcShowCount);
    NCB_METHOD(getKeyWait);
    NCB_PROPERTY_RO(renderCount, getRenderCount);
    NCB_PROPERTY_RO(renderText, getRenderText);
    NCB_PROPERTY_RO(renderDelay, getRenderDelay);
    NCB_PROPERTY_RO(renderLeft, getRenderLeft);
    NCB_PROPERTY_RO(renderTop, getRenderTop);
    NCB_PROPERTY_RO(renderRight, getRenderRight);
    NCB_PROPERTY_RO(renderBottom, getRenderBottom);
    SHIORI_TR_PROPERTY(vertical);
    SHIORI_TR_PROPERTY(bold);
    SHIORI_TR_PROPERTY(italic);
    SHIORI_TR_PROPERTY(face);
    SHIORI_TR_PROPERTY(fontSize);
    SHIORI_TR_PROPERTY(chColor);
    SHIORI_TR_PROPERTY(rubySize);
    SHIORI_TR_PROPERTY(rubyOffset);
    SHIORI_TR_PROPERTY(shadow);
    SHIORI_TR_PROPERTY(shadowColor);
    SHIORI_TR_PROPERTY(edge);
    SHIORI_TR_PROPERTY(edgeColor);
    SHIORI_TR_PROPERTY(lineSpacing);
    SHIORI_TR_PROPERTY(pitch);
    SHIORI_TR_PROPERTY(lineSize);
    SHIORI_TR_PROPERTY(defaultBold);
    SHIORI_TR_PROPERTY(defaultItalic);
    SHIORI_TR_PROPERTY(defaultFace);
    SHIORI_TR_PROPERTY(defaultFontSize);
    SHIORI_TR_PROPERTY(defaultChColor);
    SHIORI_TR_PROPERTY(defaultRubySize);
    SHIORI_TR_PROPERTY(defaultRubyOffset);
    SHIORI_TR_PROPERTY(defaultShadow);
    SHIORI_TR_PROPERTY(defaultShadowColor);
    SHIORI_TR_PROPERTY(defaultEdge);
    SHIORI_TR_PROPERTY(defaultEdgeColor);
    SHIORI_TR_PROPERTY(defaultLineSpacing);
    SHIORI_TR_PROPERTY(defaultPitch);
    SHIORI_TR_PROPERTY(defaultLineSize);
    SHIORI_TR_PROPERTY(fontScale);
}
