// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "RequestFilter.h"
#include "Perf.h"

#include "Blocker.h"
#include "Engine.h"
#include "Extensions.h"
#include "Settings.h"
#include "Shields.h"
#include "Tabs.h"
#include "Util.h"

#include <algorithm>
#include <map>
#include <unordered_map>

using namespace wk;

namespace requestfilter {

namespace {

// The main browsing context and, once a private window opened, the private one.
std::vector<WKContextRef> g_contexts;
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
    bool filtering = shields::filtersRequests() || !g_extensionRules.empty() || extensions::webRequestListening();
    return std::string(filtering ? "1" : "0") + "\t" + std::to_string(g_generation);
}

void sendConfig()
{
    if (g_contexts.empty())
        return;
    auto text = stringUTF8(configText());
    for (WKContextRef context : g_contexts) {
        if (WKContextSetInitializationUserDataForInjectedBundle)
            WKContextSetInitializationUserDataForInjectedBundle(context, text.get());
        if (WKContextPostMessageToInjectedBundle)
            WKContextPostMessageToInjectedBundle(context, stringUTF8("prism:filter").get(), text.get());
    }
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

// webRequest's name for a request type.
const char* webRequestType(const std::string& kind)
{
    if (kind == "subdocument") return "sub_frame";
    if (kind == "stylesheet" || kind == "script" || kind == "image" || kind == "font" || kind == "media" || kind == "xmlhttprequest")
        return kind.c_str();
    return "other";
}

WKPageRef pageForId(uint64_t pageId)
{
    if (!pageId)
        return nullptr;
    for (auto& [page, id] : g_pageIds) {
        if (id == pageId)
            return page;
    }
    return nullptr;
}

std::vector<std::string> splitTabs(const std::string& text)
{
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t tab = text.find('\t', start);
        parts.push_back(text.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos)
            return parts;
        start = tab + 1;
    }
}

// The bundle's question:
// "<page id>\t<kind>\t<page URL>\t<request URL>\t<resource id>\t<frame id>\t<parent frame id>\t<method>\t<frame URL>"
// The answer: "<0|1|r>\t<flags: n = don't cache>\t<request id to report the end of>\t<redirect URL>".
void answer(const std::string& text, bool privatePage, std::function<void(const std::string&)> reply)
{
    std::vector<std::string> parts = splitTabs(text);
    if (parts.size() < 4) {
        reply("0");
        return;
    }
    uint64_t pageId = std::strtoull(parts[0].c_str(), nullptr, 10);
    const std::string& kind = parts[1];
    const std::string& pageURL = parts[2];
    const std::string& url = parts[3];
    std::string host = hostOf(url);

    std::string owner;
    shields::RequestVerdict verdict = decideRequest(kind, pageURL, url, &owner);
    if (verdict.action == shields::RequestVerdict::Block) {
        record(pageId, host);
        reply("1");
        return;
    }
    if (verdict.action == shields::RequestVerdict::StandIn) {
        // Blocked, with a harmless stand-in loaded instead (so the page
        // doesn't break waiting for the script it expected).
        record(pageId, host);
        reply("r\tn\t\t" + verdict.url);
        return;
    }
    if (verdict.action == shields::RequestVerdict::Rewrite) {
        reply("r\tn\t\t" + verdict.url); // tracking parameters removed
        return;
    }
    // Extensions don't see private windows' requests (unless allowed).
    if ((privatePage && !settings::extensionsInPrivate()) || !extensions::webRequestListening()) {
        reply("0");
        return;
    }

    extensions::WebRequestDetails details;
    if (WKPageRef page = pageForId(pageId)) {
        tabs::Info tab;
        if (tabs::findByPage(page, tab))
            details.tabId = tab.id;
    }
    details.url = url;
    details.type = webRequestType(kind);
    if (parts.size() >= 9) {
        details.frameId = std::atoi(parts[5].c_str());
        details.parentFrameId = std::atoi(parts[6].c_str());
        details.method = parts[7];
        details.documentURL = parts[8];
    }
    // The id extensions saw; filled in by webRequestBefore, read when the
    // answer comes (which can be after this function returns).
    auto requestId = std::make_shared<std::string>();
    extensions::webRequestBefore(details, *requestId, [reply, pageId, host, requestId](extensions::WebRequestDecision decision) {
        // Extensions that saw the request decide each time, so the page
        // doesn't remember the answer.
        std::string tail = std::string(requestId->empty() ? "\t" : "\tn") + "\t" + *requestId + "\t";
        if (decision.cancel) {
            record(pageId, host);
            reply("1" + tail);
        } else if (!decision.redirectURL.empty()) {
            reply("r" + tail + decision.redirectURL);
        } else {
            reply("0" + tail);
        }
    });
}

void didReceiveSynchronousMessageWithListener(WKContextRef context, WKStringRef name, WKTypeRef body, WKMessageListenerRef listener, const void*)
{
    auto held = Ref<WKMessageListenerRef>::retain(listener);
    auto reply = [held](const std::string& text) {
        auto value = Ref<WKStringRef>::adopt(WKStringCreateWithUTF8CString(text.c_str()));
        WKMessageListenerSendReply(held.get(), value.get());
    };
    std::string message = toUTF8(name);
    bool privateContext = engine::isPrivateContext(context);
    if (message == "prism:contentScripts?") {
        // A new web process: the extension content scripts it should run
        // (none in private windows, unless extensions are allowed there).
        reply(privateContext && !settings::extensionsInPrivate() ? std::string("0\n") : extensions::contentScriptsPayload());
        return;
    }
    if (message != "prism:check") {
        reply("");
        return;
    }
    g_active = true;
    perf::Scope busy("checking a page's request");
    answer(bodyText(body), privateContext, reply);
}

// "prism:done": "<request id>\t<1 = finished | 0 = failed>\t<error>".
void didReceiveMessage(WKContextRef, WKStringRef name, WKTypeRef body, const void*)
{
    if (toUTF8(name) != "prism:done")
        return;
    std::vector<std::string> parts = splitTabs(bodyText(body));
    if (parts.size() < 2)
        return;
    extensions::webRequestEnded(parts[0], parts[1] == "1", parts.size() > 2 ? parts[2] : std::string());
}

WKTypeRef initializationUserData(WKContextRef, const void*)
{
    return WKStringCreateWithUTF8CString(configText().c_str());
}

void didReceivePageMessage(WKPageRef page, WKStringRef name, WKTypeRef body, const void*)
{
    std::string message = toUTF8(name);
    // Extension content scripts in their own world (PrismBundle).
    if (message == "prism:content") {
        extensions::handleBundleContentMessage(page, bodyText(body));
        return;
    }
    if (message == "prism:execResult") {
        extensions::executeFinished(bodyText(body));
        return;
    }
    if (message != "prism:page")
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
    if (std::find(g_contexts.begin(), g_contexts.end(), context) != g_contexts.end())
        return;
    g_contexts.push_back(context);
    static WKContextInjectedBundleClientV2 client = { };
    // Version 2: questions are answered through a listener, so Prism can
    // wait for extensions (webRequest) without blocking its own window.
    // WebKit only uses the listener when the plain synchronous callback is
    // left empty.
    client.base.version = 2;
    client.didReceiveMessageFromInjectedBundle = didReceiveMessage;
    client.didReceiveSynchronousMessageFromInjectedBundleWithListener = didReceiveSynchronousMessageWithListener;
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
    return decideRequest(kind, pageURL, url, owner).action == shields::RequestVerdict::Block;
}

shields::RequestVerdict decideRequest(const std::string& kind, const std::string& pageURL, const std::string& url, std::string* owner)
{
    shields::RequestVerdict blocked;
    blocked.action = shields::RequestVerdict::Block;
    std::string host = hostOf(url);
    if (host.empty() || blocker::isUserAllowed(host))
        return { };

    // Without a telling extension, check the types most untyped requests are.
    static const std::vector<std::string> unknownKinds = { "other", "xmlhttprequest", "script" };
    std::vector<std::string> kinds = kind.empty() ? unknownKinds : std::vector<std::string> { kind };

    shields::RequestVerdict rewrite;
    for (auto& type : kinds) {
        shields::RequestVerdict verdict = shields::checkRequest(url, pageURL, type);
        if (verdict.action == shields::RequestVerdict::Block || verdict.action == shields::RequestVerdict::StandIn) {
            if (owner)
                *owner = "Prism Shields";
            return verdict;
        }
        if (verdict.action == shields::RequestVerdict::Rewrite && rewrite.action == shields::RequestVerdict::Allow)
            rewrite = verdict;
    }
    for (auto& rules : g_extensionRules) {
        for (auto& type : kinds) {
            if (shields::engineBlocks(rules->engine, url, pageURL, type)) {
                if (owner)
                    *owner = rules->owner;
                return blocked;
            }
        }
    }
    return rewrite;
}

} // namespace requestfilter
