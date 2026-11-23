// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Aci.h"

#include "Dialogs.h"
#include "Engine.h"
#include "Extensions.h"
#include "Json.h"
#include "Settings.h"
#include "Tabs.h"
#include "Util.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <vector>

using namespace wk;
namespace fs = std::filesystem;

namespace aci {

namespace {

std::string g_source;   // ui/aci.js
std::string g_key;      // per start: names the view's element, proves fallback messages
std::map<std::string, std::string> g_siteViews; // site -> view
bool g_sitesLoaded = false;
std::map<std::string, std::string> g_fontData; // bundled fonts as data: URLs

const char* kViews[] = { "focus", "dyslexia", "dense" };

bool isView(const std::string& view)
{
    return std::find(std::begin(kViews), std::end(kViews), view) != std::end(kViews);
}

std::wstring sitesPath()
{
    return util::dataDirectory() + L"\\aci-sites.json";
}

void loadSites()
{
    if (g_sitesLoaded)
        return;
    g_sitesLoaded = true;
    json::Value root;
    if (!json::parse(util::readFile(sitesPath()), root) || !root.isObject())
        return;
    for (auto& [site, view] : root.members()) {
        if (view.isString() && isView(view.asString()))
            g_siteViews[site] = view.asString();
    }
}

void saveSites()
{
    std::string json = "{";
    bool first = true;
    for (auto& [site, view] : g_siteViews) {
        json += std::string(first ? "" : ",") + json::quote(site) + ":" + json::quote(view);
        first = false;
    }
    util::writeFileAtomically(sitesPath(), json + "}\n");
}

std::string siteOf(const std::wstring& url)
{
    std::string text = util::toUTF8(url);
    size_t start = text.find("://");
    if (start == std::string::npos)
        return { };
    std::string host = text.substr(start + 3);
    host = host.substr(0, host.find_first_of("/?#"));
    size_t at = host.rfind('@');
    if (at != std::string::npos)
        host.erase(0, at + 1);
    size_t port = host.rfind(':');
    if (port != std::string::npos && host.find(']') == std::string::npos)
        host.erase(port);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    if (host.rfind("www.", 0) == 0)
        host.erase(0, 4);
    return host;
}

std::string setting(const wchar_t* key, const wchar_t* fallback = L"")
{
    return util::toUTF8(util::readSetting(key, fallback));
}

// The view's settings: only what was chosen, so views keep their own defaults.
std::string settingsJSON()
{
    std::string json = "{\"font\":" + json::quote(setting(L"AciFont", L"comic"));
    std::string fileName = setting(L"AciFileName");
    if (!fileName.empty())
        json += ",\"fileName\":" + json::quote(fileName);
    for (auto [key, name] : { std::pair { L"AciTextSize", "textSize" }, { L"AciSpacing", "spacing" }, { L"AciWidth", "width" } }) {
        std::string value = setting(key);
        if (!value.empty() && value.find_first_not_of("0123456789.") == std::string::npos)
            json += ",\"" + std::string(name) + "\":" + value;
    }
    json += ",\"tint\":" + json::quote(setting(L"AciTint", L"auto"));
    json += ",\"bionic\":" + std::string(setting(L"AciBionic") == "1" ? "true" : "false");
    return json + "}";
}

std::string dataURL(const std::string& mime, const std::string& bytes)
{
    return "data:" + mime + ";base64," + util::base64(bytes);
}

std::string fontMime(const std::wstring& extension)
{
    std::wstring lower = extension;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    if (lower == L".woff2") return "font/woff2";
    if (lower == L".woff") return "font/woff";
    if (lower == L".otf") return "font/otf";
    return "font/ttf";
}

std::wstring customFontPath()
{
    std::wstring dir = util::dataDirectory();
    for (auto* extension : { L".ttf", L".otf", L".woff", L".woff2" }) {
        std::wstring path = dir + L"\\aci-font" + extension;
        std::error_code error;
        if (fs::exists(path, error))
            return path;
    }
    return { };
}

std::string fontsJSON(const std::string& font)
{
    // Only the font in use (its files are ~130 KB each); the view asks
    // Prism again if another one is picked.
    std::string json = "{";
    auto add = [&](const char* name, const wchar_t* file) {
        auto cached = g_fontData.find(name);
        if (cached == g_fontData.end()) {
            std::string bytes = util::readFile(util::exeDirectory() + L"\\ui\\fonts\\" + file);
            cached = g_fontData.emplace(name, bytes.empty() ? std::string() : dataURL("font/woff", bytes)).first;
        }
        if (!cached->second.empty())
            json += std::string(json.size() > 1 ? "," : "") + "\"" + name + "\":" + json::quote(cached->second);
    };
    if (font == "opendyslexic") {
        add("opendyslexic400", L"opendyslexic-latin-400-normal.woff");
        add("opendyslexic700", L"opendyslexic-latin-700-normal.woff");
    } else if (font == "lexend") {
        add("lexend400", L"lexend-latin-400-normal.woff");
        add("lexend700", L"lexend-latin-700-normal.woff");
    } else if (font == "file") {
        std::wstring path = customFontPath();
        if (!path.empty()) {
            std::string bytes = util::readFile(path, 8ull * 1024 * 1024);
            if (!bytes.empty())
                json += std::string(json.size() > 1 ? "," : "") + "\"file\":" + json::quote(dataURL(fontMime(fs::path(path).extension().wstring()), bytes));
        }
    }
    return json + "}";
}

void run(WKPageRef page, const std::wstring& url, const char* command, const std::string& view)
{
    if (g_source.empty() || !page || !canRun(url))
        return;
    loadSites();
    std::string site = siteOf(url);
    auto siteView = g_siteViews.find(site);
    std::string font = setting(L"AciFont", L"comic");
    std::string config = "{\"key\":" + json::quote(g_key) + ",\"token\":" + json::quote(g_key) + ",\"command\":\"" + command + "\""
        + ",\"view\":" + json::quote(view) + ",\"dark\":" + (engine::darkTheme() ? "true" : "false")
        + ",\"accent\":" + json::quote(util::toUTF8(settings::accent())) + ",\"settings\":" + settingsJSON()
        + ",\"fonts\":" + fontsJSON(font) + ",\"siteView\":" + json::quote(siteView == g_siteViews.end() ? "" : siteView->second) + "}";
    std::string script = g_source + "(" + config + ");";
    if (extensions::isolatedContentScripts() && WKPagePostMessageToInjectedBundle) {
        // In a world of Prism's own (call id 0: no answer needed).
        WKPagePostMessageToInjectedBundle(page, stringUTF8("prism:exec").get(), stringUTF8("0\tprism-aci\t" + script).get());
    } else
        WKPageEvaluateJavaScriptInMainFrame(page, stringUTF8(script).get(), nullptr, nullptr);
}

std::string lastView()
{
    std::string view = setting(L"AciLastView", L"focus");
    return isView(view) ? view : "focus";
}

int CALLBACK addFont(const LOGFONTW* font, const TEXTMETRICW*, DWORD, LPARAM data)
{
    auto* names = reinterpret_cast<std::set<std::wstring>*>(data);
    if (font->lfFaceName[0] && font->lfFaceName[0] != L'@') // '@' = vertical variants
        names->insert(font->lfFaceName);
    return 1;
}

std::string systemFontsJSON()
{
    std::set<std::wstring> names;
    HDC dc = GetDC(nullptr);
    LOGFONTW query = { };
    query.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(dc, &query, addFont, reinterpret_cast<LPARAM>(&names), 0);
    ReleaseDC(nullptr, dc);
    std::string json = "[";
    bool first = true;
    for (auto& name : names) {
        json += std::string(first ? "" : ",") + json::quote(util::toUTF8(name));
        first = false;
    }
    return json + "]";
}

// Picks a font file, keeps a copy as aci-font.<ext>; {"data", "name"} or null.
std::string pickFontFile(HWND owner)
{
    auto file = dialogs::openFile(owner, L"Choose a font", L"Fonts (*.ttf, *.otf, *.woff, *.woff2)", L"*.ttf;*.otf;*.woff;*.woff2");
    if (!file)
        return "null";
    std::string bytes = util::readFile(*file, 8ull * 1024 * 1024);
    if (bytes.size() < 12)
        return "null";
    std::wstring extension = fs::path(*file).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (extension != L".ttf" && extension != L".otf" && extension != L".woff" && extension != L".woff2")
        return "null";
    // One custom font at a time.
    std::wstring old = customFontPath();
    if (!old.empty())
        DeleteFileW(old.c_str());
    if (!util::writeFileAtomically(util::dataDirectory() + L"\\aci-font" + extension, bytes))
        return "null";
    std::string name = util::toUTF8(fs::path(*file).stem().wstring());
    util::writeSetting(L"AciFont", L"file");
    util::writeSetting(L"AciFileName", util::fromUTF8(name));
    return "{\"data\":" + json::quote(dataURL(fontMime(extension), bytes)) + ",\"name\":" + json::quote(name) + "}";
}

HWND ownerOf(WKPageRef page)
{
    tabs::Info tab;
    return tabs::findByPage(page, tab) ? tabs::windowHandle(tab.windowId) : nullptr;
}

const std::map<std::string, const wchar_t*> kSettingKeys = {
    { "font", L"AciFont" }, { "fileName", L"AciFileName" }, { "textSize", L"AciTextSize" }, { "spacing", L"AciSpacing" },
    { "width", L"AciWidth" }, { "tint", L"AciTint" }, { "bionic", L"AciBionic" },
};

void saveSetting(const std::string& name, const json::Value& value)
{
    auto key = kSettingKeys.find(name);
    if (key == kSettingKeys.end())
        return;
    std::string text;
    if (value.isBool())
        text = value.asBool() ? "1" : "0";
    else if (value.isNumber())
        text = std::to_string(static_cast<int>(value.asNumber()));
    else if (value.isString())
        text = value.asString().substr(0, 200);
    else
        return;
    util::writeSetting(key->second, util::fromUTF8(text));
}

} // namespace

void initialize()
{
    g_source = util::readFile(util::exeDirectory() + L"\\ui\\aci.js");
    while (!g_source.empty() && (g_source.back() == '\n' || g_source.back() == '\r' || g_source.back() == ' ' || g_source.back() == ';'))
        g_source.pop_back();
    unsigned char bytes[12];
    for (auto& byte : bytes)
        byte = static_cast<unsigned char>(rand() ^ (GetTickCount64() >> 3));
    static const char hex[] = "0123456789abcdef";
    for (unsigned char byte : bytes) {
        g_key += hex[byte >> 4];
        g_key += hex[byte & 15];
    }
}

bool canRun(const std::wstring& url)
{
    if (url.rfind(L"http://", 0) != 0 && url.rfind(L"https://", 0) != 0)
        return false;
    std::string site = siteOf(url);
    // Not extension pages (served from <id>.localhost).
    return !(site == "localhost" || (site.size() > 10 && site.compare(site.size() - 10, 10, ".localhost") == 0));
}

void toggle(WKPageRef page, const std::wstring& url, HWND)
{
    loadSites();
    auto siteView = g_siteViews.find(siteOf(url));
    run(page, url, "toggle", siteView != g_siteViews.end() ? siteView->second : lastView());
}

void pageLoaded(WKPageRef page, const std::wstring& url, HWND)
{
    if (!canRun(url))
        return;
    loadSites();
    auto siteView = g_siteViews.find(siteOf(url));
    std::string view = siteView != g_siteViews.end() ? siteView->second : setting(L"AciDefault", L"off");
    if (isView(view))
        run(page, url, "auto", view);
}

std::string handleMessage(WKPageRef page, const std::string& text)
{
    json::Value message;
    if (!json::parse(text, message) || !message.isObject())
        return "null";
    std::string op = message["op"].asString();
    tabs::Info tab;
    bool known = tabs::findByPage(page, tab);
    if (op == "view") {
        if (isView(message["view"].asString()))
            util::writeSetting(L"AciLastView", util::fromUTF8(message["view"].asString()));
        return "true";
    }
    if (op == "settings") {
        for (auto& [name, value] : message["settings"].members())
            saveSetting(name, value);
        // A newly picked bundled font: the view needs its files.
        std::string font = message["settings"]["font"].asString();
        return font.empty() ? std::string("true") : "{\"fonts\":" + fontsJSON(font) + "}";
    }
    if (op == "site") {
        if (!known)
            return "false";
        loadSites();
        std::string site = siteOf(tab.url);
        std::string view = message["view"].asString();
        if (site.empty())
            return "false";
        if (isView(view))
            g_siteViews[site] = view;
        else
            g_siteViews.erase(site);
        saveSites();
        return "true";
    }
    if (op == "systemFonts")
        return systemFontsJSON();
    if (op == "fontFile")
        return pickFontFile(ownerOf(page));
    return "true"; // "closed"
}

std::string handleFallbackMessage(WKPageRef page, const std::string& text)
{
    json::Value message;
    if (g_key.empty() || !json::parse(text, message) || message["token"].asString() != g_key)
        return "null";
    return handleMessage(page, text);
}

std::string handleCommand(const std::wstring& command, const std::wstring& argument, HWND owner)
{
    loadSites();
    if (command == L"set") {
        size_t newline = argument.find(L'\n');
        std::wstring key = argument.substr(0, newline), value = newline == std::wstring::npos ? L"" : argument.substr(newline + 1);
        if (key == L"default") {
            std::string view = util::toUTF8(value);
            util::writeSetting(L"AciDefault", isView(view) ? value : L"off");
        } else {
            json::Value parsed;
            json::parse(util::toUTF8(value), parsed);
            saveSetting(util::toUTF8(key), parsed);
        }
    } else if (command == L"removeSite") {
        g_siteViews.erase(util::toUTF8(argument));
        saveSites();
    } else if (command == L"fontFile") {
        pickFontFile(owner);
    } else if (command == L"systemFonts") {
        return systemFontsJSON();
    }
    // aci.get (and every change answers the new state)
    std::string json = "{\"default\":" + json::quote(setting(L"AciDefault", L"off")) + ",\"settings\":" + settingsJSON() + ",\"sites\":[";
    bool first = true;
    for (auto& [site, view] : g_siteViews) {
        json += std::string(first ? "" : ",") + "{\"site\":" + json::quote(site) + ",\"view\":" + json::quote(view) + "}";
        first = false;
    }
    return json + "]}";
}

} // namespace aci
