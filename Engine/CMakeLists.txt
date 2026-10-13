# This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
# If a copy of the MPL was not distributed with this file, You can obtain one at
# https://mozilla.org/MPL/2.0/.
#
# ShioriKrkr: krkrsdl3 (Vendor/krkrsdl3) and Shiori's host glue as one dynamic library that
# exports only shiorikrkr.h. Built per slice by Scripts/krkrsdl3/build-framework.sh, which
# passes the vcpkg toolchain and triplet; on macOS it also builds the harness.
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
set(KRKRSDL3 "${VENDOR}/krkrsdl3")
set(KRKRSDL3_BUILD "${VENDOR}/krkrsdl3_build")

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(IOS TRUE)
else()
    set(MACOS TRUE)
endif()

# krkrsdl3's own source lists (krkrsdl3_build/cmake/*.txt) name its files relative to their
# cpp/ submodule; rewrite them to Vendor/krkrsdl3 and read them in.
set(USE_SDL3 ON)
set(USE_FFMPEG OFF)
foreach(list core environ plugins tjs2)
    file(READ "${KRKRSDL3_BUILD}/cmake/cmake_${list}.txt" text)
    string(REPLACE "\${CMAKE_CURRENT_SOURCE_DIR}/cpp" "${KRKRSDL3}" text "${text}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/krkrsdl3_${list}.cmake" "${text}")
    include("${CMAKE_CURRENT_BINARY_DIR}/krkrsdl3_${list}.cmake")
endforeach()

set(SOURCES ${KRKRSDL_CORE_SRC} ${KRKRSDL_ENVIRON_SRC} ${KRKRSDL_PLUGINS_SRC} ${KRKRSDL_TJS2_SRC})
# SDL's main is the host app's, not krkrsdl3's; movies are Shiori's (shiorikrkr_movie.cpp) in
# place of krkrsdl3's Apple stubs.
list(REMOVE_ITEM SOURCES "${KRKRSDL3}/environ/sdl3/sdl3_entry.cpp" "${KRKRSDL3}/environ/apple/apple_video.mm")
list(APPEND SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/shiorikrkr_host.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/cx_decrypt.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shiorikrkr_presenter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/cursor_decode.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/shiorikrkr_movie.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/nekoworks.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/getlangname.cpp")

option(SHIORIKRKR_DEBUG_THROW "Backtrace every C++ throw (SHIORIKRKR_TRACE_THROW=1)" OFF)
if(SHIORIKRKR_DEBUG_THROW)
    list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/debug_throw.cpp")
endif()

add_library(ShioriKrkr SHARED ${SOURCES})
set_target_properties(ShioriKrkr PROPERTIES OUTPUT_NAME ShioriKrkr)

target_compile_definitions(ShioriKrkr PRIVATE _KRKRSDL3_USE_SDL3=1 NDEBUG=1)
if(IOS)
    target_compile_definitions(ShioriKrkr PRIVATE _KRKRSDL3_IOS=1)
else()
    target_compile_definitions(ShioriKrkr PRIVATE _KRKRSDL3_MACOS=1)
endif()
# Upstream code is written for warnings-off builds; keep the log readable.
target_compile_options(ShioriKrkr PRIVATE -w)
# krkrsdl3's Objective-C++ is manual retain/release; Shiori's is ARC.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/shiorikrkr_host.mm" "${CMAKE_CURRENT_SOURCE_DIR}/cursor_decode.mm"
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

target_include_directories(ShioriKrkr PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../Sources/CShioriKrkr/include"
    ${KRKRSDL_CORE_PATH}/archive
    ${KRKRSDL_CORE_PATH}/main
    ${KRKRSDL_CORE_PATH}/media/font
    ${KRKRSDL_CORE_PATH}/media/image
    ${KRKRSDL_CORE_PATH}/media/movie
    ${KRKRSDL_CORE_PATH}/media/sound
    ${KRKRSDL_CORE_PATH}/msg
    ${KRKRSDL_CORE_PATH}/render
    ${KRKRSDL_CORE_PATH}/script
    ${KRKRSDL_CORE_PATH}/utils
    ${KRKRSDL_CORE_PATH}/utils/math
    ${KRKRSDL_ENVIRON_PATH}
    ${KRKRSDL_PLUGINS_PATH}
    ${KRKRSDL_TJS2_PATH})

find_package(PNG REQUIRED)
find_package(Freetype REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(JPEG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)
find_package(Vorbis CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(oniguruma CONFIG REQUIRED)
find_package(Opus CONFIG REQUIRED)
find_package(OpusFile CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_ttf CONFIG REQUIRED)
find_package(plutovg CONFIG REQUIRED)
target_link_libraries(ShioriKrkr PRIVATE
    PNG::PNG Freetype::Freetype glm::glm-header-only JPEG::JPEG
    $<IF:$<TARGET_EXISTS:libjpeg-turbo::turbojpeg>,libjpeg-turbo::turbojpeg,libjpeg-turbo::turbojpeg-static>
    Vorbis::vorbis Vorbis::vorbisfile Vorbis::vorbisenc
    WebP::webp WebP::webpdecoder WebP::webpdemux
    oniguruma::onig Opus::opus OpusFile::opusfile
    SDL3::SDL3 SDL3_ttf::SDL3_ttf plutovg::plutovg)

# FFmpeg (LGPL-2.1+, the ASF demuxer and WMV 9 / WMA 2 decoders): the app's
# Vendor/ShioriFFmpeg.xcframework slice on iOS, build/ffmpeg-macos for the harness.
set(FFMPEG_INCLUDE "" CACHE PATH "FFmpeg headers")
set(FFMPEG_LIBRARY "" CACHE FILEPATH "FFmpeg static library (libffmpeg.a)")
target_include_directories(ShioriKrkr PRIVATE "${FFMPEG_INCLUDE}")
target_link_libraries(ShioriKrkr PRIVATE "${FFMPEG_LIBRARY}")

# Only the C interface leaves the image.
set(EXPORTS "${CMAKE_CURRENT_SOURCE_DIR}/exports.txt")
target_link_options(ShioriKrkr PRIVATE "LINKER:-exported_symbols_list,${EXPORTS}" "LINKER:-dead_strip")

if(IOS)
    target_link_libraries(ShioriKrkr PRIVATE
        "-framework Foundation" "-framework UIKit" "-framework CoreGraphics" "-framework QuartzCore"
        "-framework AVFoundation" "-framework AudioToolbox" "-framework GameController" "-framework Metal"
        "-framework CoreMotion" "-framework CoreHaptics" "-framework CoreBluetooth" "-framework CoreAudio"
        "-framework UniformTypeIdentifiers" "-framework ImageIO")
else()
    target_link_libraries(ShioriKrkr PRIVATE
        "-framework Foundation" "-framework AppKit" "-framework CoreGraphics" "-framework QuartzCore"
        "-framework IOKit" "-framework ForceFeedback" "-framework GameController" "-framework AudioToolbox"
        "-framework Carbon" "-framework Metal" "-framework CoreAudio" "-framework CoreHaptics"
        "-framework UniformTypeIdentifiers" "-framework AVFoundation" "-framework ImageIO")
    # The Mac harness: runs a game headless-ish (a window, no app), taps and captures.
    add_executable(shiorikrkr-harness "${CMAKE_CURRENT_SOURCE_DIR}/harness/main.mm")
    target_include_directories(shiorikrkr-harness PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../Sources/CShioriKrkr/include")
    target_compile_options(shiorikrkr-harness PRIVATE -fobjc-arc)
    target_link_libraries(shiorikrkr-harness PRIVATE ShioriKrkr "-framework Foundation" "-framework AppKit")
endif()
