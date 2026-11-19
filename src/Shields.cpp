// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Shields.h"

#include "Blocker.h"
#include "Engine.h"
#include "Http.h"
#include "Json.h"
#include "Util.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace shields {

using namespace wk;

namespace {

// ---------------------------------------------------------------------------
// prism_shields.dll (Brave's adblock-rust with a C API; see shields/src/lib.rs)

struct Library {
    void* (*builderNew)(void) = nullptr;
    size_t (*builderAddList)(void*, const char*, size_t, int) = nullptr;
    void* (*builderBuild)(void*) = nullptr;
    size_t (*engineUseResources)(void*, const char*, size_t) = nullptr;
    unsigned char* (*engineSerialize)(const void*, size_t*) = nullptr;
    void (*freeBuffer)(unsigned char*, size_t) = nullptr;
    void* (*engineDeserialize)(const unsigned char*, size_t) = nullptr;
    void (*engineFree)(void*) = nullptr;
    char* (*urlCosmeticResources)(const void*, const char*) = nullptr;
    char* (*hiddenClassIdSelectors)(const void*, const char*, const char*, const char*) = nullptr;
    int (*checkRequest)(const void*, const char*, const char*, const char*) = nullptr;
    void (*freeString)(char*) = nullptr;
    bool loaded = false;
};

Library g_lib;

template<typename T>
bool loadSymbol(HMODULE module, const char* name, T& target)
{
    target = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    return target != nullptr;
}

bool loadLibrary()
{
    std::wstring path = util::exeDirectory() + L"\\prism_shields.dll";
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module)
        return false;
    bool ok = loadSymbol(module, "prism_shields_builder_new", g_lib.builderNew)
        && loadSymbol(module, "prism_shields_builder_add_list", g_lib.builderAddList)
        && loadSymbol(module, "prism_shields_builder_build", g_lib.builderBuild)
        && loadSymbol(module, "prism_shields_engine_use_resources", g_lib.engineUseResources)
        && loadSymbol(module, "prism_shields_engine_serialize", g_lib.engineSerialize)
        && loadSymbol(module, "prism_shields_free_buffer", g_lib.freeBuffer)
        && loadSymbol(module, "prism_shields_engine_deserialize", g_lib.engineDeserialize)
        && loadSymbol(module, "prism_shields_engine_free", g_lib.engineFree)
        && loadSymbol(module, "prism_shields_url_cosmetic_resources", g_lib.urlCosmeticResources)
        && loadSymbol(module, "prism_shields_hidden_class_id_selectors", g_lib.hiddenClassIdSelectors)
        && loadSymbol(module, "prism_shields_check_request", g_lib.checkRequest)
        && loadSymbol(module, "prism_shields_free_string", g_lib.freeString);
    g_lib.loaded = ok;
    return ok;
}

// A string from the library, freed automatically.
std::string takeString(char* text)
{
    if (!text)
        return { };
    std::string out = text;
    g_lib.freeString(text);
    return out;
}

// ---------------------------------------------------------------------------
// Filter lists

struct ListInfo {
    const char* id;
    const char* name;
    const char* group;   // ads, privacy, malware, annoyances
    bool defaultOn;
    bool trusted;        // uBlock Origin's own lists may use trusted scriptlets
    std::vector<const char*> urls; // mirrors, tried in order
};

#define UBO_CDN(path) "https://ublockorigin.github.io/uAssetsCDN/" path, "https://ublockorigin.pages.dev/" path, \
    "https://cdn.jsdelivr.net/gh/uBlockOrigin/uAssetsCDN@main/" path, "https://raw.githubusercontent.com/uBlockOrigin/uAssetsCDN/main/" path

const std::vector<ListInfo>& catalog()
{
    // Same sources and defaults as uBlock Origin (see its assets.json).
    static const std::vector<ListInfo> lists = {
        { "ublock-filters", "uBlock filters – Ads", "ads", true, true, { UBO_CDN("filters/filters.min.txt") } },
        { "easylist", "EasyList", "ads", true, false, { UBO_CDN("thirdparties/easylist.txt") } },
        { "ublock-privacy", "uBlock filters – Privacy", "privacy", true, true, { UBO_CDN("filters/privacy.min.txt") } },
        { "easyprivacy", "EasyPrivacy", "privacy", true, false, { UBO_CDN("thirdparties/easyprivacy.txt") } },
        { "plowe", "Peter Lowe’s Ad and tracking server list", "privacy", true, false,
            { "https://pgl.yoyo.org/adservers/serverlist.php?hostformat=hosts&showintro=1&mimetype=plaintext" } },
        { "ublock-badware", "uBlock filters – Badware risks", "malware", true, true, { UBO_CDN("filters/badware.min.txt") } },
        { "ublock-unbreak", "uBlock filters – Unbreak", "fixes", true, true, { UBO_CDN("filters/unbreak.min.txt") } },
        { "ublock-quick-fixes", "uBlock filters – Quick fixes", "fixes", true, true, { UBO_CDN("filters/quick-fixes.min.txt") } },
        { "ublock-cookies", "uBlock filters – Cookie notices", "annoyances", false, true, { UBO_CDN("filters/annoyances-cookies.txt") } },
        { "easylist-cookies", "EasyList – Cookie notices", "annoyances", false, false, { UBO_CDN("thirdparties/easylist-cookies.txt") } },
        { "ublock-annoyances", "uBlock filters – Annoyances", "annoyances", false, true, { UBO_CDN("filters/annoyances.min.txt") } },
        { "easylist-annoyances", "EasyList – Other annoyances", "annoyances", false, false, { UBO_CDN("thirdparties/easylist-annoyances.txt") } },
        { "easylist-social", "EasyList – Social widgets", "annoyances", false, false, { UBO_CDN("thirdparties/easylist-social.txt") } },
    };
    return lists;
}

const ListInfo* findList(const std::string& id)
{
    for (auto& list : catalog()) {
        if (id == list.id)
            return &list;
    }
    return nullptr;
}

constexpr int64_t kUpdateEverySeconds = 4 * 24 * 60 * 60;

fs::path shieldsDir() { return fs::path(util::dataDirectory()) / L"Shields"; }
fs::path listFile(const std::string& id) { return shieldsDir() / L"lists" / util::fromUTF8(id + ".txt"); }
fs::path customFile() { return shieldsDir() / L"custom-filters.txt"; }

std::vector<std::string> splitList(const std::wstring& text)
{
    std::vector<std::string> out;
    std::string utf8 = util::toUTF8(text);
    size_t start = 0;
    while (start <= utf8.size()) {
        size_t comma = utf8.find(',', start);
        std::string item = utf8.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!item.empty())
            out.push_back(item);
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return out;
}

std::wstring joinList(const std::vector<std::string>& items)
{
    std::string out;
    for (auto& item : items)
        out += (out.empty() ? "" : ",") + item;
    return util::fromUTF8(out);
}

// ---------------------------------------------------------------------------
// Settings (prism.ini)

bool shieldsOn() { return util::readSetting(L"ShieldsEnabled", L"1") != L"0"; }

std::vector<std::string> enabledLists()
{
    std::wstring saved = util::readSetting(L"ShieldsLists", L"default");
    if (saved == L"default") {
        std::vector<std::string> out;
        for (auto& list : catalog()) {
            if (list.defaultOn)
                out.push_back(list.id);
        }
        return out;
    }
    std::vector<std::string> out;
    for (auto& id : splitList(saved)) {
        if (findList(id))
            out.push_back(id);
    }
    return out;
}

std::vector<std::string> disabledSites() { return splitList(util::readSetting(L"ShieldsDisabledSites")); }
std::vector<std::string> allowedDomains() { return splitList(util::readSetting(L"ShieldsAllowedDomains")); }

std::string lowerASCII(std::string text)
{
    for (auto& c : text)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
}

// "https://www.Example.com:8080/x" -> "www.example.com"
std::string hostOf(const std::string& url)
{
    size_t start = url.find("://");
    if (start == std::string::npos)
        return { };
    start += 3;
    size_t end = url.find_first_of("/?#", start);
    std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    size_t at = host.rfind('@');
    if (at != std::string::npos)
        host = host.substr(at + 1);
    if (!host.empty() && host[0] != '[')
        host = host.substr(0, host.find(':'));
    return lowerASCII(host);
}

// The "site" the Shields panel switches: the host without "www.".
std::string siteOf(const std::string& host)
{
    return host.rfind("www.", 0) == 0 ? host.substr(4) : host;
}

bool siteDisabled(const std::string& host)
{
    auto sites = disabledSites();
    for (auto& site : sites) {
        if (host == site || (host.size() > site.size() && host.compare(host.size() - site.size() - 1, std::string::npos, "." + site) == 0))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Domains for the network blocker

bool isPlainDomain(const std::string& text)
{
    if (text.empty() || text.size() > 253 || text.find('.') == std::string::npos)
        return false;
    for (char c : text) {
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '_'))
            return false;
    }
    return text.front() != '.' && text.back() != '.';
}

// Rules that block a whole domain everywhere: "||ads.example.com^" (with at
// most a few harmless options) and hosts-file lines ("0.0.0.0 ads.example.com").
void collectDomains(const std::string& text, std::vector<std::string>& blocked, std::vector<std::string>& allowed,
    std::vector<std::string>& thirdPartyOnly)
{
    static const std::set<std::string> okOptions = { "third-party", "3p", "all", "important", "document", "doc" };
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos)
            continue;
        line = line.substr(first);
        if (line[0] == '!' || line[0] == '[')
            continue;

        // Hosts file format.
        if (line.rfind("0.0.0.0 ", 0) == 0 || line.rfind("127.0.0.1 ", 0) == 0) {
            std::string domain = line.substr(line.find(' ') + 1);
            domain = domain.substr(0, domain.find_first_of(" \t#"));
            domain = lowerASCII(domain);
            if (isPlainDomain(domain) && domain != "localhost" && domain != "local")
                blocked.push_back(domain);
            continue;
        }
        if (line[0] == '#' || line.find('#') != std::string::npos)
            continue; // comments and cosmetic filters

        bool exception = line.rfind("@@", 0) == 0;
        if (exception)
            line = line.substr(2);
        if (line.rfind("||", 0) != 0)
            continue;
        std::string options;
        size_t dollar = line.find('$');
        if (dollar != std::string::npos) {
            options = line.substr(dollar + 1);
            line = line.substr(0, dollar);
        }
        std::string domain = line.substr(2);
        while (!domain.empty() && (domain.back() == '^' || domain.back() == '/'))
            domain.pop_back();
        domain = lowerASCII(domain);
        if (!isPlainDomain(domain))
            continue;
        bool usable = true;
        bool thirdParty = false;
        size_t start = 0;
        while (usable && start <= options.size() && !options.empty()) {
            size_t comma = options.find(',', start);
            std::string option = options.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            usable = okOptions.count(option) > 0;
            thirdParty = thirdParty || option == "third-party" || option == "3p";
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        if (!usable)
            continue;
        if (exception) {
            allowed.push_back(domain);
        } else {
            blocked.push_back(domain);
            if (thirdParty)
                thirdPartyOnly.push_back(domain);
        }
    }
}

// ---------------------------------------------------------------------------
// State (main thread unless noted)

std::function<void(const std::string&)> g_notify;
void* g_engine = nullptr;          // adblock-rust engine, used on the main thread
std::string g_resources;           // scriptlets JSON
std::string g_runtimeSource;       // ui/shields-runtime.js
std::string g_status = "starting"; // starting, downloading, building, ready, error, unavailable
std::string g_error;
size_t g_domainCount = 0;
bool g_working = false;
bool g_rerun = false;
bool g_rerunForce = false;
// The first build after starting uses the lists already on disk (updates
// come right after), so pages don't wait for downloads.
bool g_cacheOnlyNext = false;
std::vector<std::function<void()>> g_waiting;

void flushWaiting()
{
    auto tasks = std::move(g_waiting);
    g_waiting.clear();
    for (auto& task : tasks)
        task();
}

// Per-host scripts, most recently used last.
std::list<std::pair<std::string, std::string>> g_hostScripts;

std::string stateJSON();

void notifyChanged()
{
    if (g_notify)
        g_notify("{\"type\":\"shields\",\"state\":" + stateJSON() + "}");
}

void resetHostScripts()
{
    g_hostScripts.clear();
    engine::setUserScripts("shields", { });
}

// ---------------------------------------------------------------------------
// Building the engine (worker thread)

struct BuildResult {
    bool skippedDownloads { false };
    void* engine { nullptr };
    std::shared_ptr<blocker::Lists> domains;
    std::string error;
    std::vector<std::string> failedDownloads;
};

bool needsDownload(const std::string& id, bool force)
{
    std::error_code error;
    fs::path file = listFile(id);
    if (!fs::exists(file, error) || fs::file_size(file, error) == 0)
        return true;
    if (!force) {
        auto age = fs::file_time_type::clock::now() - fs::last_write_time(file, error);
        if (std::chrono::duration_cast<std::chrono::seconds>(age).count() < kUpdateEverySeconds)
            return false;
    }
    return true;
}

bool download(const ListInfo& list, std::string& error)
{
    for (const char* url : list.urls) {
        std::string body;
        std::wstring httpError;
        if (!http::get(util::fromUTF8(url), body, httpError, 64 * 1024 * 1024)) {
            error = util::toUTF8(httpError);
            continue;
        }
        // A mirror answering with an error page isn't a filter list.
        size_t first = body.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
        if (body.size() < 64 || first == std::string::npos || body[first] == '<') {
            error = "unexpected content";
            continue;
        }
        util::ensureDirectory((shieldsDir() / L"lists").wstring());
        if (util::writeFileAtomically(listFile(list.id).wstring(), body))
            return true;
        error = "couldn't save it";
    }
    return false;
}

BuildResult build(const std::vector<std::string>& ids, const std::string& custom, bool force,
    std::function<void(const std::string&)> progress, bool cacheOnly)
{
    BuildResult result;

    // 1. Fetch lists that are missing or due for an update (only missing
    //    ones for the quick first build after starting).
    for (auto& id : ids) {
        const ListInfo* list = findList(id);
        if (!list || !needsDownload(id, force))
            continue;
        if (cacheOnly && !util::readFile(listFile(id).wstring()).empty()) {
            result.skippedDownloads = true;
            continue;
        }
        progress(std::string("Downloading ") + list->name + "…");
        std::string error;
        if (!download(*list, error))
            result.failedDownloads.push_back(list->name);
    }

    // 2. Compile everything into one engine, and collect whole-domain rules
    //    for the network blocker.
    progress("Building the blocker…");
    void* builder = g_lib.builderNew();
    std::vector<std::string> blocked, allowed, thirdPartyOnly;
    size_t loaded = 0;
    for (auto& id : ids) {
        const ListInfo* list = findList(id);
        std::string text = util::readFile(listFile(id).wstring());
        if (!list || text.empty())
            continue;
        g_lib.builderAddList(builder, text.data(), text.size(), list->trusted ? 1 : 0);
        collectDomains(text, blocked, allowed, thirdPartyOnly);
        ++loaded;
    }
    if (!custom.empty()) {
        g_lib.builderAddList(builder, custom.data(), custom.size(), 0);
        collectDomains(custom, blocked, allowed, thirdPartyOnly);
    }
    result.engine = g_lib.builderBuild(builder);
    if (!result.engine) {
        result.error = "The blocker couldn't be built.";
        return result;
    }
    if (!g_resources.empty())
        g_lib.engineUseResources(result.engine, g_resources.data(), g_resources.size());
    if (!loaded && !ids.empty())
        result.error = "No filter lists could be downloaded. Check your internet connection.";

    auto domains = std::make_shared<blocker::Lists>();
    domains->owners.push_back("Prism Shields");
    for (auto& domain : blocked)
        domains->blocked.emplace(domain, 0);
    for (auto& domain : allowed)
        domains->allowed.insert(domain);
    // Third-party only if every rule for it says so; a domain some list
    // blocks outright stays blocked everywhere.
    {
        std::unordered_set<std::string> thirdPartySet(thirdPartyOnly.begin(), thirdPartyOnly.end());
        std::unordered_map<std::string, size_t> blockCount;
        for (auto& domain : blocked)
            ++blockCount[domain];
        std::unordered_map<std::string, size_t> thirdPartyCount;
        for (auto& domain : thirdPartyOnly)
            ++thirdPartyCount[domain];
        for (auto& domain : thirdPartySet) {
            if (thirdPartyCount[domain] == blockCount[domain])
                domains->thirdPartyOnly.insert(domain);
        }
    }
    result.domains = domains;
    return result;
}

void startBuild(bool force)
{
    if (!g_lib.loaded)
        return;
    if (g_working) {
        g_rerun = true;
        g_rerunForce = g_rerunForce || force;
        return;
    }
    g_working = true;
    g_status = "building";
    g_error.clear();
    notifyChanged();

    std::vector<std::string> ids = shieldsOn() ? enabledLists() : std::vector<std::string>();
    std::string custom = util::readFile(customFile().wstring());
    bool cacheOnly = g_cacheOnlyNext;
    g_cacheOnlyNext = false;
    std::thread([ids, custom, force, cacheOnly] {
        auto progress = [](const std::string& text) {
            util::postToMainThread([text] {
                g_status = text.rfind("Downloading", 0) == 0 ? "downloading" : "building";
                g_error = text;
                notifyChanged();
            });
        };
        BuildResult result = build(ids, custom, force, progress, cacheOnly);
        util::postToMainThread([result] {
            g_working = false;
            if (g_engine)
                g_lib.engineFree(g_engine);
            g_engine = result.engine;
            g_domainCount = result.domains ? result.domains->blocked.size() : 0;
            blocker::setLists("shields", shieldsOn() ? result.domains : nullptr);
            engine::updateBlockingProxy();
            resetHostScripts();
            if (!result.error.empty()) {
                g_status = "error";
                g_error = result.error;
            } else {
                g_status = "ready";
                g_error = result.failedDownloads.empty() ? "" : "Couldn't update: " + [&] {
                    std::string names;
                    for (auto& name : result.failedDownloads)
                        names += (names.empty() ? "" : ", ") + name;
                    return names;
                }() + ". Using the last downloaded copy.";
                util::writeSetting(L"ShieldsUpdated", std::to_wstring(static_cast<long long>(time(nullptr))));
            }
            notifyChanged();
            flushWaiting();
            // The quick build skipped due updates: fetch them now.
            if (result.skippedDownloads && !g_rerun)
                g_rerun = true;
            if (g_rerun) {
                bool force = g_rerunForce;
                g_rerun = g_rerunForce = false;
                startBuild(force);
            }
        });
    }).detach();
}

// ---------------------------------------------------------------------------
// Per-site scripts

std::string buildHostScript(const std::string& host, const std::string& url)
{
    std::string resourcesJSON = takeString(g_lib.urlCosmeticResources(g_engine, url.c_str()));
    json::Value resources;
    if (!json::parse(resourcesJSON, resources) || !resources.isObject())
        return { };

    std::string hide = "[";
    bool first = true;
    for (auto& selector : resources["hide_selectors"].items()) {
        hide += (first ? "" : ",") + json::quote(selector.asString());
        first = false;
    }
    hide += "]";
    std::string exceptions = "[";
    first = true;
    for (auto& exception : resources["exceptions"].items()) {
        exceptions += (first ? "" : ",") + json::quote(exception.asString());
        first = false;
    }
    exceptions += "]";
    std::string config = "{\"hide\":" + hide + ",\"exceptions\":" + exceptions
        + ",\"generichide\":" + (resources["generichide"].asBool() ? "true" : "false") + "}";

    std::string script = "/* Prism Shields: " + host + " */\n(function () {\n"
        "if (location.hostname !== " + json::quote(host) + ") return;\n";
    const std::string& scriptlets = resources["injected_script"].asString();
    if (!scriptlets.empty()) {
        // uBlock Origin's scriptlets expect a scriptletGlobals object.
        script += "try { (function () { const scriptletGlobals = {};\n" + scriptlets + "\n})(); } catch (e) { }\n";
    }
    script += "try { " + g_runtimeSource + "(" + config + "); } catch (e) { }\n})();\n";
    return script;
}

} // namespace

// ---------------------------------------------------------------------------
// Public

void initialize(std::function<void(const std::string&)> notify)
{
    g_notify = std::move(notify);
    util::ensureDirectory(shieldsDir().wstring());
    if (!loadLibrary()) {
        g_status = "unavailable";
        g_error = "prism_shields.dll is missing, so Prism Shields is off.";
        return;
    }
    g_resources = util::readFile(util::exeDirectory() + L"\\shields\\scriptlets.json");
    g_runtimeSource = util::readFile(util::exeDirectory() + L"\\ui\\shields-runtime.js");
    while (!g_runtimeSource.empty() && (g_runtimeSource.back() == '\n' || g_runtimeSource.back() == '\r' || g_runtimeSource.back() == ' '))
        g_runtimeSource.pop_back();

    std::unordered_set<std::string> allowed;
    for (auto& domain : allowedDomains())
        allowed.insert(domain);
    blocker::setUserAllowed(std::move(allowed));

    g_cacheOnlyNext = true;
    startBuild(false);

    // Check for list updates every few hours (each list updates every 4 days).
    std::thread([] {
        for (;;) {
            Sleep(6 * 60 * 60 * 1000);
            util::postToMainThread([] { startBuild(false); });
        }
    }).detach();
}

bool isStarting()
{
    return g_lib.loaded && shieldsOn() && !g_engine && g_status != "error" && g_status != "unavailable";
}

void whenReady(std::function<void()> task)
{
    if (!isStarting()) {
        task();
        return;
    }
    g_waiting.push_back(std::move(task));
    if (g_waiting.size() == 1) {
        // Don't hold pages back for long if building takes a while.
        std::thread([] {
            Sleep(5000);
            util::postToMainThread([] { flushWaiting(); });
        }).detach();
    }
}

bool isActiveFor(const std::wstring& url)
{
    if (!g_engine || !shieldsOn())
        return false;
    std::string text = util::toUTF8(url);
    if (text.rfind("http://", 0) != 0 && text.rfind("https://", 0) != 0)
        return false;
    return !siteDisabled(hostOf(text));
}

void prepareNavigation(const std::wstring& wideURL)
{
    if (!g_engine || g_runtimeSource.empty() || !shieldsOn())
        return;
    std::string url = util::toUTF8(wideURL);
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
        return;
    std::string host = hostOf(url);
    if (host.empty() || siteDisabled(host))
        return;

    for (auto it = g_hostScripts.begin(); it != g_hostScripts.end(); ++it) {
        if (it->first == host) {
            g_hostScripts.splice(g_hostScripts.end(), g_hostScripts, it); // most recent
            return;
        }
    }

    std::string script = buildHostScript(host, url);
    if (script.empty())
        return;
    g_hostScripts.emplace_back(host, script);

    // Every page and frame parses all of these scripts (each one only runs on
    // its own site), so keep the set small: when it grows past 32 sites, keep
    // the 16 most recently used.
    if (g_hostScripts.size() > 32) {
        while (g_hostScripts.size() > 16)
            g_hostScripts.pop_front();
        std::vector<std::string> sources;
        for (auto& entry : g_hostScripts)
            sources.push_back(entry.second);
        engine::setUserScripts("shields", std::move(sources));
    } else
        engine::addUserScript("shields", script);
}

void handleScriptMessage(WKScriptMessageRef message, WKCompletionListenerRef listener)
{
    auto reply = [&](const std::string& json) { WKCompletionListenerComplete(listener, stringUTF8(json).get()); };
    WKTypeRef body = WKScriptMessageGetBody(message);
    if (!g_engine || !body || WKGetTypeID(body) != WKStringGetTypeID())
        return reply("null");
    std::string text = util::toUTF8(toWide(static_cast<WKStringRef>(body)));
    json::Value parsed;
    if (text.size() > 8 * 1024 * 1024 || !json::parse(text, parsed) || !parsed.isObject())
        return reply("null");

    const std::string& op = parsed["op"].asString();
    auto raw = [&](const json::Value& value) {
        return value.isArray() ? text.substr(value.start(), value.end() - value.start()) : std::string("[]");
    };
    if (op == "generic")
        return reply(takeString(g_lib.hiddenClassIdSelectors(g_engine, raw(parsed["classes"]).c_str(), raw(parsed["ids"]).c_str(), raw(parsed["exceptions"]).c_str())));
    if (op == "check") {
        std::string page = parsed["page"].asString();
        std::string out = "[";
        bool first = true;
        for (auto& item : parsed["items"].items()) {
            int verdict = g_lib.checkRequest(g_engine, item[0].asString().c_str(), page.c_str(), item[1].asString().c_str());
            out += std::string(first ? "" : ",") + (verdict == 1 ? "true" : "false");
            first = false;
        }
        return reply(out + "]");
    }
    reply("null");
}

namespace {

std::string stateJSON()
{
    std::string json = "{\"available\":" + std::string(g_lib.loaded ? "true" : "false");
    json += ",\"enabled\":" + std::string(shieldsOn() ? "true" : "false");
    json += ",\"status\":" + json::quote(g_status);
    json += ",\"message\":" + json::quote(g_error);
    json += ",\"domains\":" + std::to_string(g_domainCount);
    json += ",\"blocked\":" + std::to_string(blocker::blockedCount());
    json += ",\"proxy\":" + std::string(engine::blockingProxyActive() ? "true" : "false");
    json += ",\"updated\":" + std::to_string(_wtoi64(util::readSetting(L"ShieldsUpdated", L"0").c_str()));
    json += ",\"lists\":[";
    auto on = enabledLists();
    bool first = true;
    for (auto& list : catalog()) {
        std::error_code error;
        auto size = fs::file_size(listFile(list.id), error);
        json += std::string(first ? "" : ",") + "{\"id\":" + json::quote(list.id) + ",\"name\":" + json::quote(list.name)
            + ",\"group\":" + json::quote(list.group)
            + ",\"on\":" + (std::find(on.begin(), on.end(), list.id) != on.end() ? "true" : "false")
            + ",\"size\":" + std::to_string(error ? 0 : static_cast<unsigned long long>(size)) + "}";
        first = false;
    }
    json += "],\"disabledSites\":[";
    first = true;
    for (auto& site : disabledSites()) {
        json += (first ? "" : ",") + json::quote(site);
        first = false;
    }
    json += "],\"allowedDomains\":[";
    first = true;
    for (auto& domain : allowedDomains()) {
        json += (first ? "" : ",") + json::quote(domain);
        first = false;
    }
    json += "],\"custom\":" + json::quote(util::readFile(customFile().wstring())) + "}";
    return json;
}

void applyAllowedDomains(std::vector<std::string> domains)
{
    util::writeSetting(L"ShieldsAllowedDomains", joinList(domains));
    std::unordered_set<std::string> set(domains.begin(), domains.end());
    blocker::setUserAllowed(std::move(set));
}

} // namespace

std::string handleCommand(const std::wstring& command, const std::wstring& argument)
{
    std::vector<std::string> args;
    {
        std::string utf8 = util::toUTF8(argument);
        size_t start = 0;
        for (;;) {
            size_t newline = utf8.find('\n', start);
            args.push_back(utf8.substr(start, newline == std::string::npos ? std::string::npos : newline - start));
            if (newline == std::string::npos)
                break;
            start = newline + 1;
        }
    }
    auto arg = [&](size_t i) { return i < args.size() ? args[i] : std::string(); };

    if (command == L"get")
        return stateJSON();

    if (command == L"setEnabled") {
        util::writeSetting(L"ShieldsEnabled", arg(0) == "1" ? L"1" : L"0");
        if (arg(0) != "1") {
            blocker::setLists("shields", nullptr);
            engine::updateBlockingProxy();
            resetHostScripts();
        }
        startBuild(false);
        return stateJSON();
    }

    if (command == L"setList") {
        auto on = enabledLists();
        std::string id = arg(0);
        if (findList(id)) {
            on.erase(std::remove(on.begin(), on.end(), id), on.end());
            if (arg(1) == "1")
                on.push_back(id);
            util::writeSetting(L"ShieldsLists", joinList(on));
            startBuild(false);
        }
        return stateJSON();
    }

    if (command == L"update") {
        startBuild(true);
        return stateJSON();
    }

    if (command == L"setCustom") {
        // Everything after the command is the filter text (it may contain newlines).
        util::ensureDirectory(shieldsDir().wstring());
        util::writeFileAtomically(customFile().wstring(), util::toUTF8(argument));
        startBuild(false);
        return stateJSON();
    }

    if (command == L"site") {
        std::string host = hostOf(arg(0));
        std::string json = "{\"host\":" + json::quote(host) + ",\"site\":" + json::quote(siteOf(host));
        json += ",\"web\":" + std::string(host.empty() ? "false" : "true");
        json += ",\"siteOn\":" + std::string(!host.empty() && !siteDisabled(host) ? "true" : "false");
        json += ",\"recent\":[";
        bool first = true;
        for (auto& [domain, count] : blocker::recentBlocked(12)) {
            json += std::string(first ? "" : ",") + "{\"domain\":" + json::quote(domain) + ",\"count\":" + std::to_string(count) + "}";
            first = false;
        }
        json += "],\"state\":" + stateJSON() + "}";
        return json;
    }

    if (command == L"setSite") {
        std::string site = siteOf(hostOf(arg(0)));
        if (!site.empty()) {
            auto sites = disabledSites();
            sites.erase(std::remove(sites.begin(), sites.end(), site), sites.end());
            if (arg(1) != "1")
                sites.push_back(site);
            util::writeSetting(L"ShieldsDisabledSites", joinList(sites));
            // Forget prepared scripts for that site.
            g_hostScripts.remove_if([&](auto& entry) { return siteDisabled(entry.first) || siteOf(entry.first) == site; });
            std::vector<std::string> sources;
            for (auto& entry : g_hostScripts)
                sources.push_back(entry.second);
            engine::setUserScripts("shields", std::move(sources));
        }
        notifyChanged();
        return stateJSON();
    }

    if (command == L"removeDisabledSite") {
        auto sites = disabledSites();
        sites.erase(std::remove(sites.begin(), sites.end(), arg(0)), sites.end());
        util::writeSetting(L"ShieldsDisabledSites", joinList(sites));
        return stateJSON();
    }

    if (command == L"allowDomain" || command == L"removeAllowedDomain") {
        auto domains = allowedDomains();
        std::string domain = lowerASCII(arg(0));
        domains.erase(std::remove(domains.begin(), domains.end(), domain), domains.end());
        if (command == L"allowDomain" && isPlainDomain(domain))
            domains.push_back(domain);
        applyAllowedDomains(domains);
        notifyChanged();
        return stateJSON();
    }

    return "null";
}

} // namespace shields
