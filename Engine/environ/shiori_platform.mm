/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// The platform functions krkr2's core expects from its environment (environ/Platform.h and
// the ones its cocos2d-x scene used to define), written for Shiori on iOS and macOS. Paths
// come from the host; message boxes go to the host's alerts; nothing here draws.
#include "shiori_environ.h"

#include "tjsCommHead.h"
#include "Platform.h"
#include "StorageIntf.h"
#include "StorageImpl.h"
#include "DebugIntf.h"
#include "TVPScreen.h"
#include "GraphicsLoaderIntf.h"

#import <Foundation/Foundation.h>
#include <mach/mach.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>
#include <atomic>
#include <chrono>
#include <thread>

namespace {
std::atomic<bool> exitFlag{false};
} // namespace

shiori::HostServices &shiori::host() {
    static HostServices services;
    return services;
}

void shiori::log(const std::string &line) {
    auto &h = host();
    if (h.log) h.log(line);
    else fprintf(stderr, "%s\n", line.c_str());
}

bool shiori::exitRequested() { return exitFlag.load(); }
void shiori::requestExit(int) { exitFlag = true; }
void shiori::resetExit() { exitFlag = false; }

// ---- memory ----
void TVPGetMemoryInfo(TVPMemoryInfo &m) {
    memset(&m, 0, sizeof(m));
    m.MemTotal = (unsigned long)(NSProcessInfo.processInfo.physicalMemory / 1024);
    m.MemFree = (unsigned long)TVPGetSystemFreeMemory() * 1024;
    m.VirtualTotal = m.MemTotal;
    m.VirtualUsed = (unsigned long)TVPGetSelfUsedMemory() * 1024;
}

tjs_int TVPGetSystemFreeMemory() {
#if TARGET_OS_IPHONE
    extern size_t os_proc_available_memory(void) __attribute__((weak_import));
    if (os_proc_available_memory) return tjs_int(os_proc_available_memory() / (1024 * 1024));
#endif
    vm_statistics64_data_t stats;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&stats, &count) != KERN_SUCCESS)
        return 1024;
    return tjs_int((uint64_t(stats.free_count) + stats.inactive_count) * vm_page_size / (1024 * 1024));
}

tjs_int TVPGetSelfUsedMemory() {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
    return tjs_int(info.phys_footprint / (1024 * 1024));
}

void TVPCheckMemory() {}

// ---- message boxes ----
int TVPShowSimpleMessageBox(const ttstr &text, const ttstr &caption, const std::vector<ttstr> &buttons) {
    std::vector<std::string> labels;
    for (const ttstr &b : buttons) labels.push_back(b.AsStdString());
    shiori::log("ShioriKrkr: message box: " + caption.AsStdString() + ": " + text.AsStdString());
    auto &h = shiori::host();
    if (!h.messageBox) return 0;
    return h.messageBox(text.AsStdString(), caption.AsStdString(), labels);
}

int TVPShowSimpleMessageBox(const ttstr &text, const ttstr &caption) {
    return TVPShowSimpleMessageBox(text, caption, std::vector<ttstr>{ttstr(TJS_W("OK"))});
}

int TVPShowSimpleMessageBoxYesNo(const ttstr &text, const ttstr &caption) {
    return TVPShowSimpleMessageBox(text, caption, std::vector<ttstr>{ttstr(TJS_W("Yes")), ttstr(TJS_W("No"))});
}

extern "C" int TVPShowSimpleMessageBox(const char *text, const char *caption, unsigned int count,
                                       const char **buttons) {
    std::vector<ttstr> labels;
    for (unsigned int i = 0; i < count; ++i) labels.emplace_back(buttons[i]);
    return TVPShowSimpleMessageBox(ttstr(text ? text : ""), ttstr(caption ? caption : ""), labels);
}

int TVPShowSimpleInputBox(ttstr &text, const ttstr &caption, const ttstr &prompt,
                          const std::vector<ttstr> &buttons) {
    // No text entry on Shiori: the default answer stands.
    return 0;
}

// ---- paths and files ----
std::vector<std::string> TVPGetDriverPath() { return {"/"}; }
std::vector<std::string> TVPGetAppStoragePath() { return {shiori::host().dataPath}; }
bool TVPCheckStartupPath(const std::string &) { return true; }
bool TVPCheckStartupArg() { return false; }
std::string TVPGetPackageVersionString() { return "Shiori"; }

const std::vector<std::string> &TVPGetApplicationHomeDirectory() {
    // Folders whose own spelling is known, so case-insensitive lookups start below them.
    static std::vector<std::string> dirs;
    static std::string forRoot;
    if (forRoot != shiori::host().gameRoot) {
        forRoot = shiori::host().gameRoot;
        dirs.clear();
        std::string root = forRoot;
        while (root.size() > 1 && root.back() == '/') root.pop_back();
        if (!root.empty()) dirs.push_back(root);
        std::string data = shiori::host().dataPath;
        while (data.size() > 1 && data.back() == '/') data.pop_back();
        if (!data.empty()) dirs.push_back(data);
    }
    return dirs;
}

std::string TVPGetDefaultFileDir() { return shiori::host().gameRoot; }

ttstr TVPGetDataPath() { return ttstr(shiori::host().dataPath); }

const std::string &TVPGetInternalPreferencePath() {
    static std::string path;
    path = shiori::host().dataPath + ".preference/";
    mkdir(path.c_str(), 0755);
    return path;
}

std::string TVPGetCurrentLanguage() {
    NSString *language = NSLocale.preferredLanguages.firstObject ?: @"en";
    return std::string(language.UTF8String);
}

bool TVPDeleteFile(const std::string &filename) { return unlink(filename.c_str()) == 0; }

bool TVPRenameFile(const std::string &from, const std::string &to) { return rename(from.c_str(), to.c_str()) == 0; }

bool TVPCopyFile(const std::string &from, const std::string &to) {
    FILE *in = fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE *out = fopen(to.c_str(), "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buffer[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0)
        if (fwrite(buffer, 1, n, out) != n) {
            ok = false;
            break;
        }
    fclose(in);
    fclose(out);
    return ok;
}

bool TVPCreateFolders(const ttstr &folder) {
    if (folder.IsEmpty()) return true;
    std::string path = folder.AsStdString();
    NSString *ns = [NSString stringWithUTF8String:path.c_str()];
    if (!ns) return false;
    return [NSFileManager.defaultManager createDirectoryAtPath:ns withIntermediateDirectories:YES attributes:nil error:nil];
}

bool TVPWriteDataToFile(const ttstr &filepath, const void *data, unsigned int len) {
    std::string path = filepath.AsStdString();
    // Written beside and renamed into place, so a crash never leaves half a save.
    std::string temp = path + ".shiori-tmp";
    FILE *f = fopen(temp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        unlink(temp.c_str());
        return false;
    }
    return rename(temp.c_str(), path.c_str()) == 0;
}

bool TVP_stat(const char *name, tTVP_stat &s) {
    struct stat t;
    if (stat(name, &t) != 0) return false;
    s.st_mode = t.st_mode;
    s.st_size = t.st_size;
    s.st_atime = t.st_atimespec.tv_sec;
    s.st_mtime = t.st_mtimespec.tv_sec;
    s.st_ctime = t.st_ctimespec.tv_sec;
    return true;
}

bool TVP_stat(const tjs_char *name, tTVP_stat &s) { return TVP_stat(ttstr(name).AsStdString().c_str(), s); }

bool TVP_utime(const char *name, time_t modtime) {
    struct utimbuf times = {modtime, modtime};
    return utime(name, &times) == 0;
}

void TVPSendToOtherApp(const std::string &) {}

// ---- process ----
void TVPExitApplication(int code) {
    shiori::log("ShioriKrkr: the game quit (" + std::to_string(code) + ")");
    shiori::requestExit(code);
}

void TVPRelinquishCPU() { std::this_thread::yield(); }

void TVPPrintLog(const char *str) { shiori::log(str ? str : ""); }

void TVPShowIME(int, int, int, int) {}
void TVPHideIME() {}
void TVPProcessInputEvents() {}
void TVPForceSwapBuffer() {}
void TVPControlAdDialog(int, int, int) {}

tjs_uint32 TVPGetRoughTickCount32() {
    using namespace std::chrono;
    return tjs_uint32(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void TVPConsoleLog(const ttstr &line, bool) { shiori::log(line.AsStdString()); }

namespace TJS {
void TVPConsoleLog(const ttstr &line) { shiori::log(line.AsStdString()); }
} // namespace TJS

ttstr TVPGetPlatformName() {
#if TARGET_OS_IPHONE
    return TJS_W("iPhone");
#else
    return TJS_W("MacOS");
#endif
}

// ---- the screen ----
// KiriKiri asks for the desktop size to place and size its windows; a 1920x1080 desktop keeps
// every game's 1280x720 (or 1920x1080) window at its natural size.
int tTVPScreen::GetWidth() { return 1920; }
int tTVPScreen::GetHeight() { return 1080; }
int tTVPScreen::GetDesktopLeft() { return 0; }
int tTVPScreen::GetDesktopTop() { return 0; }
int tTVPScreen::GetDesktopWidth() { return GetWidth(); }
int tTVPScreen::GetDesktopHeight() { return GetHeight(); }

bool TVPGetScreenSize(tjs_int idx, tjs_int &w, tjs_int &h) {
    if (idx != 0) return false;
    w = tTVPScreen::GetWidth();
    h = tTVPScreen::GetHeight();
    return true;
}

// ---- formats and archives krkr2 supports that Shiori leaves out ----
tTVPArchive *TVPOpen7ZArchive(const ttstr &, tTJSBinaryStream *, bool) { return nullptr; }
tTVPArchive *TVPOpenTARArchive(const ttstr &, tTJSBinaryStream *, bool) { return nullptr; }
tTVPArchive *TVPOpenZIPArchive(const ttstr &, tTJSBinaryStream *, bool) { return nullptr; }

static void unsupportedImage(const char *format) {
    TVPThrowExceptionMessage(TJS_W("%1 images are not supported"), ttstr(format));
}
void TVPLoadBPG(void *, void *, tTVPGraphicSizeCallback, tTVPGraphicScanLineCallback, tTVPMetaInfoPushCallback,
                tTJSBinaryStream *, tjs_int, tTVPGraphicLoadMode) {
    unsupportedImage("BPG");
}
void TVPLoadHeaderBPG(void *, tTJSBinaryStream *, iTJSDispatch2 **) { unsupportedImage("BPG"); }
void TVPLoadJXR(void *, void *, tTVPGraphicSizeCallback, tTVPGraphicScanLineCallback, tTVPMetaInfoPushCallback,
                tTJSBinaryStream *, tjs_int, tTVPGraphicLoadMode) {
    unsupportedImage("JPEG XR");
}
void TVPLoadHeaderJXR(void *, tTJSBinaryStream *, iTJSDispatch2 **) { unsupportedImage("JPEG XR"); }
void TVPLoadPVRv3(void *, void *, tTVPGraphicSizeCallback, tTVPGraphicScanLineCallback, tTVPMetaInfoPushCallback,
                  tTJSBinaryStream *, tjs_int, tTVPGraphicLoadMode) {
    unsupportedImage("PVR");
}
void TVPLoadHeaderPVRv3(void *, tTJSBinaryStream *, iTJSDispatch2 **) { unsupportedImage("PVR"); }
bool TVPAcceptSaveAsJXR(void *, const ttstr &, iTJSDispatch2 **) { return false; }
void TVPSaveAsJXR(void *, tTJSBinaryStream *, const iTVPBaseBitmap *, const ttstr &, iTJSDispatch2 *) {
    unsupportedImage("JPEG XR");
}
