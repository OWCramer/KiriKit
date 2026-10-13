/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

#pragma once

#include "shiorikrkr.h"

#ifdef __cplusplus
#include <cstdint>
#include <string>
#include <vector>

#include "tjsCommHead.h"

namespace shiorikrkr {

// The host's E-mote player (shiorikrkr_config_t.emote), or NULL to draw no characters.
void setEmoteHost(const shiorikrkr_emote_host_t *host);
/// Counts games started in this process: script objects cached by Shiori's code belong to one
/// game's script engine and are made again when this changes.
unsigned sessionGeneration();
const shiorikrkr_emote_host_t *emoteHost();

namespace util {
std::string utf8(const ttstr &s);
// A storage's bytes by the name the game gives it (found through the auto paths).
bool readStorage(const ttstr &name, std::vector<uint8_t> &out, std::string &error);
double number(tTJSVariant **param, tjs_int count, tjs_int index, double fallback = 0);
std::string text(tTJSVariant **param, tjs_int count, tjs_int index);
} // namespace util

} // namespace shiorikrkr
#endif
