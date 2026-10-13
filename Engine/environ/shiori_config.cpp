/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// krkr2's configuration managers (declared in its environ/ConfigManager headers), written
// for Shiori: no preference files and no launcher UI. The host sets the few values the core
// reads (renderer, default_font ...) before the game starts; UI texts the core shows are
// plain English.
#include "shiori_environ.h"

#include "ConfigManager/GlobalConfigManager.h"
#include "ConfigManager/IndividualConfigManager.h"
#include "ConfigManager/LocaleConfigManager.h"

#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace {

std::unordered_map<std::string, std::string> &hostValues() {
    static std::unordered_map<std::string, std::string> values;
    return values;
}

bool lookup(const std::string &name, std::string &value) {
    auto &values = hostValues();
    auto found = values.find(name);
    if (found == values.end()) return false;
    value = found->second;
    return true;
}

} // namespace

void shiori::setConfigValue(const std::string &name, const std::string &value) { hostValues()[name] = value; }

void shiori::clearConfigValues() { hostValues().clear(); }

// ---- iSysConfigManager ----
void iSysConfigManager::Initialize() {}
void iSysConfigManager::SaveToFile() {}
bool iSysConfigManager::IsValueExist(const std::string &name) {
    std::string value;
    return lookup(name, value);
}
template <>
bool iSysConfigManager::GetValue<bool>(const std::string &name, const bool &defVal) {
    std::string value;
    if (!lookup(name, value)) return defVal;
    return value == "1" || value == "true" || value == "yes";
}
template <>
int iSysConfigManager::GetValue<int>(const std::string &name, const int &defVal) {
    std::string value;
    return lookup(name, value) ? std::atoi(value.c_str()) : defVal;
}
template <>
float iSysConfigManager::GetValue<float>(const std::string &name, const float &defVal) {
    std::string value;
    return lookup(name, value) ? float(std::atof(value.c_str())) : defVal;
}
template <>
std::string iSysConfigManager::GetValue<std::string>(const std::string &name, const std::string &defVal) {
    std::string value;
    return lookup(name, value) ? value : defVal;
}
void iSysConfigManager::SetValueInt(const std::string &name, int val) { hostValues()[name] = std::to_string(val); }
void iSysConfigManager::SetValueFloat(const std::string &name, float val) { hostValues()[name] = std::to_string(val); }
void iSysConfigManager::SetValue(const std::string &name, const std::string &val) { hostValues()[name] = val; }
void iSysConfigManager::SetKeyMap(int k, int v) {
    if (v) KeyMap[k] = v;
    else KeyMap.erase(k);
}
std::vector<std::string> iSysConfigManager::GetCustomArgumentsForPush() { return {}; }

// ---- GlobalConfigManager ----
GlobalConfigManager::GlobalConfigManager() {}
std::string GlobalConfigManager::GetFilePath() { return {}; }
GlobalConfigManager *GlobalConfigManager::GetInstance() {
    static GlobalConfigManager instance;
    return &instance;
}

// ---- IndividualConfigManager ----
std::string IndividualConfigManager::GetFilePath() { return {}; }
void IndividualConfigManager::Clear() {}
IndividualConfigManager *IndividualConfigManager::GetInstance() {
    static IndividualConfigManager *instance = new IndividualConfigManager();
    return instance;
}
bool IndividualConfigManager::CheckExistAt(const std::string &) { return false; }
bool IndividualConfigManager::CreatePreferenceAt(const std::string &folder) {
    CurrentPath = folder;
    return true;
}
bool IndividualConfigManager::UsePreferenceAt(const std::string &folder) {
    CurrentPath = folder;
    return true;
}
template <>
bool IndividualConfigManager::GetValue<bool>(const std::string &name, const bool &defVal) {
    return inherit::GetValue<bool>(name, defVal);
}
template <>
int IndividualConfigManager::GetValue<int>(const std::string &name, const int &defVal) {
    return inherit::GetValue<int>(name, defVal);
}
template <>
float IndividualConfigManager::GetValue<float>(const std::string &name, const float &defVal) {
    return inherit::GetValue<float>(name, defVal);
}
template <>
std::string IndividualConfigManager::GetValue<std::string>(const std::string &name, const std::string &defVal) {
    return inherit::GetValue<std::string>(name, defVal);
}
std::vector<std::string> IndividualConfigManager::GetCustomArgumentsForPush() { return {}; }

// ---- LocaleConfigManager ----
LocaleConfigManager::LocaleConfigManager() {
    AllConfig = {
        {"retry", "Retry"},
        {"cancel", "Cancel"},
        {"ok", "OK"},
        {"msgbox_ok", "OK"},
        {"msgbox_yes", "Yes"},
        {"msgbox_no", "No"},
        {"err_no_memory", "Not enough memory."},
        {"err_occured", "Error"},
        {"notice", "Notice"},
    };
}
std::string LocaleConfigManager::GetFilePath() { return {}; }
LocaleConfigManager *LocaleConfigManager::GetInstance() {
    static LocaleConfigManager *instance = new LocaleConfigManager();
    return instance;
}
void LocaleConfigManager::Initialize(const std::string &sysLang) { currentLangCode = sysLang; }
const std::string &LocaleConfigManager::GetText(const std::string &tid) {
    auto found = AllConfig.find(tid);
    if (found != AllConfig.end()) return found->second;
    return AllConfig.emplace(tid, tid).first->second;
}
bool LocaleConfigManager::initText(cocos2d::ui::Text *) { return false; }
bool LocaleConfigManager::initText(cocos2d::ui::Button *) { return false; }
bool LocaleConfigManager::initText(cocos2d::ui::Text *, const std::string &) { return false; }
bool LocaleConfigManager::initText(cocos2d::ui::Button *, const std::string &) { return false; }
