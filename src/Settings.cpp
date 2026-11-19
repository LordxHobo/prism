// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Settings.h"

#include "Util.h"

#include <cwctype>
#include <initializer_list>
#include <iterator>
#include <vector>
#include <windows.h>

namespace settings {

namespace {

struct SearchEngine {
    const wchar_t* id;
    const wchar_t* name;
    const wchar_t* url;
};

// Privacy-respecting engines first. Google and Bing are offered because
// people ask for them, not as a recommendation.
const SearchEngine kSearchEngines[] = {
    { L"duckduckgo", L"DuckDuckGo", L"https://duckduckgo.com/?q=%s" },
    { L"startpage", L"Startpage", L"https://www.startpage.com/do/search?q=%s" },
    { L"brave", L"Brave Search", L"https://search.brave.com/search?q=%s" },
    { L"ecosia", L"Ecosia", L"https://www.ecosia.org/search?q=%s" },
    { L"google", L"Google", L"https://www.google.com/search?q=%s" },
    { L"bing", L"Bing", L"https://www.bing.com/search?q=%s" },
};

const wchar_t kDefaultAccent[] = L"#8b7cf6";

bool isOneOf(const std::wstring& value, std::initializer_list<const wchar_t*> allowed)
{
    for (auto* option : allowed) {
        if (value == option)
            return true;
    }
    return false;
}

bool isHexColor(const std::wstring& value)
{
    if (value.size() != 7 || value[0] != L'#')
        return false;
    for (size_t i = 1; i < 7; ++i) {
        if (!iswxdigit(value[i]))
            return false;
    }
    return true;
}

bool isValidSearchTemplate(const std::wstring& value)
{
    return (value.rfind(L"https://", 0) == 0 || value.rfind(L"http://", 0) == 0)
        && value.find(L"%s") != std::wstring::npos
        && value.size() < 2000;
}

bool isDirectory(const std::wstring& path)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring searchEngineId()
{
    std::wstring id = util::readSetting(L"SearchEngine", L"");
    if (id.empty()) {
        // v0.1 only had SearchURL. Keep a custom one if someone set it.
        std::wstring legacy = util::readSetting(L"SearchURL", L"");
        return !legacy.empty() && legacy != kSearchEngines[0].url ? L"custom" : L"duckduckgo";
    }
    return id;
}

std::string jsonBool(bool value)
{
    return value ? "true" : "false";
}

} // namespace

std::wstring searchURL()
{
    std::wstring id = searchEngineId();
    if (id == L"custom") {
        std::wstring custom = util::readSetting(L"SearchURL", L"");
        if (isValidSearchTemplate(custom))
            return custom;
    }
    for (auto& engine : kSearchEngines) {
        if (id == engine.id)
            return engine.url;
    }
    return kSearchEngines[0].url;
}

bool restoreSession()
{
    return util::readSetting(L"Startup", L"newtab") == L"restore";
}

std::wstring downloadDirectory()
{
    std::wstring custom = util::readSetting(L"DownloadDir", L"");
    if (!custom.empty() && isDirectory(custom))
        return custom;
    return util::downloadsDirectory();
}

std::wstring theme()
{
    std::wstring value = util::readSetting(L"Theme", L"system");
    return isOneOf(value, { L"system", L"dark", L"light" }) ? value : L"system";
}

std::wstring accent()
{
    std::wstring value = util::readSetting(L"Accent", kDefaultAccent);
    return isHexColor(value) ? value : kDefaultAccent;
}

bool compactTabs()
{
    return util::readSetting(L"TabStyle", L"normal") == L"compact";
}

bool strictCookies()
{
    return util::readSetting(L"CookiePolicy", L"standard") == L"strict";
}

bool clearOnExit()
{
    return util::readSetting(L"ClearOnExit", L"0") == L"1";
}

bool saveHistory()
{
    return util::readSetting(L"SaveHistory", L"1") == L"1";
}

bool showBookmarksBar()
{
    return util::readSetting(L"BookmarksBar", L"1") == L"1";
}

std::wstring permissionDefault(const std::wstring& permission)
{
    const wchar_t* key = permission == L"location" ? L"LocationDefault" : L"NotificationsDefault";
    std::wstring value = util::readSetting(key, L"ask");
    return value == L"block" ? L"block" : L"ask";
}

bool verticalTabs()
{
    return util::readSetting(L"TabLayout", L"horizontal") == L"vertical";
}

bool verticalTabsCollapsed()
{
    return util::readSetting(L"VerticalTabsCollapsed", L"0") == L"1";
}

std::wstring groupStyle()
{
    return util::readSetting(L"GroupStyle", L"chrome") == L"vivaldi" ? L"vivaldi" : L"chrome";
}

std::wstring palette()
{
    std::wstring value = util::readSetting(L"Palette", L"default");
    return isOneOf(value, { L"default", L"midnight", L"nord", L"forest", L"rose", L"sepia" }) ? value : L"default";
}

bool showHomeButton()
{
    return util::readSetting(L"HomeButton", L"0") == L"1";
}

namespace {

std::vector<std::string> hiddenActions()
{
    std::string list = util::toUTF8(util::readSetting(L"HiddenActions", L""));
    std::vector<std::string> ids;
    size_t start = 0;
    while (start < list.size()) {
        size_t comma = list.find(',', start);
        std::string id = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!id.empty())
            ids.push_back(id);
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return ids;
}

} // namespace

bool isActionHidden(const std::string& extensionId)
{
    for (auto& id : hiddenActions()) {
        if (id == extensionId)
            return true;
    }
    return false;
}

void setActionHidden(const std::string& extensionId, bool hidden)
{
    if (extensionId.empty() || extensionId.find(',') != std::string::npos)
        return;
    std::vector<std::string> ids;
    for (auto& id : hiddenActions()) {
        if (id != extensionId)
            ids.push_back(id);
    }
    if (hidden)
        ids.push_back(extensionId);
    std::string list;
    for (auto& id : ids)
        list += (list.empty() ? "" : ",") + id;
    util::writeSetting(L"HiddenActions", util::fromUTF8(list));
}

std::string hiddenActionsJSON()
{
    std::string json = "[";
    for (auto& id : hiddenActions())
        json += (json.size() > 1 ? "," : "") + util::jsonString(util::fromUTF8(id));
    return json + "]";
}

std::wstring newTabShortcuts()
{
    std::wstring value = util::readSetting(L"NewTabShortcuts", L"top");
    return isOneOf(value, { L"top", L"bookmarks", L"off" }) ? value : L"top";
}

std::string toJSON()
{
    std::string json = "{";
    json += "\"searchEngine\":" + util::jsonString(searchEngineId());
    json += ",\"searchURL\":" + util::jsonString(util::readSetting(L"SearchURL", L""));
    json += ",\"searchEngines\":[";
    for (size_t i = 0; i < std::size(kSearchEngines); ++i) {
        if (i)
            json += ',';
        json += "{\"id\":" + util::jsonString(kSearchEngines[i].id) + ",\"name\":" + util::jsonString(kSearchEngines[i].name) + "}";
    }
    json += "]";
    json += ",\"startup\":" + util::jsonString(restoreSession() ? L"restore" : L"newtab");
    json += ",\"downloadDir\":" + util::jsonString(downloadDirectory());
    json += ",\"theme\":" + util::jsonString(theme());
    json += ",\"accent\":" + util::jsonString(accent());
    json += ",\"tabStyle\":" + util::jsonString(compactTabs() ? L"compact" : L"normal");
    json += ",\"cookiePolicy\":" + util::jsonString(strictCookies() ? L"strict" : L"standard");
    json += ",\"clearOnExit\":" + jsonBool(clearOnExit());
    json += ",\"saveHistory\":" + jsonBool(saveHistory());
    json += ",\"bookmarksBar\":" + jsonBool(showBookmarksBar());
    json += ",\"locationDefault\":" + util::jsonString(permissionDefault(L"location"));
    json += ",\"notificationsDefault\":" + util::jsonString(permissionDefault(L"notifications"));
    json += ",\"tabLayout\":" + util::jsonString(verticalTabs() ? L"vertical" : L"horizontal");
    json += ",\"groupStyle\":" + util::jsonString(groupStyle());
    json += ",\"palette\":" + util::jsonString(palette());
    json += ",\"homeButton\":" + jsonBool(showHomeButton());
    json += ",\"newTabShortcuts\":" + util::jsonString(newTabShortcuts());
    json += ",\"dataDir\":" + util::jsonString(util::dataDirectory());
    json += "}";
    return json;
}

bool set(const std::wstring& key, const std::wstring& value)
{
    if (key == L"searchEngine") {
        bool known = value == L"custom";
        for (auto& engine : kSearchEngines)
            known = known || value == engine.id;
        if (!known)
            return false;
        util::writeSetting(L"SearchEngine", value);
    } else if (key == L"searchURL") {
        if (!isValidSearchTemplate(value))
            return false;
        util::writeSetting(L"SearchURL", value);
        util::writeSetting(L"SearchEngine", L"custom");
    } else if (key == L"startup") {
        if (!isOneOf(value, { L"newtab", L"restore" }))
            return false;
        util::writeSetting(L"Startup", value);
    } else if (key == L"downloadDir") {
        if (!value.empty() && !isDirectory(value))
            return false;
        util::writeSetting(L"DownloadDir", value);
    } else if (key == L"theme") {
        if (!isOneOf(value, { L"system", L"dark", L"light" }))
            return false;
        util::writeSetting(L"Theme", value);
    } else if (key == L"accent") {
        if (!isHexColor(value))
            return false;
        util::writeSetting(L"Accent", value);
    } else if (key == L"tabStyle") {
        if (!isOneOf(value, { L"normal", L"compact" }))
            return false;
        util::writeSetting(L"TabStyle", value);
    } else if (key == L"cookiePolicy") {
        if (!isOneOf(value, { L"standard", L"strict" }))
            return false;
        util::writeSetting(L"CookiePolicy", value);
    } else if (key == L"clearOnExit") {
        if (!isOneOf(value, { L"0", L"1" }))
            return false;
        util::writeSetting(L"ClearOnExit", value);
    } else if (key == L"saveHistory") {
        if (!isOneOf(value, { L"0", L"1" }))
            return false;
        util::writeSetting(L"SaveHistory", value);
    } else if (key == L"bookmarksBar") {
        if (!isOneOf(value, { L"0", L"1" }))
            return false;
        util::writeSetting(L"BookmarksBar", value);
    } else if (key == L"tabLayout") {
        if (!isOneOf(value, { L"horizontal", L"vertical" }))
            return false;
        util::writeSetting(L"TabLayout", value);
    } else if (key == L"verticalTabsCollapsed") {
        if (!isOneOf(value, { L"0", L"1" }))
            return false;
        util::writeSetting(L"VerticalTabsCollapsed", value);
    } else if (key == L"groupStyle") {
        if (!isOneOf(value, { L"chrome", L"vivaldi" }))
            return false;
        util::writeSetting(L"GroupStyle", value);
    } else if (key == L"palette") {
        if (!isOneOf(value, { L"default", L"midnight", L"nord", L"forest", L"rose", L"sepia" }))
            return false;
        util::writeSetting(L"Palette", value);
    } else if (key == L"homeButton") {
        if (!isOneOf(value, { L"0", L"1" }))
            return false;
        util::writeSetting(L"HomeButton", value);
    } else if (key == L"newTabShortcuts") {
        if (!isOneOf(value, { L"top", L"bookmarks", L"off" }))
            return false;
        util::writeSetting(L"NewTabShortcuts", value);
    } else if (key == L"locationDefault" || key == L"notificationsDefault") {
        if (!isOneOf(value, { L"ask", L"block" }))
            return false;
        util::writeSetting(key == L"locationDefault" ? L"LocationDefault" : L"NotificationsDefault", value);
    } else
        return false;
    return true;
}

} // namespace settings
