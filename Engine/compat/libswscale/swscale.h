/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Shiori's FFmpeg is built without libswscale; krkr2's software render manager only names the
// type (its swscale path is compiled out, USE_SWSCALE is off).
#pragma once
struct SwsContext;
