// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "RequestFilter.h"

#include "Blocker.h"
#include "Shields.h"
#include "Util.h"

#include <algorithm>
#include <map>
#include <unordered_map>

using namespace wk;

namespace requestfilter {

namespace {

WKContextRef g_context = nullptr;
bool g_active = false;
unsigned long g_generation = 1;
std::vector<std::shared_ptr<ExtensionRules>> g_extensionRules;

struct PageStats {
    uint32_t blocked = 0;
    std::map<std::string, uint32_t> hosts;
};
std::unordered_map<uint64_t, PageStats> g_stats;   // bundle page id -> counts
std::unordered_map<WKPageRef, uint64_t> g_pageIds;  // page -> bundle page id

std::string toUTF8(WKStringRef string)
{
    return string ? util::toUTF8(toWide(string)) : std::string();
}

std::string bodyText(WKTypeRef body)
{
    if (!body || WKGetTypeID(body) != WKStringGetTypeID())
        return { };
    return toUTF8(static_cast<WKStringRef>(body));
}

std::string hostOf(const std::string& url)
{
    size_t scheme = url.find("://");
    if (scheme == std::string::npos)
        return { };
    size_t start = scheme + 3;
    size_t end = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    size_t at = authority.rfind('@');
    if (at != std::string::npos)
        authority = authority.substr(at + 1);
    if (!authority.empty() && authority[0] == '[')
        return authority.substr(1, authority.find(']') - 1);
    std::string host = authority.substr(0, authority.find(':'));
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return host;
}

std::string configText()
{
    // Nothing to ask when nothing filters: pages skip the round trip.
    bool filtering = shields::filtersRequests() || !g_extensionRules.empty();
    return std::string(filtering ? "1" : "0") + "\t" + std::to_string(g_generation);
}

void sendConfig()
{
    if (!g_context)
        return;
    auto text = stringUTF8(configText());
    if (WKContextSetInitializationUserDataForInjectedBundle)
        WKContextSetInitializationUserDataForInjectedBundle(g_context, text.get());
    if (WKContextPostMessageToInjectedBundle)
        WKContextPostMessageToInjectedBundle(g_context, stringUTF8("prism:filter").get(), text.get());
}

void record(uint64_t pageId, const std::string& host)
{
    blocker::recordBlocked(host);
    if (!pageId)
        return;
    if (g_stats.size() > 512 && !g_stats.count(pageId)) {
        // Pages that went away without telling us: forget the oldest kind.
        for (auto it = g_stats.begin(); it != g_stats.end();) {
            bool known = std::any_of(g_pageIds.begin(), g_pageIds.end(), [&](auto& entry) { return entry.second == it->first; });
            it = known ? std::next(it) : g_stats.erase(it);
        }
    }
    PageStats& stats = g_stats[pageId];
    ++stats.blocked;
    if (stats.hosts.size() < 200 || stats.hosts.count(host))
        ++stats.hosts[host];
}

// "<page id>\t<kind>\t<page URL>\t<request URL>" -> "1" (block) or "0".
std::string answer(const std::string& text)
{
    size_t a = text.find('\t');
    size_t b = a == std::string::npos ? a : text.find('\t', a + 1);
    size_t c = b == std::string::npos ? b : text.find('\t', b + 1);
    if (c == std::string::npos)
        return "0";
    uint64_t pageId = std::strtoull(text.c_str(), nullptr, 10);
    std::string kind = text.substr(a + 1, b - a - 1);
    std::string pageURL = text.substr(b + 1, c - b - 1);
    std::string url = text.substr(c + 1);
    std::string owner;
    if (!decide(kind, pageURL, url, &owner))
        return "0";
    record(pageId, hostOf(url));
    return "1";
}

void didReceiveSynchronousMessage(WKContextRef, WKStringRef name, WKTypeRef body, WKTypeRef* returnData, const void*)
{
    if (toUTF8(name) != "prism:check") {
        *returnData = nullptr;
        return;
    }
    g_active = true;
    *returnData = WKStringCreateWithUTF8CString(answer(bodyText(body)).c_str());
}

WKTypeRef initializationUserData(WKContextRef, const void*)
{
    return WKStringCreateWithUTF8CString(configText().c_str());
}

void didReceivePageMessage(WKPageRef page, WKStringRef name, WKTypeRef body, const void*)
{
    if (toUTF8(name) != "prism:page")
        return;
    uint64_t id = std::strtoull(bodyText(body).c_str(), nullptr, 10);
    if (id)
        g_pageIds[page] = id;
}

const PageStats* statsFor(WKPageRef page)
{
    auto id = g_pageIds.find(page);
    if (id == g_pageIds.end())
        return nullptr;
    auto stats = g_stats.find(id->second);
    return stats == g_stats.end() ? nullptr : &stats->second;
}

} // namespace

ExtensionRules::~ExtensionRules()
{
    shields::freeEngine(engine);
}

std::wstring bundlePath()
{
    std::wstring path = util::exeDirectory() + L"\\PrismBundle.dll";
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ? std::wstring() : path;
}

void initialize(WKContextRef context)
{
    if (!WKContextSetInjectedBundleClient || !WKMessageListenerSendReply)
        return;
    g_context = context;
    static WKContextInjectedBundleClientV2 client = { };
    client.base.version = 1; // V1 fields: messages + initialization data
    client.didReceiveSynchronousMessageFromInjectedBundle = didReceiveSynchronousMessage;
    client.getInjectedBundleInitializationUserData = initializationUserData;
    WKContextSetInjectedBundleClient(context, &client.base);
    sendConfig();
}

bool bundleActive()
{
    return g_active;
}

void attachPage(WKPageRef page)
{
    if (!WKPageSetPageInjectedBundleClient || !page)
        return;
    static WKPageInjectedBundleClientV0 client = { };
    client.base.version = 0;
    client.didReceiveMessageFromInjectedBundle = didReceivePageMessage;
    WKPageSetPageInjectedBundleClient(page, &client.base);
}

void detachPage(WKPageRef page)
{
    auto id = g_pageIds.find(page);
    if (id == g_pageIds.end())
        return;
    g_stats.erase(id->second);
    g_pageIds.erase(id);
}

void pageCommitted(WKPageRef page)
{
    auto id = g_pageIds.find(page);
    if (id != g_pageIds.end())
        g_stats.erase(id->second);
}

uint32_t blockedOnPage(WKPageRef page)
{
    const PageStats* stats = statsFor(page);
    return stats ? stats->blocked : 0;
}

std::vector<std::pair<std::string, uint32_t>> blockedHostsOnPage(WKPageRef page, size_t max)
{
    std::vector<std::pair<std::string, uint32_t>> out;
    if (const PageStats* stats = statsFor(page))
        out.assign(stats->hosts.begin(), stats->hosts.end());
    std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (out.size() > max)
        out.resize(max);
    return out;
}

void setExtensionRules(std::vector<std::shared_ptr<ExtensionRules>> rules)
{
    g_extensionRules = std::move(rules);
    rulesChanged();
}

void rulesChanged()
{
    ++g_generation;
    sendConfig();
}

bool decide(const std::string& kind, const std::string& pageURL, const std::string& url, std::string* owner)
{
    std::string host = hostOf(url);
    if (host.empty() || blocker::isUserAllowed(host))
        return false;

    // Without a telling extension, check the types most untyped requests are.
    static const std::vector<std::string> unknownKinds = { "other", "xmlhttprequest", "script" };
    std::vector<std::string> kinds = kind.empty() ? unknownKinds : std::vector<std::string> { kind };

    for (auto& type : kinds) {
        if (shields::blocksRequest(url, pageURL, type)) {
            if (owner)
                *owner = "Prism Shields";
            return true;
        }
    }
    for (auto& rules : g_extensionRules) {
        for (auto& type : kinds) {
            if (shields::engineBlocks(rules->engine, url, pageURL, type)) {
                if (owner)
                    *owner = rules->owner;
                return true;
            }
        }
    }
    return false;
}

} // namespace requestfilter
