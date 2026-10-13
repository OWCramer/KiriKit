/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// KAGParserEx.dll for ShioriKrkr: wamsoft's KAGParserEx (github.com/wamsoft/KAGParserEx,
// KiriKiri licence, by miahmie and wtnbgo), built in. Linking the DLL name replaces the global
// KAGParser class with it, as the DLL does on Windows. Games written for it need what krkr2's
// own parser lacks: [emb] with escape=false (Sekai Project's KAG reads its configuration
// scenarios through it), multi-line tags, parameter macros.
//
// Its classes and messages are renamed here so they sit beside krkr2's own KAGParser.
#include "tjsCommHead.h"
#include "EventIntf.h"
#include "tjsGlobalStringMap.h"

// It registers its members the plug-in way (on the class object it makes).
#undef TJS_NCM_REG_THIS
#define TJS_NCM_REG_THIS classobj
#undef TJS_NATIVE_SET_ClassID
#define TJS_NATIVE_SET_ClassID TJS_NATIVE_CLASSID_NAME = TJS_NCM_CLASSID;
#undef TJS_NATIVE_CLASSID_NAME

#define tTJSNI_KAGParser tTJSNI_ShioriKAGParserEx
#define tTJSNC_KAGParser tTJSNC_ShioriKAGParserEx
#define tTVPScenarioCacheItem tTVPShioriKAGExScenarioCacheItem
#define tTVPCharHolder tTVPShioriKAGExCharHolder
#define TVPClearScnearioCache TVPShioriKAGExClearScenarioCache
#define tTVPClearScenarioCacheCallback tTVPShioriKAGExClearScenarioCacheCallback
#define TVPKAGNoLine TVPShioriKAGExNoLine
#define TVPKAGCannotOmmitFirstLabelName TVPShioriKAGExCannotOmmitFirstLabelName
#define TVPKAGMalformedSaveData TVPShioriKAGExMalformedSaveData
#define TVPKAGLabelNotFound TVPShioriKAGExLabelNotFound
#define TVPLabelOrScriptInMacro TVPShioriKAGExLabelOrScriptInMacro
#define TVPKAGInlineScriptNotEnd TVPShioriKAGExInlineScriptNotEnd
#define TVPKAGSyntaxError TVPShioriKAGExSyntaxError
#define TVPKAGCallStackUnderflow TVPShioriKAGExCallStackUnderflow
#define TVPKAGReturnLostSync TVPShioriKAGExReturnLostSync
#define TVPKAGSpecifyKAGParser TVPShioriKAGExSpecifyKAGParser
#define TVPUnknownMacroName TVPShioriKAGExUnknownMacroName
#define TVPScenarioCache TVPShioriKAGExScenarioCache

#include "KAGParserEx.cpp"

// Once for each game (its script engine), when the game links KAGParserEx.dll.
void shioriLinkKAGParserEx() {
    tTJSNI_KAGParser::initMethod();
    iTJSDispatch2 *global = TVPGetScriptDispatch();
    if (!global) return;
    iTJSDispatch2 *parser = tTJSNC_KAGParser::CreateNativeClass();
    tTJSVariant value(parser);
    parser->Release();
    global->PropSet(TJS_MEMBERENSURE, TVP_KAGPARSER_EX_CLASSNAME, nullptr, &value, global);
    global->Release();
}
