/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

#pragma once

#include "cx_decrypt.h"

#include <string>

namespace shiorikrkr {

/// When `scheme` is an Hx scheme (SHCX v2 with an Hx section), hands krkr2's XP3 reader the
/// Hx index and member decryption and returns true; `line` says what was loaded (or, on
/// false, why a malformed Hx section was refused; empty for a plain CxDec scheme).
bool hxInstall(const CxScheme &scheme, void (*log)(const std::string &), std::string &line);

/// Takes them away again (the game ended).
void hxUninstall();

} // namespace shiorikrkr
