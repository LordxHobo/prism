// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Extensions.h"
#include "ExtensionsInternal.h"
#include "ExtensionServer.h"

#include "Blocker.h"
#include "DnrFilters.h"
#include "RequestFilter.h"
#include "Shields.h"
#include "BrowserWindow.h"
#include "Engine.h"
#include "Http.h"
#include "Json.h"
#include "Perf.h"
#include "Util.h"

#include <algorithm>
#include <ctime>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <memory>
#include <random>
#include <set>
#include <shobjidl.h>
#include <thread>

namespace fs = std::filesystem;

namespace extensions {

using namespace wk;

namespace {

// ---------------------------------------------------------------------------
// Data (Extension and friends are in ExtensionsInternal.h)

struct Job {
    std::string extensionId, source, sourceURL;
    fs::path staging;
};

std::function<void(const std::string&)> g_notify;
std::vector<std::unique_ptr<Extension>> g_extensions;
std::map<int, Job> g_jobs;
int g_nextJob = 1;
int g_blockGeneration = 0;
std::wstring g_pendingInstall;
std::string g_runtimeSource;

// ---------------------------------------------------------------------------
// Small helpers

fs::path dataRoot() { return fs::path(util::dataDirectory()); }
fs::path extensionsRoot() { return dataRoot() / L"Extensions"; }
fs::path stagingRoot() { return extensionsRoot() / L".staging"; }
fs::path extensionData(const std::string& id) { return dataRoot() / L"ExtensionData" / util::fromUTF8(id); }
fs::path registryFile() { return dataRoot() / L"extensions.json"; }

std::string readText(const fs::path& path) { return util::readFile(path.wstring()); }

std::string lowerASCII(std::string text)
{
    for (auto& c : text)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
}

std::wstring lowerWide(std::wstring text)
{
    for (auto& c : text)
        c = static_cast<wchar_t>(towlower(c));
    return text;
}

// A name that's safe as a folder name and as a JavaScript string.
std::string safeId(const std::string& raw)
{
    std::string out;
    for (char c : raw) {
        if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '@')
            out += c;
        else if (c == '{' || c == '}')
            continue;
        else
            out += '_';
    }
    while (!out.empty() && (out.back() == '.' || out.front() == '.')) {
        if (out.back() == '.')
            out.pop_back();
        else
            out.erase(out.begin());
    }
    if (out.size() > 80)
        out.resize(80);
    return out.empty() ? "extension" : out;
}

std::string randomToken()
{
    std::random_device device;
    std::string token;
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i)
        token += hex[device() & 15];
    return token;
}

std::vector<std::string> stringList(const json::Value& value)
{
    std::vector<std::string> out;
    if (value.isString())
        out.push_back(value.asString());
    for (auto& item : value.items()) {
        if (item.isString())
            out.push_back(item.asString());
    }
    return out;
}

void addUnique(std::vector<std::string>& list, const std::string& value)
{
    if (std::find(list.begin(), list.end(), value) == list.end())
        list.push_back(value);
}

// Paths inside an extension must stay inside it.
bool safeRelativePath(const std::string& path)
{
    if (path.empty() || path.find("..") != std::string::npos || path.find(':') != std::string::npos)
        return false;
    return true;
}

fs::path resolveFile(const fs::path& dir, std::string relative)
{
    while (!relative.empty() && (relative[0] == '/' || relative[0] == '\\'))
        relative.erase(relative.begin());
    if (!safeRelativePath(relative))
        return { };
    return dir / fs::path(util::fromUTF8(relative));
}

std::string userLocale()
{
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = { };
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH))
        return util::toUTF8(name);
    return "en-US";
}

// ---------------------------------------------------------------------------
// Reading an extension

// "__MSG_extName__" -> the translated text.
std::string resolveMessage(const std::string& text, const json::Value& messages)
{
    if (text.size() < 9 || text.compare(0, 6, "__MSG_") != 0 || text.compare(text.size() - 2, 2, "__") != 0)
        return text;
    std::string key = lowerASCII(text.substr(6, text.size() - 8));
    for (auto& member : messages.members()) {
        if (lowerASCII(member.first) == key)
            return member.second["message"].asString();
    }
    return text;
}

std::string mimeTypeFor(const fs::path& path)
{
    std::wstring ext = lowerWide(path.extension().wstring());
    if (ext == L".svg") return "image/svg+xml";
    if (ext == L".jpg" || ext == L".jpeg") return "image/jpeg";
    if (ext == L".gif") return "image/gif";
    if (ext == L".webp") return "image/webp";
    return "image/png";
}

std::string loadIcon(const fs::path& dir, const json::Value& icons)
{
    if (!icons.isObject())
        return { };
    // Prefer 48-128 px; otherwise the largest available.
    std::string best;
    int bestSize = -1;
    for (auto& member : icons.members()) {
        int size = atoi(member.first.c_str());
        bool better = bestSize < 0
            || (size >= 48 && size <= 128 && (bestSize < 48 || bestSize > 128 || size < bestSize))
            || ((bestSize < 48 || bestSize > 128) && size > bestSize && !(bestSize >= 48 && bestSize <= 128));
        if (better && member.second.isString()) {
            best = member.second.asString();
            bestSize = size;
        }
    }
    fs::path file = resolveFile(dir, best);
    if (file.empty())
        return { };
    std::error_code error;
    if (fs::file_size(file, error) > 512 * 1024 || error)
        return { };
    std::string bytes = readText(file);
    if (bytes.empty())
        return { };
    return "data:" + mimeTypeFor(file) + ";base64," + util::base64(bytes);
}

// The icon file closest to 32 px (toolbar buttons are 16 px at 2x).
std::string bestIconPath(const json::Value& icons)
{
    std::string best;
    int bestScore = 1 << 30;
    for (auto& member : icons.members()) {
        int size = atoi(member.first.c_str());
        int score = size >= 32 ? size - 32 : (32 - size) * 4;
        if (member.second.isString() && safeRelativePath(member.second.asString()) && score < bestScore) {
            bestScore = score;
            best = member.second.asString();
        }
    }
    return best;
}

bool isHostPermission(const std::string& permission)
{
    return permission == "<all_urls>" || permission.find("://") != std::string::npos;
}

// "Ctrl+Shift+Y" (and the other names Chrome accepts) -> a virtual key.
bool parseShortcut(const std::string& text, Command& command)
{
    command.ctrl = command.alt = command.shift = false;
    command.key = 0;
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= text.size()) {
        size_t plus = text.find('+', start);
        parts.push_back(text.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
        if (plus == std::string::npos)
            break;
        start = plus + 1;
    }
    if (parts.empty())
        return false;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::string& modifier = parts[i];
        if (modifier == "Ctrl" || modifier == "Command")
            command.ctrl = true;
        else if (modifier == "Alt")
            command.alt = true;
        else if (modifier == "Shift")
            command.shift = true;
        else
            return false; // MacCtrl and other Mac-only modifiers
    }
    const std::string& key = parts.back();
    static const std::map<std::string, unsigned> named = {
        { "Comma", VK_OEM_COMMA }, { "Period", VK_OEM_PERIOD }, { "Home", VK_HOME }, { "End", VK_END },
        { "PageUp", VK_PRIOR }, { "PageDown", VK_NEXT }, { "Space", VK_SPACE }, { "Insert", VK_INSERT },
        { "Delete", VK_DELETE }, { "Up", VK_UP }, { "Down", VK_DOWN }, { "Left", VK_LEFT }, { "Right", VK_RIGHT },
        { "MediaNextTrack", VK_MEDIA_NEXT_TRACK }, { "MediaPrevTrack", VK_MEDIA_PREV_TRACK },
        { "MediaPlayPause", VK_MEDIA_PLAY_PAUSE }, { "MediaStop", VK_MEDIA_STOP },
    };
    if (key.size() == 1 && ((key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= '0' && key[0] <= '9')))
        command.key = static_cast<unsigned char>(key[0]);
    else if (key.size() >= 2 && key[0] == 'F' && std::all_of(key.begin() + 1, key.end(), ::isdigit)) {
        int number = std::atoi(key.c_str() + 1);
        if (number >= 1 && number <= 12)
            command.key = VK_F1 + number - 1;
    } else if (auto found = named.find(key); found != named.end())
        command.key = found->second;
    if (!command.key)
        return false;
    bool media = command.key >= VK_MEDIA_NEXT_TRACK && command.key <= VK_MEDIA_PLAY_PAUSE;
    // Like Chrome: other keys need Ctrl or Alt.
    if (!media && !command.ctrl && !command.alt)
        return false;
    command.shortcut = text;
    return true;
}

std::vector<Command> parseCommands(const json::Value& commands)
{
    std::vector<Command> out;
    for (auto& [name, details] : commands.members()) {
        Command command;
        command.name = name;
        command.description = details["description"].asString();
        const json::Value& suggested = details["suggested_key"];
        std::string key;
        if (suggested.isString())
            key = suggested.asString();
        else if (suggested["windows"].isString())
            key = suggested["windows"].asString();
        else if (suggested["default"].isString())
            key = suggested["default"].asString();
        if (!key.empty() && !parseShortcut(key, command)) {
            command.shortcut.clear();
            command.key = 0;
        }
        out.push_back(std::move(command));
    }
    return out;
}


// Reads manifest.json and everything we need from an extension folder.
// Runs on worker threads too: touches only the file system.
bool loadExtension(const fs::path& dir, Extension& ext, std::string& error)
{
    ext.dir = dir;
    std::string text = readText(dir / L"manifest.json");
    if (text.empty()) {
        error = "This isn't a browser extension (it has no manifest.json).";
        return false;
    }
    json::Value manifest;
    std::string parseError;
    if (!json::parse(text, manifest, &parseError) || !manifest.isObject()) {
        error = "The extension's manifest.json is broken: " + parseError;
        return false;
    }
    ext.manifestVersion = static_cast<int>(manifest["manifest_version"].asNumber(2));
    if (ext.manifestVersion != 2 && ext.manifestVersion != 3) {
        error = "Unsupported manifest_version " + std::to_string(ext.manifestVersion) + ".";
        return false;
    }
    if (!manifest["name"].isString() || !manifest["version"].isString()) {
        error = "The extension's manifest.json has no name or version.";
        return false;
    }
    ext.manifestJSON = json::serialize(manifest);

    // Translations: the user's language if the extension has it.
    std::string defaultLocale = manifest["default_locale"].asString();
    std::string locale = userLocale();
    ext.uiLanguage = locale;
    std::vector<std::string> candidates;
    std::string underscored = locale;
    std::replace(underscored.begin(), underscored.end(), '-', '_');
    candidates.push_back(underscored);
    candidates.push_back(underscored.substr(0, underscored.find('_')));
    if (!defaultLocale.empty())
        candidates.push_back(defaultLocale);
    candidates.push_back("en");
    json::Value messages;
    for (auto& candidate : candidates) {
        if (!safeRelativePath(candidate))
            continue;
        std::string messagesText = readText(dir / L"_locales" / util::fromUTF8(candidate) / L"messages.json");
        if (!messagesText.empty() && json::parse(messagesText, messages) && messages.isObject())
            break;
        messages = json::Value();
    }
    ext.messagesJSON = messages.isObject() ? json::serialize(messages) : "{}";

    ext.name = resolveMessage(manifest["name"].asString(), messages);
    ext.version = manifest["version"].asString();
    ext.description = resolveMessage(manifest["description"].asString(), messages);
    ext.homepage = manifest["homepage_url"].asString();

    const json::Value& gecko = manifest["browser_specific_settings"]["gecko"].isObject()
        ? manifest["browser_specific_settings"]["gecko"] : manifest["applications"]["gecko"];
    ext.geckoId = gecko["id"].asString();

    // Permissions (MV2 mixes host patterns into "permissions").
    for (auto& permission : stringList(manifest["permissions"])) {
        if (isHostPermission(permission))
            addUnique(ext.hosts, permission);
        else
            addUnique(ext.permissions, permission);
    }
    for (auto& host : stringList(manifest["host_permissions"]))
        addUnique(ext.hosts, host);
    ext.unlimitedStorage = std::find(ext.permissions.begin(), ext.permissions.end(), "unlimitedStorage") != ext.permissions.end();

    ext.contentScripts.clear();
    for (auto& entry : manifest["content_scripts"].items()) {
        ContentScript script;
        script.matches = stringList(entry["matches"]);
        script.excludeMatches = stringList(entry["exclude_matches"]);
        script.includeGlobs = stringList(entry["include_globs"]);
        script.excludeGlobs = stringList(entry["exclude_globs"]);
        script.js = stringList(entry["js"]);
        script.css = stringList(entry["css"]);
        script.allFrames = entry["all_frames"].asBool();
        std::string runAt = entry["run_at"].asString();
        if (runAt == "document_start" || runAt == "document_end" || runAt == "document_idle")
            script.runAt = runAt;
        if (entry["world"].asString() == "MAIN")
            script.runAt = script.runAt; // runs in the page world anyway
        if (script.matches.empty() || (script.js.empty() && script.css.empty()))
            continue;
        for (auto& match : script.matches)
            addUnique(ext.hosts, match);
        ext.contentScripts.push_back(std::move(script));
    }

    ext.optionalPermissions.clear();
    ext.optionalHosts.clear();
    for (auto& permission : stringList(manifest["optional_permissions"])) {
        if (isHostPermission(permission))
            addUnique(ext.optionalHosts, permission);
        else
            addUnique(ext.optionalPermissions, permission);
    }
    for (auto& host : stringList(manifest["optional_host_permissions"]))
        addUnique(ext.optionalHosts, host);
    for (auto& item : ext.granted) {
        if (std::find(ext.optionalPermissions.begin(), ext.optionalPermissions.end(), item) != ext.optionalPermissions.end())
            addUnique(ext.permissions, item);
        else if (std::find(ext.optionalHosts.begin(), ext.optionalHosts.end(), item) != ext.optionalHosts.end())
            addUnique(ext.hosts, item);
    }

    // Background part.
    const json::Value& background = manifest["background"];
    ext.backgroundPage = background["page"].asString();
    ext.backgroundScripts = stringList(background["scripts"]);
    ext.serviceWorker = background["service_worker"].asString();
    ext.serviceWorkerModule = background["type"].asString() == "module";
    if (!ext.backgroundPage.empty() && !safeRelativePath(ext.backgroundPage))
        ext.backgroundPage.clear();

    // Toolbar button.
    const char* actionKey = manifest.has("action") ? "action" : manifest.has("browser_action") ? "browser_action" : manifest.has("page_action") ? "page_action" : nullptr;
    ext.hasAction = actionKey != nullptr;
    ext.actionTitle.clear();
    ext.actionPopup.clear();
    ext.actionIcon.clear();
    if (actionKey) {
        const json::Value& action = manifest[actionKey];
        ext.actionTitle = resolveMessage(action["default_title"].asString(), messages);
        ext.actionPopup = action["default_popup"].asString();
        const json::Value& icon = action["default_icon"];
        ext.actionIcon = icon.isString() ? icon.asString() : bestIconPath(icon);
    }
    if (ext.actionIcon.empty())
        ext.actionIcon = bestIconPath(manifest["icons"]);
    if (ext.actionTitle.empty())
        ext.actionTitle = ext.name;

    ext.webAccessible.clear();
    for (auto& entry : manifest["web_accessible_resources"].items()) {
        if (entry.isString())
            ext.webAccessible.push_back(entry.asString()); // MV2
        for (auto& resource : entry["resources"].items())
            ext.webAccessible.push_back(resource.asString()); // MV3
    }

    ext.optionsPage = manifest["options_ui"]["page"].asString();
    if (ext.optionsPage.empty())
        ext.optionsPage = manifest["options_page"].asString();

    // What Prism can't do yet, in plain words (shown on the Extensions page).
    ext.unsupported.clear();
    auto needs = [&](const char* permission) {
        return std::find(ext.permissions.begin(), ext.permissions.end(), permission) != ext.permissions.end();
    };
    if (needs("webRequestBlocking"))
        ext.unsupported.push_back("Changing request and response headers with webRequest");
    if (needs("offscreen"))
        ext.unsupported.push_back("Offscreen documents");
    if (needs("cookies"))
        ext.unsupported.push_back("Reading and changing cookies");
    if (needs("downloads"))
        ext.unsupported.push_back("Managing downloads");
    if (needs("history") || needs("bookmarks") || needs("topSites"))
        ext.unsupported.push_back("Bookmarks and history (coming in 0.6)");
    if (needs("notifications"))
        ext.unsupported.push_back("Desktop notifications");
    if (needs("nativeMessaging"))
        ext.unsupported.push_back("Talking to apps on your PC");
    if (needs("identity"))
        ext.unsupported.push_back("Signing in with Google or Firefox accounts");
    if (needs("proxy") || needs("privacy") || needs("webNavigation") || needs("debugger"))
        ext.unsupported.push_back("Some browser-level APIs (proxy, privacy, webNavigation, debugger)");
    ext.commands = parseCommands(manifest["commands"]);
    bool globalShortcut = false;
    for (auto& member : manifest["commands"].members())
        globalShortcut = globalShortcut || member.second["global"].asBool();
    if (globalShortcut)
        ext.unsupported.push_back("Shortcuts that work when Prism isn't in front");
    if (manifest.has("side_panel") || manifest.has("sidebar_action"))
        ext.unsupported.push_back("Side panel");
    if (manifest.has("chrome_url_overrides") || manifest.has("devtools_page") || manifest.has("omnibox"))
        ext.unsupported.push_back("Replacing Prism pages, developer tools panels or address-bar keywords");

    ext.hasBackground = manifest.has("background");
    ext.hasPopup = !manifest["action"]["default_popup"].asString().empty()
        || !manifest["browser_action"]["default_popup"].asString().empty()
        || !manifest["page_action"]["default_popup"].asString().empty();
    ext.hasOptions = !manifest["options_ui"]["page"].asString().empty() || !manifest["options_page"].asString().empty();

    // declarativeNetRequest rulesets (ad blockers). Use friendly names if the
    // extension ships them (uBlock Origin Lite does).
    ext.rulesets.clear();
    json::Value details;
    std::string detailsText = readText(dir / L"rulesets" / L"ruleset-details.json");
    if (!detailsText.empty())
        json::parse(detailsText, details);
    for (auto& resource : manifest["declarative_net_request"]["rule_resources"].items()) {
        Ruleset ruleset;
        ruleset.id = resource["id"].asString();
        ruleset.path = resource["path"].asString();
        ruleset.enabledByDefault = resource["enabled"].asBool();
        ruleset.name = ruleset.id;
        for (auto& detail : details.items()) {
            if (detail["id"].asString() == ruleset.id && detail["name"].isString())
                ruleset.name = detail["name"].asString();
        }
        if (!ruleset.id.empty() && !ruleset.path.empty())
            ext.rulesets.push_back(std::move(ruleset));
    }

    ext.iconDataURL = loadIcon(dir, manifest["icons"]);
    return true;
}

// Plain-language descriptions of what an extension may do.
std::vector<std::string> describePermissions(const Extension& ext)
{
    std::vector<std::string> out;
    bool allSites = false;
    std::vector<std::string> sites;
    for (auto& host : ext.hosts) {
        if (host == "<all_urls>" || host == "*://*/*" || host == "http://*/*" || host == "https://*/*" || host.find("://*/") != std::string::npos)
            allSites = true;
        else {
            size_t start = host.find("://");
            std::string site = start == std::string::npos ? host : host.substr(start + 3);
            site = site.substr(0, site.find('/'));
            if (site.rfind("*.", 0) == 0)
                site = site.substr(2);
            if (!site.empty())
                addUnique(sites, site);
        }
    }
    if (allSites)
        out.push_back("Read and change everything on all websites");
    else if (!sites.empty()) {
        std::string list;
        for (size_t i = 0; i < sites.size() && i < 6; ++i)
            list += (i ? ", " : "") + sites[i];
        if (sites.size() > 6)
            list += " and " + std::to_string(sites.size() - 6) + " more";
        out.push_back("Read and change your data on " + list);
    }

    static const std::pair<const char*, const char*> known[] = {
        { "tabs", "Read your browsing history" },
        { "history", "Read and change your browsing history" },
        { "bookmarks", "Read and change your bookmarks" },
        { "cookies", "Read and change cookies" },
        { "downloads", "Manage your downloads" },
        { "clipboardRead", "Read what you copy and paste" },
        { "clipboardWrite", "Change what you copy and paste" },
        { "nativeMessaging", "Talk to programs on your PC" },
        { "notifications", "Show notifications" },
        { "geolocation", "Know your location" },
        { "declarativeNetRequest", "Block content on any page" },
        { "declarativeNetRequestWithHostAccess", "Block content on pages it can access" },
        { "webRequest", "See your network traffic" },
        { "webRequestBlocking", "Block and change your network traffic" },
        { "proxy", "Change your proxy settings" },
        { "privacy", "Change your privacy settings" },
        { "management", "Manage your other extensions" },
        { "debugger", "Access the page debugger" },
        { "userScripts", "Run scripts you add on websites" },
        { "scripting", "Run its own scripts on pages it can access" },
    };
    for (auto& permission : ext.permissions) {
        for (auto& [name, text] : known) {
            if (permission == name)
                out.push_back(text);
        }
    }
    return out;
}

// What works in Prism today, for the Extensions page.
std::string stringArrayJSON(const std::vector<std::string>& list);

std::string supportJSON(const Extension& ext)
{
    size_t enabledRulesets = 0;
    for (auto& ruleset : ext.rulesets)
        enabledRulesets += ext.rulesetEnabled(ruleset) ? 1 : 0;
    bool something = !ext.contentScripts.empty() || !ext.rulesets.empty() || ext.hasBackground || ext.hasAction || ext.hasOptions;
    std::string level = !something ? "none" : !ext.unsupported.empty() ? "partial" : "full";
    std::string json = "{\"level\":\"" + level + "\"";
    json += ",\"contentScripts\":" + std::to_string(ext.contentScripts.size());
    json += ",\"rulesets\":" + std::to_string(ext.rulesets.size());
    json += ",\"rulesetsEnabled\":" + std::to_string(enabledRulesets);
    json += ",\"blockingDomains\":" + std::to_string(ext.blockingDomains);
    json += ",\"blockingRules\":" + std::to_string(ext.blockingRules);
    json += ",\"shortcuts\":[";
    bool firstShortcut = true;
    for (auto& command : ext.commands) {
        if (command.shortcut.empty())
            continue;
        std::string label = command.description;
        if (label.empty())
            label = command.name.rfind("_execute_", 0) == 0 ? "Open the toolbar popup" : command.name;
        json += std::string(firstShortcut ? "" : ",") + "{\"keys\":" + json::quote(command.shortcut) + ",\"label\":" + json::quote(label) + "}";
        firstShortcut = false;
    }
    json += "]";
    json += std::string(",\"background\":") + (ext.hasBackground ? "true" : "false");
    json += std::string(",\"popup\":") + (!ext.actionPopup.empty() ? "true" : "false");
    json += std::string(",\"action\":") + (ext.hasAction ? "true" : "false");
    json += std::string(",\"options\":") + (!ext.optionsPage.empty() ? "true" : "false");
    json += ",\"unsupported\":" + stringArrayJSON(ext.unsupported);
    json += "}";
    return json;
}

std::string stringArrayJSON(const std::vector<std::string>& list)
{
    std::string json = "[";
    for (size_t i = 0; i < list.size(); ++i)
        json += (i ? "," : "") + json::quote(list[i]);
    return json + "]";
}

std::string extensionJSON(const Extension& ext)
{
    std::string json = "{\"id\":" + json::quote(ext.id);
    json += ",\"name\":" + json::quote(ext.name);
    json += ",\"version\":" + json::quote(ext.version);
    json += ",\"description\":" + json::quote(ext.description);
    json += ",\"homepage\":" + json::quote(ext.homepage);
    json += ",\"manifestVersion\":" + std::to_string(ext.manifestVersion);
    json += ",\"source\":" + json::quote(ext.source);
    json += ",\"sourceURL\":" + json::quote(ext.sourceURL);
    json += std::string(",\"enabled\":") + (ext.enabled ? "true" : "false");
    json += ",\"icon\":" + json::quote(ext.iconDataURL);
    json += ",\"permissions\":" + stringArrayJSON(describePermissions(ext));
    json += ",\"support\":" + supportJSON(ext);
    json += ",\"errors\":[";
    for (size_t i = 0; i < ext.errors.size(); ++i) {
        auto& entry = ext.errors[i];
        json += std::string(i ? "," : "") + "{\"where\":" + json::quote(entry.where) + ",\"message\":" + json::quote(entry.message)
            + ",\"source\":" + json::quote(entry.source) + ",\"time\":" + std::to_string(entry.time) + ",\"count\":" + std::to_string(entry.count) + "}";
    }
    json += "]";
    json += ",\"rulesets\":[";
    for (size_t i = 0; i < ext.rulesets.size(); ++i) {
        auto& ruleset = ext.rulesets[i];
        json += (i ? "," : "") + std::string("{\"id\":") + json::quote(ruleset.id) + ",\"name\":" + json::quote(ruleset.name)
            + ",\"enabled\":" + (ext.rulesetEnabled(ruleset) ? "true" : "false") + "}";
    }
    json += "]}";
    return json;
}

Extension* findExtension(const std::string& id)
{
    for (auto& ext : g_extensions) {
        if (ext->id == id)
            return ext.get();
    }
    return nullptr;
}

std::string listJSON()
{
    std::string json = "{\"extensions\":[";
    for (size_t i = 0; i < g_extensions.size(); ++i)
        json += (i ? "," : "") + extensionJSON(*g_extensions[i]);
    json += "],\"blocked\":" + std::to_string(blocker::blockedCount());
    json += ",\"blockingActive\":" + std::string(engine::blockingProxyActive() ? "true" : "false");
    if (!g_pendingInstall.empty()) {
        json += ",\"pendingInstall\":" + json::quote(util::toUTF8(g_pendingInstall));
        g_pendingInstall.clear();
    }
    json += "}";
    return json;
}

void notifyList()
{
    if (g_notify)
        g_notify("{\"type\":\"changed\",\"list\":" + listJSON() + "}");
}

// ---------------------------------------------------------------------------
// Registry (which extensions are installed, on/off, ruleset choices)

void saveRegistry()
{
    std::string json = "{\"extensions\":[";
    for (size_t i = 0; i < g_extensions.size(); ++i) {
        auto& ext = *g_extensions[i];
        json += (i ? ",\n" : "\n") + std::string("{\"id\":") + json::quote(ext.id)
            + ",\"enabled\":" + (ext.enabled ? "true" : "false")
            + ",\"source\":" + json::quote(ext.source)
            + ",\"sourceURL\":" + json::quote(ext.sourceURL)
            + ",\"granted\":" + stringArrayJSON(ext.granted)
            + ",\"rulesets\":{";
        bool first = true;
        for (auto& [id, on] : ext.rulesetChoices) {
            json += (first ? "" : ",") + json::quote(id) + ":" + (on ? "true" : "false");
            first = false;
        }
        json += "}}";
    }
    json += "\n]}\n";
    util::writeFileAtomically(registryFile().wstring(), json);
}

void loadRegistry()
{
    g_extensions.clear();
    json::Value registry;
    if (!json::parse(readText(registryFile()), registry))
        return;
    for (auto& entry : registry["extensions"].items()) {
        auto ext = std::make_unique<Extension>();
        ext->id = safeId(entry["id"].asString());
        ext->enabled = entry["enabled"].asBool(true);
        ext->source = entry["source"].asString();
        ext->sourceURL = entry["sourceURL"].asString();
        for (auto& member : entry["rulesets"].members())
            ext->rulesetChoices[member.first] = member.second.asBool();
        ext->granted = stringList(entry["granted"]);
        std::string error;
        if (!loadExtension(extensionsRoot() / util::fromUTF8(ext->id), *ext, error))
            continue; // folder missing or broken: skip it
        ext->token = randomToken();
        g_extensions.push_back(std::move(ext));
    }
}

// ---------------------------------------------------------------------------
// Content scripts

std::string loadRuntimeSource()
{
    std::string source = util::readFile(util::exeDirectory() + L"\\ui\\extension-runtime.js");
    // The file is a function expression; drop the trailing newline(s).
    while (!source.empty() && (source.back() == '\n' || source.back() == '\r' || source.back() == ' '))
        source.pop_back();
    return source;
}

// Concatenated text of an entry's files (in order).
std::string joinFiles(const fs::path& dir, const std::vector<std::string>& files, const char* separator)
{
    std::string out;
    for (auto& file : files) {
        fs::path path = resolveFile(dir, file);
        if (path.empty())
            continue;
        out += readText(path);
        out += separator;
    }
    return out;
}

std::string buildUserScript(const Extension& ext)
{
    std::string config = "{\"id\":" + json::quote(ext.id);
    config += ",\"name\":" + json::quote(ext.name);
    config += ",\"token\":" + json::quote(ext.token);
    config += ",\"uiLanguage\":" + json::quote(ext.uiLanguage);
    config += ",\"base\":" + json::quote(internal::baseURL(ext));
    config += ",\"manifest\":" + ext.manifestJSON;
    config += ",\"messages\":" + ext.messagesJSON;
    config += ",\"entries\":[";
    std::string scripts;
    for (size_t i = 0; i < ext.contentScripts.size(); ++i) {
        auto& entry = ext.contentScripts[i];
        config += (i ? "," : "") + std::string("{\"matches\":") + stringArrayJSON(entry.matches)
            + ",\"excludeMatches\":" + stringArrayJSON(entry.excludeMatches)
            + ",\"includeGlobs\":" + stringArrayJSON(entry.includeGlobs)
            + ",\"excludeGlobs\":" + stringArrayJSON(entry.excludeGlobs)
            + ",\"allFrames\":" + (entry.allFrames ? "true" : "false")
            + ",\"runAt\":" + json::quote(entry.runAt)
            + ",\"css\":" + json::quote(joinFiles(ext.dir, entry.css, "\n")) + "}";
        // The extension's own code, wrapped so it runs only on matching pages.
        // (Install checks that each script is a complete, valid program, so
        // it can't break out of this wrapper.)
        if (entry.js.empty())
            scripts += i ? ",\nnull" : "null";
        else
            scripts += std::string(i ? ",\n" : "") + "function (chrome, browser) {\n" + joinFiles(ext.dir, entry.js, "\n;\n") + "\n}";
    }
    config += "]";
    // WebKit has no requestIdleCallback. The runtime adds one to the page
    // only when the extension's scripts use it, so websites don't get an
    // extra feature to detect when they don't need to.
    if (scripts.find("IdleCallback") != std::string::npos)
        config += ",\"idleCallback\":true";
    config += "}";
    return "/* Prism: " + ext.name + " */\n" + g_runtimeSource + "(" + config + ", [\n" + scripts + "\n]);\n";
}

void rebuildUserScripts()
{
    std::vector<std::string> sources;
    if (!g_runtimeSource.empty()) {
        for (auto& ext : g_extensions) {
            if (ext->enabled && !ext->contentScripts.empty())
                sources.push_back(buildUserScript(*ext));
        }
    }
    engine::setUserScripts("extensions", std::move(sources));
}

// ---------------------------------------------------------------------------
// Blocking (declarativeNetRequest rulesets -> Blocker)

void rebuildBlocking()
{
    struct Work {
        std::string extensionId, owner;
        std::vector<fs::path> files;
    };
    std::vector<Work> work;
    for (auto& ext : g_extensions) {
        if (!ext->enabled)
            continue;
        Work item { ext->id, ext->name, { } };
        for (auto& ruleset : ext->rulesets) {
            if (!ext->rulesetEnabled(ruleset))
                continue;
            fs::path file = resolveFile(ext->dir, ruleset.path);
            if (!file.empty())
                item.files.push_back(file);
        }
        if (!item.files.empty())
            work.push_back(std::move(item));
    }

    int generation = ++g_blockGeneration;
    // Filter lists can be tens of MB: parse them off the UI thread.
    std::thread([work = std::move(work), generation] {
        auto lists = std::make_shared<blocker::Lists>();
        std::map<std::string, size_t> counts, ruleCounts;
        std::vector<std::shared_ptr<requestfilter::ExtensionRules>> pathRules;
        for (auto& item : work) {
            uint16_t owner = static_cast<uint16_t>(lists->owners.size());
            lists->owners.push_back(item.owner);
            std::vector<std::string> blocked, allowed;
            dnr::Conversion conversion;
            for (auto& file : item.files) {
                json::Value rules;
                if (json::parse(readText(file), rules)) {
                    blocker::collectDomains(rules, blocked, allowed);
                    dnr::convert(rules, conversion);
                }
            }
            // The same rules with paths, types and sites, for the page filter.
            if (void* engine = shields::buildEngine(conversion.filters)) {
                ruleCounts[item.extensionId] = conversion.converted;
                auto rules = std::make_shared<requestfilter::ExtensionRules>();
                rules->owner = item.owner;
                rules->engine = engine;
                pathRules.push_back(std::move(rules));
            }
            size_t before = lists->blocked.size();
            for (auto& domain : blocked)
                lists->blocked.emplace(domain, owner);
            for (auto& domain : allowed)
                lists->allowed.insert(domain);
            counts[item.extensionId] = lists->blocked.size() - before;
        }
        util::postToMainThread([lists, counts, ruleCounts, generation, pathRules = std::move(pathRules)]() mutable {
            if (generation != g_blockGeneration)
                return; // a newer rebuild is on its way
            requestfilter::setExtensionRules(std::move(pathRules));
            for (auto& ext : g_extensions) {
                auto found = counts.find(ext->id);
                ext->blockingDomains = found == counts.end() ? 0 : found->second;
                auto rules = ruleCounts.find(ext->id);
                ext->blockingRules = rules == ruleCounts.end() ? 0 : rules->second;
            }
            blocker::setLists("extensions", lists);
            engine::updateBlockingProxy();
            notifyList();
        });
    }).detach();
}

void applyAll()
{
    rebuildUserScripts();
    rebuildBlocking();
    host::extensionsChanged();
}

// ---------------------------------------------------------------------------
// Storage (chrome.storage.local / sync / session)

struct Store {
    bool loaded { false };
    std::map<std::string, std::string> items; // key -> raw JSON value
    size_t bytes { 0 };
};

std::map<std::string, std::map<std::string, Store>> g_storage;

size_t quotaFor(const Extension& ext, const std::string& area)
{
    if (area == "sync")
        return 100 * 1024;
    if (area == "local" && ext.unlimitedStorage)
        return 512u * 1024 * 1024;
    return 10 * 1024 * 1024;
}

// Storage files can be big: unlimitedStorage allows 512 MB of data.
constexpr unsigned long long kMaxStorageFile = 1024ull * 1024 * 1024;

fs::path storageFile(const Extension& ext, const std::string& area)
{
    return extensionData(ext.id) / util::fromUTF8("storage-" + area + ".json");
}

Store& storeFor(const Extension& ext, const std::string& area)
{
    Store& store = g_storage[ext.id][area];
    if (store.loaded)
        return store;
    store.loaded = true;
    if (area == "session")
        return store; // memory only
    fs::path file = storageFile(ext, area);
    std::string text = util::readFile(file.wstring(), kMaxStorageFile);
    json::Value saved;
    if (json::parse(text, saved) && saved.isObject()) {
        for (auto& member : saved.members()) {
            std::string raw = text.substr(member.second.start(), member.second.end() - member.second.start());
            store.bytes += member.first.size() + raw.size();
            store.items[member.first] = std::move(raw);
        }
    } else {
        // The file is there but couldn't be read. Keep it aside instead of
        // overwriting it with the extension's fresh, empty settings.
        std::error_code error;
        if (fs::exists(file, error) && fs::file_size(file, error) > 0) {
            fs::path aside = file;
            aside.replace_filename(util::fromUTF8("storage-" + area + ".unreadable.json"));
            fs::rename(file, aside, error);
        }
    }
    return store;
}

std::string serializeStore(const Store& store)
{
    size_t length = 2;
    for (auto& [key, raw] : store.items)
        length += key.size() + raw.size() + 4;
    std::string json;
    json.reserve(length);
    json += '{';
    bool first = true;
    for (auto& [key, raw] : store.items) {
        if (!first)
            json += ',';
        json += json::quote(key);
        json += ':';
        json += raw;
        first = false;
    }
    json += '}';
    return json;
}

// Saving. Changes are written a moment later on a background thread, so a
// big store (AdNauseam keeps 10+ MB) isn't rewritten on every small change.
// The newest contents of each file win; flushStorage() writes everything
// still waiting when Prism exits.

std::set<std::pair<std::string, std::string>> g_dirtyStores; // extension ID, area
UINT_PTR g_saveTimer = 0;
std::mutex g_pendingMutex;                 // guards g_pendingWrites, g_writerRunning
std::mutex g_writeMutex;                   // held while taking and writing one file
std::map<std::wstring, std::string> g_pendingWrites; // file -> contents
bool g_writerRunning = false;

bool writeOnePending()
{
    std::lock_guard<std::mutex> writing(g_writeMutex);
    std::wstring path;
    std::string contents;
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        if (g_pendingWrites.empty())
            return false;
        auto next = g_pendingWrites.begin();
        path = next->first;
        contents = std::move(next->second);
        g_pendingWrites.erase(next);
    }
    util::ensureDirectory(fs::path(path).parent_path().wstring());
    util::writeFileAtomically(path, contents);
    return true;
}

void queueWrite(const fs::path& file, std::string contents)
{
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    g_pendingWrites[file.wstring()] = std::move(contents);
    if (g_writerRunning)
        return;
    g_writerRunning = true;
    std::thread([] {
        for (;;) {
            if (writeOnePending())
                continue;
            std::lock_guard<std::mutex> lock(g_pendingMutex);
            if (g_pendingWrites.empty()) {
                g_writerRunning = false;
                return;
            }
        }
    }).detach();
}

void saveDirtyStores()
{
    if (g_saveTimer) {
        KillTimer(nullptr, g_saveTimer);
        g_saveTimer = 0;
    }
    perf::Scope busy("saving extension storage");
    auto dirty = std::move(g_dirtyStores);
    g_dirtyStores.clear();
    for (auto& [id, area] : dirty) {
        Extension* ext = findExtension(id);
        auto extStores = g_storage.find(id);
        if (!ext || extStores == g_storage.end())
            continue;
        auto store = extStores->second.find(area);
        if (store != extStores->second.end())
            queueWrite(storageFile(*ext, area), serializeStore(store->second));
    }
}

void saveStore(const Extension& ext, const std::string& area, const Store&)
{
    if (area == "session")
        return;
    g_dirtyStores.insert({ ext.id, area });
    if (!g_saveTimer)
        g_saveTimer = SetTimer(nullptr, 0, 1000, [](HWND, UINT, UINT_PTR, DWORD) { saveDirtyStores(); });
    if (!g_saveTimer)
        saveDirtyStores();
}

// Drops anything waiting to be saved for an extension that's being removed.
void forgetPendingStorage(const Extension& ext)
{
    for (auto it = g_dirtyStores.begin(); it != g_dirtyStores.end();)
        it = it->first == ext.id ? g_dirtyStores.erase(it) : std::next(it);
    std::lock_guard<std::mutex> writing(g_writeMutex);
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    for (const char* area : { "local", "sync" })
        g_pendingWrites.erase(storageFile(ext, area).wstring());
}

std::string rawOf(const std::string& text, const json::Value& value)
{
    return text.substr(value.start(), value.end() - value.start());
}

std::string storageOperation(Extension& ext, const std::string& op, const std::string& text, const json::Value& message, std::string& error)
{
    std::string area = message["area"].asString();
    if (area != "local" && area != "sync" && area != "session") {
        error = "Unknown storage area";
        return { };
    }
    Store& store = storeFor(ext, area);

    if (op == "storage.get") {
        const json::Value& keys = message["keys"];
        std::string json = "{";
        bool first = true;
        auto add = [&](const std::string& key, const std::string& raw) {
            json += (first ? "" : ",") + json::quote(key) + ":" + raw;
            first = false;
        };
        if (keys.isNull()) {
            for (auto& [key, raw] : store.items)
                add(key, raw);
        } else {
            for (auto& key : keys.items()) {
                auto found = store.items.find(key.asString());
                if (found != store.items.end())
                    add(found->first, found->second);
            }
        }
        return json + "}";
    }

    if (op == "storage.bytes")
        return std::to_string(store.bytes);

    // Changes, for storage.onChanged.
    std::string changes = "{";
    bool firstChange = true;
    auto change = [&](const std::string& key, const std::string* oldRaw, const std::string* newRaw) {
        changes += (firstChange ? "" : ",") + json::quote(key) + ":{";
        if (oldRaw)
            changes += "\"oldValue\":" + *oldRaw;
        if (newRaw)
            changes += std::string(oldRaw ? "," : "") + "\"newValue\":" + *newRaw;
        changes += "}";
        firstChange = false;
    };

    if (op == "storage.set") {
        const json::Value& items = message["items"];
        if (!items.isObject()) {
            error = "Invalid items";
            return { };
        }
        size_t newBytes = store.bytes;
        for (auto& member : items.members()) {
            auto found = store.items.find(member.first);
            if (found != store.items.end())
                newBytes -= member.first.size() + found->second.size();
            newBytes += member.first.size() + (member.second.end() - member.second.start());
        }
        if (newBytes > quotaFor(ext, area)) {
            error = "QUOTA_BYTES quota exceeded";
            return { };
        }
        for (auto& member : items.members()) {
            std::string raw = rawOf(text, member.second);
            auto found = store.items.find(member.first);
            if (found != store.items.end()) {
                std::string old = found->second;
                change(member.first, &old, &raw);
                found->second = raw;
            } else {
                change(member.first, nullptr, &raw);
                store.items.emplace(member.first, raw);
            }
        }
        store.bytes = newBytes;
    } else if (op == "storage.remove") {
        for (auto& key : message["keys"].items()) {
            auto found = store.items.find(key.asString());
            if (found == store.items.end())
                continue;
            change(found->first, &found->second, nullptr);
            store.bytes -= found->first.size() + found->second.size();
            store.items.erase(found);
        }
    } else if (op == "storage.clear") {
        for (auto& [key, raw] : store.items)
            change(key, &raw, nullptr);
        store.items.clear();
        store.bytes = 0;
    } else {
        error = "Unknown storage operation";
        return { };
    }
    saveStore(ext, area, store);
    return "{\"changes\":" + changes + "}}";
}

void reply(WKCompletionListenerRef listener, const std::string& json)
{
    WKCompletionListenerComplete(listener, wk::stringUTF8(json).get());
}

// ---------------------------------------------------------------------------
// Installing

enum class InputKind { ChromeStore, FirefoxStore, File, Folder, Unknown };

// Pulls the 32-letter extension ID out of a Chrome Web Store link.
std::string chromeStoreId(const std::wstring& url)
{
    std::wstring lower = lowerWide(url);
    if (lower.find(L"chromewebstore.google.com/") == std::wstring::npos && lower.find(L"chrome.google.com/webstore/") == std::wstring::npos)
        return { };
    std::string text = util::toUTF8(lower);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find_first_of("/?#&=", pos);
        std::string segment = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (segment.size() == 32 && std::all_of(segment.begin(), segment.end(), [](char c) { return c >= 'a' && c <= 'p'; }))
            return segment;
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }
    return { };
}

// The add-on's "slug" from an addons.mozilla.org link.
std::string firefoxSlug(const std::wstring& url)
{
    std::string text = util::toUTF8(url);
    std::string lower = lowerASCII(text);
    if (lower.find("addons.mozilla.org/") == std::string::npos)
        return { };
    size_t at = lower.find("/addon/");
    if (at == std::string::npos)
        return { };
    std::string slug = text.substr(at + 7);
    slug = slug.substr(0, slug.find_first_of("/?#"));
    return slug;
}

std::wstring systemTool(const wchar_t* name)
{
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    return std::wstring(system) + L"\\" + name;
}

// Unzips with Windows' built-in tar.exe (Windows 10 1803 and later).
bool unzip(const fs::path& zip, const fs::path& destination, std::string& error)
{
    std::wstring tar = systemTool(L"tar.exe");
    if (GetFileAttributesW(tar.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = "Windows' tar.exe is missing (it comes with Windows 10 version 1803 and later).";
        return false;
    }
    std::wstring command = L"\"" + tar + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + destination.wstring() + L"\"";
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = { };
    if (!CreateProcessW(tar.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        error = "Couldn't start tar.exe to unpack the extension.";
        return false;
    }
    DWORD wait = WaitForSingleObject(process.hProcess, 120000);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    if (wait == WAIT_TIMEOUT)
        TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0) {
        error = "The extension package couldn't be unpacked (it may be damaged).";
        return false;
    }
    return true;
}

uint32_t readU32(const std::string& bytes, size_t offset)
{
    if (offset + 4 > bytes.size())
        return 0;
    return static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset]))
        | static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8
        | static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16
        | static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24;
}

// A .crx is a small header followed by a normal zip; .xpi is a plain zip.
bool zipFromPackage(const std::string& package, std::string& zip, std::string& error)
{
    if (package.size() > 4 && package.compare(0, 4, "PK\x03\x04") == 0) {
        zip = package;
        return true;
    }
    if (package.size() < 16 || package.compare(0, 4, "Cr24") != 0) {
        error = "This file isn't a .crx, .xpi or .zip extension package.";
        return false;
    }
    uint32_t version = readU32(package, 4);
    size_t offset;
    if (version == 3)
        offset = 12 + static_cast<size_t>(readU32(package, 8));
    else if (version == 2)
        offset = 16 + static_cast<size_t>(readU32(package, 8)) + readU32(package, 12);
    else {
        error = "Unsupported .crx version.";
        return false;
    }
    if (offset >= package.size() || package.compare(offset, 4, "PK\x03\x04") != 0) {
        error = "The .crx file is damaged.";
        return false;
    }
    zip = package.substr(offset);
    return true;
}

struct Prepared {
    bool ok { false };
    std::string error;
    std::string extensionId, source, sourceURL;
    fs::path staging;
    std::string previewJSON;
};

// Worker thread: get the extension into a staging folder and describe it.
// Tells prism://extensions what an install is doing ("Downloading 1.2 of 3.4 MB").
void reportProgress(int job, const std::string& step, uint64_t received = 0, uint64_t total = 0)
{
    std::string event = "{\"type\":\"progress\",\"job\":" + std::to_string(job) + ",\"step\":" + json::quote(step)
        + ",\"received\":" + std::to_string(received) + ",\"total\":" + std::to_string(total) + "}";
    util::postToMainThread([event] {
        if (g_notify)
            g_notify(event);
    });
}

Prepared prepare(int job, std::wstring input)
{
    Prepared result;
    // Trim spaces and quotes (from "Copy as path").
    while (!input.empty() && (input.back() == L' ' || input.back() == L'"'))
        input.pop_back();
    while (!input.empty() && (input.front() == L' ' || input.front() == L'"'))
        input.erase(input.begin());

    result.staging = stagingRoot() / std::to_wstring(job);
    std::error_code ignored;
    fs::remove_all(result.staging, ignored);
    fs::create_directories(result.staging, ignored);

    std::string package; // .crx / .xpi / .zip bytes
    std::string suggestedId;
    std::wstring httpError;

    if (std::string id = chromeStoreId(input); !id.empty()) {
        result.source = "chrome";
        result.sourceURL = "https://chromewebstore.google.com/detail/" + id;
        suggestedId = id;
        std::wstring url = L"https://clients2.google.com/service/update2/crx?response=redirect&prodversion=142.0.0.0&acceptformat=crx2,crx3&x=id%3D"
            + util::fromUTF8(id) + L"%26installsource%3Dondemand%26uc";
        reportProgress(job, "Contacting the Chrome Web Store\u2026");
        auto onProgress = [job](uint64_t received, uint64_t total) { reportProgress(job, "Downloading from the Chrome Web Store", received, total); };
        if (!http::get(url, package, httpError, 256 * 1024 * 1024, onProgress)) {
            result.error = "Couldn't download from the Chrome Web Store: " + util::toUTF8(httpError);
            return result;
        }
    } else if (std::string slug = firefoxSlug(input); !slug.empty()) {
        result.source = "firefox";
        result.sourceURL = "https://addons.mozilla.org/firefox/addon/" + slug + "/";
        std::string apiBody;
        reportProgress(job, "Looking it up on Firefox Add-ons\u2026");
        if (!http::get(L"https://addons.mozilla.org/api/v5/addons/addon/" + util::fromUTF8(slug) + L"/", apiBody, httpError, 4 * 1024 * 1024)) {
            result.error = "Couldn't find that add-on on Firefox Add-ons: " + util::toUTF8(httpError);
            return result;
        }
        json::Value info;
        json::parse(apiBody, info);
        // v5 has current_version.file; older API versions used a "files" array.
        std::string fileURL = info["current_version"]["file"]["url"].asString();
        if (fileURL.empty())
            fileURL = info["current_version"]["files"][0]["url"].asString();
        if (fileURL.rfind("https://", 0) != 0) {
            result.error = "Firefox Add-ons didn't return a download for this add-on.";
            return result;
        }
        suggestedId = "amo-" + safeId(slug);
        auto onProgress = [job](uint64_t received, uint64_t total) { reportProgress(job, "Downloading from Firefox Add-ons", received, total); };
        if (!http::get(util::fromUTF8(fileURL), package, httpError, 256 * 1024 * 1024, onProgress)) {
            result.error = "Couldn't download from Firefox Add-ons: " + util::toUTF8(httpError);
            return result;
        }
    } else if (DWORD attributes = GetFileAttributesW(input.c_str()); attributes != INVALID_FILE_ATTRIBUTES) {
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            // An unpacked extension folder (for developers).
            result.source = "folder";
            result.sourceURL = util::toUTF8(input);
            std::error_code copyError;
            fs::copy(fs::path(input), result.staging, fs::copy_options::recursive | fs::copy_options::overwrite_existing, copyError);
            if (copyError) {
                result.error = "Couldn't copy the folder: " + copyError.message();
                return result;
            }
        } else {
            result.source = "file";
            result.sourceURL = util::toUTF8(input);
            package = util::readFile(input);
            if (package.empty()) {
                result.error = "Couldn't read that file.";
                return result;
            }
        }
    } else {
        result.error = "Paste a link to an extension in the Chrome Web Store (chromewebstore.google.com) or on Firefox Add-ons (addons.mozilla.org), or choose a file.";
        return result;
    }

    if (!package.empty()) {
        reportProgress(job, "Unpacking\u2026");
        std::string zip;
        if (!zipFromPackage(package, zip, result.error))
            return result;
        fs::path zipFile = stagingRoot() / (std::to_wstring(job) + L".zip");
        if (!util::writeFileAtomically(zipFile.wstring(), zip)) {
            result.error = "Couldn't save the download.";
            return result;
        }
        bool unpacked = unzip(zipFile, result.staging, result.error);
        fs::remove(zipFile, ignored);
        if (!unpacked)
            return result;
    }

    // Some packages put everything in one subfolder.
    if (!fs::exists(result.staging / L"manifest.json")) {
        for (auto& entry : fs::directory_iterator(result.staging, ignored)) {
            if (entry.is_directory() && fs::exists(entry.path() / L"manifest.json")) {
                fs::path inner = entry.path();
                fs::path moved = stagingRoot() / (std::to_wstring(job) + L"-inner");
                fs::rename(inner, moved, ignored);
                fs::remove_all(result.staging, ignored);
                fs::rename(moved, result.staging, ignored);
                break;
            }
        }
    }

    reportProgress(job, "Checking what it can do\u2026");
    Extension ext;
    if (!loadExtension(result.staging, ext, result.error))
        return result;
    ext.source = result.source;

    if (!ext.geckoId.empty() && result.source != "chrome")
        suggestedId = ext.geckoId;
    if (suggestedId.empty())
        suggestedId = "local-" + ext.name;
    result.extensionId = safeId(suggestedId);

    // The page checks each content script is a complete program before the
    // install goes ahead (see buildUserScript).
    std::string scripts = "[";
    bool first = true;
    for (auto& entry : ext.contentScripts) {
        if (entry.js.empty())
            continue;
        std::string names;
        for (auto& file : entry.js)
            names += (names.empty() ? "" : ", ") + file;
        scripts += (first ? "" : ",") + std::string("{\"files\":") + json::quote(names)
            + ",\"source\":" + json::quote(joinFiles(ext.dir, entry.js, "\n;\n")) + "}";
        first = false;
    }
    scripts += "]";

    result.previewJSON = "{\"id\":" + json::quote(result.extensionId)
        + ",\"name\":" + json::quote(ext.name)
        + ",\"version\":" + json::quote(ext.version)
        + ",\"description\":" + json::quote(ext.description)
        + ",\"icon\":" + json::quote(ext.iconDataURL)
        + ",\"source\":" + json::quote(result.source)
        + ",\"permissions\":" + stringArrayJSON(describePermissions(ext))
        + ",\"support\":" + supportJSON(ext)
        + ",\"scripts\":" + scripts + "}";
    result.ok = true;
    return result;
}

void startInstall(int job, const std::wstring& input)
{
    std::thread([job, input] {
        Prepared prepared = prepare(job, input);
        util::postToMainThread([job, prepared] {
            if (!prepared.ok) {
                std::error_code ignored;
                fs::remove_all(prepared.staging, ignored);
                if (g_notify)
                    g_notify("{\"type\":\"failed\",\"job\":" + std::to_string(job) + ",\"error\":" + json::quote(prepared.error) + "}");
                return;
            }
            g_jobs[job] = Job { prepared.extensionId, prepared.source, prepared.sourceURL, prepared.staging };
            std::string existing;
            if (Extension* ext = findExtension(prepared.extensionId))
                existing = ",\"replaces\":" + json::quote(ext->version);
            if (g_notify)
                g_notify("{\"type\":\"prepared\",\"job\":" + std::to_string(job) + existing + ",\"preview\":" + prepared.previewJSON + "}");
        });
    }).detach();
}

std::string confirmInstall(int job)
{
    auto found = g_jobs.find(job);
    if (found == g_jobs.end())
        return "{\"error\":\"This install has expired. Try again.\"}";
    Job details = found->second;
    g_jobs.erase(found);

    fs::path target = extensionsRoot() / util::fromUTF8(details.extensionId);
    host::stop(details.extensionId);
    std::error_code error;
    fs::remove_all(target, error);
    error.clear();
    fs::rename(details.staging, target, error);
    if (error) {
        fs::remove_all(details.staging, error);
        return "{\"error\":\"Couldn't install the extension (its files are in use?). Try again.\"}";
    }

    Extension* existing = findExtension(details.extensionId);
    std::string previousVersion = existing ? existing->version : "";
    auto ext = std::make_unique<Extension>();
    ext->id = details.extensionId;
    ext->source = details.source;
    ext->sourceURL = details.sourceURL;
    ext->enabled = existing ? existing->enabled : true;
    if (existing)
        ext->rulesetChoices = existing->rulesetChoices;
    if (existing)
        ext->granted = existing->granted;
    std::string loadError;
    if (!loadExtension(target, *ext, loadError))
        return "{\"error\":" + json::quote(loadError) + "}";
    ext->token = randomToken();

    if (existing) {
        for (auto& slot : g_extensions) {
            if (slot.get() == existing)
                slot = std::move(ext);
        }
    } else
        g_extensions.push_back(std::move(ext));
    saveRegistry();
    applyAll();
    host::installed(details.extensionId, existing != nullptr, previousVersion);
    return listJSON();
}

std::wstring pickPath(HWND owner, bool folder)
{
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return result;
    DWORD options = 0;
    dialog->GetOptions(&options);
    if (folder) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"Choose an unpacked extension folder (the one with manifest.json)");
    } else {
        COMDLG_FILTERSPEC filters[] = { { L"Extensions (*.crx, *.xpi, *.zip)", L"*.crx;*.xpi;*.zip" }, { L"All files", L"*.*" } };
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        dialog->SetTitle(L"Choose an extension file");
    }
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Public

void initialize(std::function<void(const std::string&)> notify)
{
    g_notify = std::move(notify);
    server::start(); // extension addresses include its port
    g_runtimeSource = loadRuntimeSource();
    std::error_code ignored;
    fs::create_directories(extensionsRoot(), ignored);
    fs::remove_all(stagingRoot(), ignored); // leftovers from an interrupted install
    loadRegistry();
    applyAll();
}

void handleScriptMessage(WKScriptMessageRef message, WKCompletionListenerRef listener)
{
    WKTypeRef body = WKScriptMessageGetBody(message);
    if (!body || WKGetTypeID(body) != WKStringGetTypeID()) {
        reply(listener, "{\"ok\":false,\"error\":\"Invalid message\"}");
        return;
    }
    std::string text = util::toUTF8(wk::toWide(static_cast<WKStringRef>(body)));
    json::Value parsed;
    if (text.size() > 64 * 1024 * 1024 || !json::parse(text, parsed) || !parsed.isObject()) {
        reply(listener, "{\"ok\":false,\"error\":\"Invalid message\"}");
        return;
    }

    // Only messages with an extension's secret token are answered. Pages
    // can see the message handler, but never a token.
    const std::string& token = parsed["t"].asString();
    Extension* ext = nullptr;
    for (auto& candidate : g_extensions) {
        if (candidate->enabled && token.size() == 32 && candidate->token == token)
            ext = candidate.get();
    }
    if (!ext) {
        reply(listener, "{\"ok\":false,\"error\":\"Unknown extension\"}");
        return;
    }

    std::string op = parsed["op"].asString();
    if (op.rfind("storage.", 0) != 0) {
        // Messaging, ports and waiting for messages (ExtensionHost.cpp).
        host::handleContentMessage(*ext, message, text, parsed, listener);
        return;
    }
    std::string error;
    std::string result = storageOperation(*ext, op, text, parsed, error);
    if (error.empty() && op != "storage.get" && op != "storage.bytes") {
        json::Value changed;
        if (json::parse(result, changed))
            host::storageChanged(*ext, parsed["area"].asString(), rawOf(result, changed["changes"]), static_cast<int>(parsed["ctx"].asNumber(0)));
    }
    if (!error.empty())
        reply(listener, "{\"ok\":false,\"error\":" + json::quote(error) + "}");
    else
        reply(listener, "{\"ok\":true,\"result\":" + (result.empty() ? std::string("null") : result) + "}");
}

std::string handleCommand(const std::wstring& command, const std::wstring& argument, HWND owner)
{
    // Arguments are "a\nb\nc".
    std::vector<std::wstring> args;
    size_t start = 0;
    for (;;) {
        size_t newline = argument.find(L'\n', start);
        args.push_back(argument.substr(start, newline == std::wstring::npos ? std::wstring::npos : newline - start));
        if (newline == std::wstring::npos)
            break;
        start = newline + 1;
    }
    auto arg = [&](size_t i) { return i < args.size() ? args[i] : std::wstring(); };
    auto argUTF8 = [&](size_t i) { return util::toUTF8(arg(i)); };

    if (command == L"list")
        return listJSON();

    if (command == L"install") {
        if (arg(0).empty())
            return "{\"error\":\"Paste a link or choose a file first.\"}";
        int job = g_nextJob++;
        startInstall(job, arg(0));
        return "{\"job\":" + std::to_string(job) + "}";
    }

    if (command == L"pickFile" || command == L"pickFolder")
        return "{\"path\":" + json::quote(util::toUTF8(pickPath(owner, command == L"pickFolder"))) + "}";

    if (command == L"confirm")
        return confirmInstall(_wtoi(arg(0).c_str()));

    if (command == L"cancel") {
        auto found = g_jobs.find(_wtoi(arg(0).c_str()));
        if (found != g_jobs.end()) {
            std::error_code ignored;
            fs::remove_all(found->second.staging, ignored);
            g_jobs.erase(found);
        }
        return "true";
    }

    if (command == L"setEnabled") {
        if (Extension* ext = findExtension(argUTF8(0))) {
            ext->enabled = arg(1) == L"1";
            saveRegistry();
            applyAll();
        }
        // Starting or stopping a background page can leave the Extensions
        // tab partly unpainted (black areas until it scrolls): repaint it a
        // moment later, once the background window exists.
        static HWND s_repaint = nullptr;
        s_repaint = owner;
        SetTimer(nullptr, 0, 250, [](HWND, UINT, UINT_PTR id, DWORD) {
            KillTimer(nullptr, id);
            if (s_repaint && IsWindow(s_repaint))
                RedrawWindow(s_repaint, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
        });
        return listJSON();
    }

    if (command == L"setRuleset") {
        if (Extension* ext = findExtension(argUTF8(0))) {
            ext->rulesetChoices[argUTF8(1)] = arg(2) == L"1";
            saveRegistry();
            rebuildBlocking();
        }
        return listJSON();
    }

    if (command == L"remove") {
        std::string id = argUTF8(0);
        Extension* ext = findExtension(id);
        if (!ext)
            return listJSON();
        int answer = MessageBoxW(owner, (L"Remove " + util::fromUTF8(ext->name) + L"?\n\nIts settings and saved data are deleted too.").c_str(),
            L"Remove extension", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer != IDOK)
            return listJSON();
        host::stop(id);
        forgetPendingStorage(*ext);
        g_extensions.erase(std::remove_if(g_extensions.begin(), g_extensions.end(), [&](auto& e) { return e->id == id; }), g_extensions.end());
        g_storage.erase(id);
        saveRegistry();
        applyAll();
        std::error_code ignored;
        fs::remove_all(extensionsRoot() / util::fromUTF8(id), ignored);
        fs::remove_all(extensionData(id), ignored);
        host::removed(id);
        return listJSON();
    }

    if (command == L"backgroundLog") {
        Extension* ext = findExtension(argUTF8(0));
        if (!ext)
            return "null";
        if (arg(1) == L"clear")
            ext->consoleLog.clear();
        std::string json = "{\"status\":" + host::backgroundStatusJSON(ext->id) + ",\"lines\":[";
        bool first = true;
        for (auto& line : ext->consoleLog) {
            json += std::string(first ? "" : ",") + json::quote(line);
            first = false;
        }
        return json + "]}";
    }

    if (command == L"clearErrors") {
        if (Extension* ext = findExtension(argUTF8(0)))
            ext->errors.clear();
        return listJSON();
    }

    if (command == L"inspect") {
        return inspectBackground(argUTF8(0)) ? "true" : "false";
    }

    if (command == L"openOptions") {
        int windowId = -1;
        if (BrowserWindow* window = BrowserWindow::fromHWND(owner))
            windowId = window->id();
        openOptionsPage(argUTF8(0), windowId);
        return "true";
    }

    if (command == L"queueInstall") {
        g_pendingInstall = arg(0);
        return "true";
    }

    return "null";
}

// ---------------------------------------------------------------------------
// For ExtensionHost.cpp (ExtensionsInternal.h)

bool Extension::hasPermission(const std::string& permission) const
{
    return std::find(permissions.begin(), permissions.end(), permission) != permissions.end();
}

namespace internal {

std::vector<Extension*> enabledExtensions()
{
    std::vector<Extension*> out;
    for (auto& ext : g_extensions) {
        if (ext->enabled)
            out.push_back(ext.get());
    }
    return out;
}

Extension* find(const std::string& id)
{
    return findExtension(id);
}

std::wstring extensionDataDirectory(const std::string& id)
{
    return extensionData(id).wstring();
}

Extension* findEnabled(const std::string& id)
{
    Extension* ext = findExtension(id);
    return ext && ext->enabled ? ext : nullptr;
}

std::string fileURL(const Extension& ext, const std::string& path)
{
    std::string url = util::toUTF8(util::fileURLFromPath(ext.dir.wstring()));
    if (url.empty() || url.back() != '/')
        url += '/';
    std::string clean = path;
    while (!clean.empty() && clean[0] == '/')
        clean.erase(0, 1);
    return url + clean;
}

std::string baseURL(const Extension& ext)
{
    uint16_t port = server::start();
    if (!port)
        return fileURL(ext, ""); // no server: the v0.5.0 behaviour
    return "http://" + server::hostLabel(ext.id) + ".localhost:" + std::to_string(port) + "/";
}

// "%20" -> " ", and lower case: Windows paths are case-insensitive, and
// WebKit and Windows escape file URLs differently.
static std::string normalizeFileURL(const std::string& url)
{
    std::string out;
    for (size_t i = 0; i < url.size(); ++i) {
        if (url[i] == '%' && i + 2 < url.size() && isxdigit(static_cast<unsigned char>(url[i + 1])) && isxdigit(static_cast<unsigned char>(url[i + 2]))) {
            out += static_cast<char>(strtol(url.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else
            out += url[i] == '\\' ? '/' : url[i];
    }
    return lowerASCII(out);
}

Extension* extensionForURL(const std::string& url, std::string* path)
{
    std::string id, rest;
    for (const char* scheme : { "chrome-extension://", "moz-extension://" }) {
        if (url.rfind(scheme, 0) == 0) {
            std::string tail = url.substr(strlen(scheme));
            size_t slash = tail.find('/');
            id = tail.substr(0, slash);
            rest = slash == std::string::npos ? "" : tail.substr(slash + 1);
            for (auto& ext : g_extensions) {
                // Firefox extensions know themselves by their gecko ID's UUID;
                // accept the Prism ID either way.
                if (ext->id == id) {
                    if (path)
                        *path = rest;
                    return ext.get();
                }
            }
            return nullptr;
        }
    }
    if (url.rfind("http://", 0) == 0 && server::port()) {
        std::string rest = lowerASCII(url.substr(7));
        size_t slash = rest.find('/');
        std::string host = rest.substr(0, slash);
        std::string suffix = ".localhost:" + std::to_string(server::port());
        if (host.size() <= suffix.size() || host.compare(host.size() - suffix.size(), std::string::npos, suffix) != 0)
            return nullptr;
        std::string label = host.substr(0, host.size() - suffix.size());
        for (auto& ext : g_extensions) {
            if (server::hostLabel(ext->id) == label) {
                if (path) {
                    std::string tail = slash == std::string::npos ? "" : url.substr(7 + slash + 1);
                    tail = tail.substr(0, tail.find_first_of("?#"));
                    *path = normalizeFileURL(tail); // percent-decoded
                }
                return ext.get();
            }
        }
        return nullptr;
    }
    if (url.rfind("file:", 0) != 0)
        return nullptr;
    std::string normalized = normalizeFileURL(url);
    std::string root = normalizeFileURL(util::toUTF8(util::fileURLFromPath(extensionsRoot().wstring())));
    if (!root.empty() && root.back() != '/')
        root += '/';
    if (normalized.rfind(root, 0) != 0)
        return nullptr;
    std::string tail = normalized.substr(root.size());
    size_t slash = tail.find('/');
    id = tail.substr(0, slash);
    for (auto& ext : g_extensions) {
        if (lowerASCII(ext->id) == id) {
            if (path) {
                // Lower case, but Windows file names aren't case-sensitive.
                std::string rest = slash == std::string::npos ? "" : tail.substr(slash + 1);
                *path = rest.substr(0, rest.find_first_of("?#"));
            }
            return ext.get();
        }
    }
    return nullptr;
}

fs::path resolve(const Extension& ext, const std::string& relative)
{
    std::string clean = relative.substr(0, relative.find_first_of("?#"));
    while (!clean.empty() && clean[0] == '/')
        clean.erase(0, 1);
    return resolveFile(ext.dir, clean);
}

std::string storage(Extension& ext, const std::string& op, const std::string& text, const json::Value& message, std::string& error)
{
    return storageOperation(ext, op, text, message, error);
}

void setRulesetEnabled(Extension& ext, const std::string& rulesetId, bool enabled)
{
    ext.rulesetChoices[rulesetId] = enabled;
    saveRegistry();
    rebuildBlocking();
    notifyList();
}

void grant(Extension& ext, const std::vector<std::string>& items)
{
    for (auto& item : items) {
        addUnique(ext.granted, item);
        if (std::find(ext.optionalHosts.begin(), ext.optionalHosts.end(), item) != ext.optionalHosts.end())
            addUnique(ext.hosts, item);
        else
            addUnique(ext.permissions, item);
    }
    saveRegistry();
    notifyList();
}

void revoke(Extension& ext, const std::vector<std::string>& items)
{
    for (auto& item : items) {
        ext.granted.erase(std::remove(ext.granted.begin(), ext.granted.end(), item), ext.granted.end());
        ext.permissions.erase(std::remove(ext.permissions.begin(), ext.permissions.end(), item), ext.permissions.end());
        ext.hosts.erase(std::remove(ext.hosts.begin(), ext.hosts.end(), item), ext.hosts.end());
    }
    saveRegistry();
    notifyList();
}

std::string injectionScript(const Extension& ext, const std::string& code)
{
    if (g_runtimeSource.empty())
        return "undefined";
    std::string config = "{\"id\":" + json::quote(ext.id);
    config += ",\"name\":" + json::quote(ext.name);
    config += ",\"token\":" + json::quote(ext.token);
    config += ",\"uiLanguage\":" + json::quote(ext.uiLanguage);
    config += ",\"base\":" + json::quote(baseURL(ext));
    config += ",\"manifest\":" + ext.manifestJSON;
    config += ",\"messages\":" + ext.messagesJSON;
    config += ",\"injected\":true";
    if (code.find("IdleCallback") != std::string::npos)
        config += ",\"idleCallback\":true";
    config += ",\"entries\":[{\"matches\":[\"<all_urls>\"],\"excludeMatches\":[],\"includeGlobs\":[],\"excludeGlobs\":[],\"allFrames\":false,\"runAt\":\"document_start\",\"css\":\"\"}]}";
    return g_runtimeSource + "(" + config + ", [function (chrome, browser) {\n" + code + "\n}])";
}

void logError(Extension& ext, const std::string& where, const std::string& message, const std::string& source)
{
    // The same error again only counts up.
    for (auto& entry : ext.errors) {
        if (entry.message == message && entry.source == source && entry.where == where) {
            ++entry.count;
            entry.time = static_cast<long long>(time(nullptr));
            return;
        }
    }
    ext.errors.push_back({ where, message, source, static_cast<long long>(time(nullptr)), 1 });
    if (ext.errors.size() > 30)
        ext.errors.erase(ext.errors.begin());
    // An extension stuck in an error loop shouldn't keep redrawing the page.
    static ULONGLONG lastNotify = 0;
    if (GetTickCount64() - lastNotify > 1000) {
        lastNotify = GetTickCount64();
        notifyList();
    }
}

void contentScriptsChanged()
{
    rebuildUserScripts();
}

} // namespace internal

bool isStoreURL(const std::wstring& url)
{
    return !chromeStoreId(url).empty() || !firefoxSlug(url).empty();
}

void queueInstall(const std::wstring& url)
{
    g_pendingInstall = url;
}

void flushStorage()
{
    saveDirtyStores();
    while (writeOnePending()) { }
    // Wait for a write the background thread may be in the middle of.
    std::lock_guard<std::mutex> writing(g_writeMutex);
}

} // namespace extensions
