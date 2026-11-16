// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Settings.h"

#include "Util.h"

#include <cwctype>
#include <initializer_list>
#include <iterator>
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
    } else
        return false;
    return true;
}

} // namespace settings
