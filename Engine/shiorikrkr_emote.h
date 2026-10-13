/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

#pragma once

#include "shiorikrkr.h"

namespace shiorikrkr {

// The host's E-mote player (shiorikrkr_config_t.emote), or NULL to draw no characters.
void setEmoteHost(const shiorikrkr_emote_host_t *host);
const shiorikrkr_emote_host_t *emoteHost();

} // namespace shiorikrkr
