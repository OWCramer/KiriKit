/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// What built-in plug-ins take from wamsoft's simplebinder.hpp: the names of their link entry
// points when linked statically (onV2Link_<plug-in>). shiori_plugin_links.cpp calls them.
#pragma once

#define SHIORI_SB_CAT(a, b) a##b
#define SHIORI_SB_XCAT(a, b) SHIORI_SB_CAT(a, b)
#define ONV2LINK SHIORI_SB_XCAT(onV2Link_, TVP_PLUGIN_NAME)
#define ONV2UNLINK SHIORI_SB_XCAT(onV2Unlink_, TVP_PLUGIN_NAME)
#define ONV2DETACH SHIORI_SB_XCAT(onV2Detach_, TVP_PLUGIN_NAME)
