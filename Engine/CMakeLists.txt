# This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
# If a copy of the MPL was not distributed with this file, You can obtain one at
# https://mozilla.org/MPL/2.0/.
#
# ShioriKrkr: Shiori's own KiriKiri engine. krkr2's core (Vendor/krkr2 at the commit
# AetherKrkr forked from, KiriKiri licence) with Shiori's own environment (window, input,
# audio, Metal compositor, host interface), as one dynamic library that exports only
# shiorikrkr.h. Built per slice by Scripts/krkr2/build-framework.sh; on macOS it also builds
# the harness. Which krkr2 files are left out and why: Packages/ShioriKrkrKit/PROVENANCE.md.
cmake_minimum_required(VERSION 3.28)
project(ShioriKrkr LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_OBJC_VISIBILITY_PRESET hidden)
set(CMAKE_OBJCXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

set(VENDOR "${CMAKE_CURRENT_SOURCE_DIR}/../../../Vendor" CACHE PATH "Shiori's Vendor folder")
set(KRKR2 "${VENDOR}/krkr2/cpp")
set(CORE "${KRKR2}/core")
set(SDL3PLUGINS "${VENDOR}/krkrsdl3/plugins")
set(HERE "${CMAKE_CURRENT_SOURCE_DIR}")

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(IOS TRUE)
else()
    set(MACOS TRUE)
endif()

# ---- TJS2's parsers (bison 3.8) and date word map, generated at configure time ----
set(GEN "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GEN}")
find_program(BISON NAMES bison PATHS /opt/homebrew/opt/bison/bin NO_DEFAULT_PATH)
if(NOT BISON)
    find_program(BISON NAMES bison REQUIRED)
endif()
set(PARSERS)
foreach(grammar tjs tjsdate tjspp)
    add_custom_command(OUTPUT "${GEN}/${grammar}.tab.cpp" "${GEN}/${grammar}.tab.hpp"
        COMMAND "${BISON}" "${CORE}/tjs2/bison/${grammar}.y"
        DEPENDS "${CORE}/tjs2/bison/${grammar}.y"
        WORKING_DIRECTORY "${GEN}")
    list(APPEND PARSERS "${GEN}/${grammar}.tab.cpp")
endforeach()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT "${GEN}/tjsDateWordMap.inc"
    COMMAND "${Python3_EXECUTABLE}" -I create_world_map.py "${GEN}/tjsDateWordMap.inc"
    DEPENDS "${CORE}/tjs2/script/create_world_map.py"
    WORKING_DIRECTORY "${CORE}/tjs2/script")

# ---- krkr2 core ----
set(TJS2_SOURCES
    tjsLex tjsNative tjsRandomGenerator tjsDebug tjsRegExp tjsBinarySerializer tjsInterCodeGen
    tjsObject tjsConstArrayData tjsUtils tjsException tjsDictionary tjsInterCodeExec
    tjsVariantString tjsScriptBlock tjsMath tjs tjsMessage tjsDisassemble tjsDate tjsError
    tjsInterface tjsMT19937ar-cok tjsArray tjsByteCodeLoader tjsDateParser tjsGlobalStringMap
    tjsOctPack tjsNamespace tjsScriptCache tjsConfig tjsCompileControl tjsObjectExtendable
    tjsVariant tjsString)
list(TRANSFORM TJS2_SOURCES PREPEND "${CORE}/tjs2/")
list(TRANSFORM TJS2_SOURCES APPEND ".cpp")

# Left out of base: the 7-Zip, TAR and ZIP archive readers (krkr2's loose-package support;
# Shiori's games are XP3, and they pull in 7-Zip and libarchive).
set(BASE_SOURCES
    BinaryStream CharacterSet EventIntf ScriptMgnIntf StorageIntf SysInitIntf SystemIntf
    TextStream UtilStreams XP3Archive KAGParser MsgIntf
    impl/EventImpl impl/FileSelector impl/NativeEventQueue impl/ScriptMgnImpl impl/StorageImpl
    impl/SysInitImpl impl/SystemImpl impl/MsgImpl)
list(TRANSFORM BASE_SOURCES PREPEND "${CORE}/base/")
list(TRANSFORM BASE_SOURCES APPEND ".cpp")

# Left out of visual: the cocos2d-x OpenGL renderer (ogl/), JPEG XR, PVR and BPG loaders.
set(VISUAL_SOURCES
    LoadWEBP TransIntf MenuItemIntf SaveTLG5 FontSystem RectItf VideoOvlIntf LayerBitmapIntf
    FontImpl RenderManager LoadJPEG GraphicsLoaderIntf SaveTLG6 FreeType ImageFunction tvpgl
    LoadTLG LayerTreeOwnerImpl GraphicsLoadThread LayerManager PrerenderedFont WindowIntf
    FreeTypeFontRasterizer LoadPNG LayerIntf ComplexRect CharacterData BitmapLayerTreeOwner
    BitmapIntf argb
    gl/blend_function gl/ResampleImage gl/WeightFunctor
    impl/BasicDrawDevice impl/BitmapBitsAlloc impl/BitmapInfomation impl/DInputMgn
    impl/DrawDevice impl/GraphicsLoaderImpl impl/LayerBitmapImpl impl/LayerImpl
    impl/MenuItemImpl impl/PassThroughDrawDevice impl/VideoOvlImpl
    impl/WindowImpl)
list(TRANSFORM VISUAL_SOURCES PREPEND "${CORE}/visual/")
list(TRANSFORM VISUAL_SOURCES APPEND ".cpp")

# Left out of sound: WaveMixer (OpenAL/SDL2; Shiori's audio output replaces it).
set(SOUND_SOURCES
    CDDAIntf MIDIIntf PhaseVocoderDSP PhaseVocoderFilter
    SoundBufferBaseIntf VorbisWaveDecoder WaveFormatConverter WaveIntf WaveLoopManager
    WaveSegmentQueue win32/CDDAImpl win32/MIDIImpl win32/SoundBufferBaseImpl win32/tvpsnd
    win32/WaveImpl)
list(TRANSFORM SOUND_SOURCES PREPEND "${CORE}/sound/")
list(TRANSFORM SOUND_SOURCES APPEND ".cpp")

set(UTILS_SOURCES
    ClipboardIntf.cpp DebugIntf.cpp MathAlgorithms_Default.cpp md5.c MiscUtility.cpp PadIntf.cpp
    Random.cpp RealFFT_Default.cpp ThreadIntf.cpp TickCount.cpp TimerIntf.cpp VelocityTracker.cpp
    encoding/gbk2unicode.c encoding/jis2unicode.c win32/ClipboardImpl.cpp win32/DebugImpl.cpp
    win32/PadImpl.cpp win32/ThreadImpl.cpp win32/TimerImpl.cpp win32/TVPTimer.cpp)
list(TRANSFORM UTILS_SOURCES PREPEND "${CORE}/utils/")

set(OTHER_CORE_SOURCES
    "${CORE}/plugin/ncbind.cpp" "${CORE}/plugin/PluginIntf.cpp" "${CORE}/plugin/PluginImpl.cpp"
    "${CORE}/extension/Extension.cpp"
    "${CORE}/environ/Application.cpp" "${CORE}/environ/DetectCPU.cpp"
    "${CORE}/environ/win32/SystemControl.cpp")

# ---- plug-ins (built in; Plugins.link finds them by their DLL names) ----
# krkr2's own (KiriKiri licence). Left out: layerex_draw (libgdiplus; krkrsdl3's plutovg one
# below instead), psbfile (FreeMote layout), motionplayer (closed-binary lineage), psdfile
# (Boost.Spirit), steam, layerExMovie (Kodi movie player).
set(KRKR2_PLUGIN_SOURCES
    scriptsEx win32dialog dirlist csvParser varfile saveStruct getabout addFont wutcwf windowEx
    getSample layerExPerspective fftgraph LayerExBase xp3filter fstat/main)
list(TRANSFORM KRKR2_PLUGIN_SOURCES PREPEND "${KRKR2}/plugins/")
list(TRANSFORM KRKR2_PLUGIN_SOURCES APPEND ".cpp")
# wamsoft's originals (KiriKiri licence; Vendor/upstream-plugins, pinned in fetch.sh), built in
# as static plug-ins: each file is compiled with
# its plug-in name, and shiori_plugin_links.cpp runs their link entry points when a game links
# the DLL name.
set(UPSTREAM "${VENDOR}/upstream-plugins")
set(PLUGIN_SOURCES)
function(shiori_plugin name dll dir)
    set(files ${ARGN})
    list(TRANSFORM files PREPEND "${dir}/")
    set_source_files_properties(${files} PROPERTIES
        COMPILE_DEFINITIONS "TVP_STATIC_PLUGIN;TVP_PLUGIN_NAME=${name};NCB_MODULE_NAME=TJS_W(\"${dll}\")"
        INCLUDE_DIRECTORIES "${dir}"
        COMPILE_OPTIONS "-include;${HERE}/compat/plugins/shiori_plugin_prefix.h")
    set(PLUGIN_SOURCES ${PLUGIN_SOURCES} ${files} PARENT_SCOPE)
endfunction()
# KAGParserEx (wamsoft): krkr2's core parser lacks some of it ([emb] escape=false, multi-line
# tags, parameter macros); plugins/shiori_kagparserex.cpp builds it in under other names.
set_source_files_properties("${HERE}/plugins/shiori_kagparserex.cpp" PROPERTIES
    INCLUDE_DIRECTORIES "${UPSTREAM}/wamsoft/KAGParserEx"
    COMPILE_OPTIONS "-include;${HERE}/compat/plugins/shiori_plugin_prefix.h")
set(PLUGIN_SOURCES ${PLUGIN_SOURCES} "${HERE}/plugins/shiori_kagparserex.cpp")
shiori_plugin(extrans extrans.dll "${UPSTREAM}/wamsoft/extrans" Main.cpp wave.cpp mosaic.cpp turn.cpp
    turntrans_table.cpp rotatebase.cpp rotatetrans.cpp ripple.cpp)
shiori_plugin(AlphaMovie AlphaMovie.dll "${UPSTREAM}/wamsoft/AlphaMovie" AlphaMovie.cpp)
shiori_plugin(layerExAreaAverage layerExAreaAverage.dll "${UPSTREAM}/wamsoft/layerExAreaAverage" main.cpp)
shiori_plugin(layerExBTOA layerExBTOA.dll "${UPSTREAM}/wamsoft/layerExBTOA" Main.cpp)
shiori_plugin(layerExImage layerExImage.dll "${UPSTREAM}/wamsoft/layerExImage" LayerExImage.cpp Main.cpp LicensesGen.cpp)
shiori_plugin(layerExRaster layerExRaster.dll "${UPSTREAM}/wamsoft/layerExRaster" main.cpp)
shiori_plugin(shrinkCopy shrinkCopy.dll "${UPSTREAM}/wamsoft/shrinkCopy" main.cpp)
# krkrsdl3's (KiriKiri licence plus its publish-your-changes term) for the few with no other
# source: PackinOne, lzfs, gfxEffect, and LayerExDraw on plutovg.
shiori_plugin(PackinOne PackinOne.dll "${SDL3PLUGINS}" PackinOne.cpp)
shiori_plugin(lzfs lzfs.dll "${SDL3PLUGINS}" lzfs.cpp)
shiori_plugin(gfxEffect gfxEffect.dll "${SDL3PLUGINS}" gfxEffect.cpp)
shiori_plugin(layerExDraw layerExDraw.dll "${SDL3PLUGINS}/LayerExDraw" LayerExDraw.cpp)

# ---- Shiori's environment (replaces krkr2's cocos2d-x one) ----
file(GLOB SHIORI_ENVIRON_SOURCES CONFIGURE_DEPENDS "${HERE}/environ/*.cpp" "${HERE}/environ/*.mm")

set(SHIORI_SOURCES "${HERE}/shiorikrkr_host.mm" "${HERE}/cx_decrypt.cpp" "${HERE}/shiori_hx.cpp" "${HERE}/shiori_hx_link.cpp" "${HERE}/nekoworks.cpp" "${HERE}/cursor_decode.mm"
    "${HERE}/render/shiori_metal_render.mm" "${HERE}/render/shiori_layer_gpu.cpp" "${HERE}/shiorikrkr_movie.cpp")
set(SOURCES ${SHIORI_SOURCES} ${PARSERS} "${GEN}/tjsDateWordMap.inc" ${TJS2_SOURCES} ${BASE_SOURCES} ${VISUAL_SOURCES}
    ${SOUND_SOURCES} ${UTILS_SOURCES} ${OTHER_CORE_SOURCES} ${SHIORI_ENVIRON_SOURCES}
    ${KRKR2_PLUGIN_SOURCES} ${PLUGIN_SOURCES} "${HERE}/getlangname.cpp" "${HERE}/plugins/shiori_steam.cpp" "${HERE}/plugins/shiori_gamepad.cpp" "${HERE}/plugins/shiori_wavefilters.cpp" "${HERE}/plugins/shiori_windows_standins.cpp" "${HERE}/shiori_plugin_links.cpp"
    "${HERE}/shiorikrkr_emote.cpp" "${HERE}/shiorikrkr_motion.cpp" "${HERE}/shiorikrkr_motionplayer.cpp" "${HERE}/shiorikrkr_psbfile.cpp" "${HERE}/datapack.cpp" "${HERE}/shiori_prefetch.cpp"
    "${HERE}/plugins/shiori_textrender.cpp")

option(SHIORIKRKR_DEBUG_THROW "Backtrace every C++ throw (SHIORIKRKR_TRACE_THROW=1)" OFF)
if(SHIORIKRKR_DEBUG_THROW)
    list(APPEND SOURCES "${HERE}/debug_throw.cpp")
endif()

add_library(ShioriKrkr SHARED ${SOURCES})
set_target_properties(ShioriKrkr PROPERTIES OUTPUT_NAME ShioriKrkr)

target_compile_definitions(ShioriKrkr PRIVATE NDEBUG=1 TJS_TEXT_OUT_CRLF __STDC_CONSTANT_MACROS
    USE_UNICODE_FSTRING SHIORIKRKR=1)
if(IOS)
    target_compile_definitions(ShioriKrkr PRIVATE SHIORIKRKR_IOS=1)
else()
    target_compile_definitions(ShioriKrkr PRIVATE SHIORIKRKR_MACOS=1 MACOS=1)
endif()
# Upstream code is written for warnings-off builds; keep the log readable.
target_compile_options(ShioriKrkr PRIVATE -w)
set_source_files_properties(${SHIORI_ENVIRON_SOURCES} "${HERE}/shiorikrkr_host.mm" "${HERE}/cursor_decode.mm"
    "${HERE}/render/shiori_metal_render.mm" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

target_include_directories(ShioriKrkr PRIVATE
    "${HERE}/compat" "${HERE}/compat/plugins"
    "${HERE}/environ"
    "${HERE}/../Sources/CShioriKrkr/include"
    "${GEN}"
    "${CORE}" "${CORE}/common" "${CORE}/tjs2"
    "${CORE}/base" "${CORE}/base/impl"
    "${CORE}/environ" "${CORE}/environ/win32"
    "${CORE}/visual" "${CORE}/visual/gl" "${CORE}/visual/impl"
    "${CORE}/sound" "${CORE}/sound/win32"
    "${CORE}/utils" "${CORE}/utils/win32"
    "${CORE}/plugin" "${CORE}/extension"
    "${KRKR2}/plugins")

find_package(PNG REQUIRED)
find_package(Freetype REQUIRED)
find_package(JPEG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)
find_package(Vorbis CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(oniguruma CONFIG REQUIRED)
find_package(Opus CONFIG REQUIRED)
find_package(OpusFile CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(uchardet CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_package(plutovg CONFIG REQUIRED)
target_link_libraries(ShioriKrkr PRIVATE
    PNG::PNG Freetype::Freetype JPEG::JPEG
    $<IF:$<TARGET_EXISTS:libjpeg-turbo::turbojpeg>,libjpeg-turbo::turbojpeg,libjpeg-turbo::turbojpeg-static>
    Vorbis::vorbis Vorbis::vorbisfile Vorbis::vorbisenc
    WebP::webp WebP::webpdecoder WebP::webpdemux
    oniguruma::onig Opus::opus OpusFile::opusfile
    fmt::fmt spdlog::spdlog lz4::lz4 uchardet::libuchardet xxHash::xxhash plutovg::plutovg iconv)

# FFmpeg (LGPL-2.1+): the app's Vendor/ShioriFFmpeg.xcframework slice on iOS,
# build/ffmpeg-macos for the harness.
set(FFMPEG_INCLUDE "" CACHE PATH "FFmpeg headers")
set(FFMPEG_LIBRARY "" CACHE FILEPATH "FFmpeg static library (libffmpeg.a)")
target_include_directories(ShioriKrkr PRIVATE "${FFMPEG_INCLUDE}")
target_link_libraries(ShioriKrkr PRIVATE "${FFMPEG_LIBRARY}")

# Only the C interface leaves the image.
set(EXPORTS "${HERE}/exports.txt")
target_link_options(ShioriKrkr PRIVATE "LINKER:-exported_symbols_list,${EXPORTS}" "LINKER:-dead_strip")

if(IOS)
    target_link_libraries(ShioriKrkr PRIVATE
        "-framework Foundation" "-framework UIKit" "-framework CoreGraphics" "-framework QuartzCore"
        "-framework AVFoundation" "-framework AudioToolbox" "-framework Metal" "-framework CoreAudio"
        "-framework CoreText" "-framework ImageIO" "-framework VideoToolbox" "-framework CoreMedia"
        "-framework CoreVideo")
else()
    target_link_libraries(ShioriKrkr PRIVATE
        "-framework Foundation" "-framework AppKit" "-framework CoreGraphics" "-framework QuartzCore"
        "-framework AudioToolbox" "-framework Metal" "-framework CoreAudio" "-framework CoreText"
        "-framework AVFoundation" "-framework ImageIO" "-framework VideoToolbox" "-framework CoreMedia"
        "-framework CoreVideo" "-framework Security")
    add_executable(shiorikrkr-harness "${HERE}/harness/main.mm")
    target_include_directories(shiorikrkr-harness PRIVATE "${HERE}/../Sources/CShioriKrkr/include")
    target_compile_options(shiorikrkr-harness PRIVATE -fobjc-arc)
    target_link_libraries(shiorikrkr-harness PRIVATE ShioriKrkr "-framework Foundation" "-framework AppKit"
        "-framework Metal" "-framework QuartzCore" "-framework ImageIO" "-framework CoreGraphics")
endif()
