// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The parts of an extension that aren't content scripts: its background
// page (or service worker), its toolbar button and popup, and its options
// page, plus the messaging between all of them and the chrome.* APIs they
// use (ui/extension-page.js is the JavaScript side).
//
// Every part of an extension that can receive messages ("context": a
// content script in one frame, the background page, the popup, an options
// tab) keeps one request open to Prism ("poll"). Prism answers it when there
// is something to deliver (a message, a port message, an event), and the
// context immediately asks again. A context that stops asking (its page
// navigated away) is forgotten after a few seconds.

#include "Extensions.h"
#include "ExtensionsInternal.h"

#include "BrowserWindow.h"
#include "Engine.h"
#include "ExtensionServer.h"
#include "Http.h"
#include "Json.h"
#include "Perf.h"
#include "RequestFilter.h"
#include "Tabs.h"
#include "Util.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <thread>

namespace fs = std::filesystem;

namespace extensions {

using namespace wk;
using internal::baseURL;

namespace {

// ---------------------------------------------------------------------------
// Small helpers

std::string ok(const std::string& result = "null")
{
    return "{\"ok\":true,\"result\":" + (result.empty() ? std::string("null") : result) + "}";
}

std::string fail(const std::string& message)
{
    return "{\"ok\":false,\"error\":" + json::quote(message) + "}";
}

void complete(WKCompletionListenerRef listener, const std::string& json)
{
    WKCompletionListenerComplete(listener, stringUTF8(json).get());
}

std::string raw(const std::string& text, const json::Value& value)
{
    if (value.end() <= value.start())
        return "null";
    return text.substr(value.start(), value.end() - value.start());
}

std::string decodeBase64(const std::string& text)
{
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int value = 0, bits = -8;
    for (char c : text) {
        size_t index = alphabet.find(c);
        if (index == std::string::npos)
            continue; // padding, line breaks
        value = (value << 6) + static_cast<int>(index);
        bits += 6;
        if (bits >= 0) {
            out += static_cast<char>((value >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

double nowMilliseconds()
{
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return static_cast<double>(value.QuadPart / 10000ULL) - 11644473600000.0;
}

std::string numberJSON(double value)
{
    if (!std::isfinite(value))
        return "null";
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
}

// ---- Match patterns ("*://*.example.com/*", "<all_urls>") ----------------------

bool matchesPattern(const std::string& pattern, const std::string& url)
{
    if (pattern == "<all_urls>")
        return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0 || url.rfind("file://", 0) == 0;
    static std::map<std::string, std::regex> cache;
    auto found = cache.find(pattern);
    if (found == cache.end()) {
        std::smatch parts;
        static const std::regex shape(R"(^(\*|https?|wss?|ftp|file):\/\/(\*|\*\.[^/*]+|[^/*]*)(\/.*)?$)");
        std::regex compiled("$^");
        if (std::regex_match(pattern, parts, shape)) {
            auto escape = [](const std::string& text) {
                std::string out;
                for (char c : text) {
                    if (strchr(".+?^${}()|[]\\", c))
                        out += '\\';
                    out += c;
                }
                return out;
            };
            std::string scheme = parts[1] == "*" ? "(https?|wss?)" : escape(parts[1]);
            std::string host = parts[2];
            if (host == "*")
                host = "[^/]*";
            else if (host.rfind("*.", 0) == 0)
                host = "([^/]+\\.)?" + escape(host.substr(2));
            else
                host = escape(host);
            std::string path = escape(parts[3].matched ? std::string(parts[3]) : "/");
            std::string pathRegex;
            for (char c : path)
                pathRegex += c == '*' ? std::string(".*") : std::string(1, c);
            try {
                compiled = std::regex("^" + scheme + "://" + host + "(:\\d+)?" + pathRegex + "$", std::regex::icase);
            } catch (...) {
            }
        }
        found = cache.emplace(pattern, std::move(compiled)).first;
    }
    return std::regex_match(url, found->second);
}

// Can this extension see the address and title of a page?
std::set<std::pair<std::string, int>> g_activeTab; // (extension, tab) after a toolbar click

bool canSee(const Extension& ext, int tabId, const std::string& url)
{
    if (ext.hasPermission("tabs") || g_activeTab.count({ ext.id, tabId }))
        return true;
    if (internal::extensionForURL(url) == &ext)
        return true;
    for (auto& host : ext.hosts) {
        if (matchesPattern(host, url))
            return true;
    }
    return false;
}

std::string tabJSON(const Extension& ext, const tabs::Info& tab)
{
    std::string url = util::toUTF8(tab.url);
    // Extension pages are reported by the same address runtime.getURL()
    // gives, so extensions can find their own tabs (AdNauseam closes its
    // welcome page that way). Older file:// addresses are mapped too.
    std::string path;
    if (Extension* owner = internal::extensionForURL(url, &path))
        url = internal::baseURL(*owner) + path;
    bool visible = canSee(ext, tab.id, util::toUTF8(tab.url));
    std::string json = "{\"id\":" + std::to_string(tab.id);
    json += ",\"windowId\":" + std::to_string(tab.windowId);
    json += ",\"index\":" + std::to_string(tab.index);
    json += std::string(",\"active\":") + (tab.active ? "true" : "false");
    json += std::string(",\"highlighted\":") + (tab.active ? "true" : "false");
    json += std::string(",\"selected\":") + (tab.active ? "true" : "false");
    json += ",\"pinned\":false,\"incognito\":false,\"discarded\":false,\"autoDiscardable\":true,\"audible\":false,\"groupId\":-1";
    json += ",\"mutedInfo\":{\"muted\":false}";
    json += std::string(",\"status\":") + (tab.loading ? "\"loading\"" : "\"complete\"");
    if (visible) {
        json += ",\"url\":" + json::quote(url);
        json += ",\"title\":" + json::quote(util::toUTF8(tab.title));
    }
    json += "}";
    return json;
}

std::string windowJSON(const Extension& ext, const tabs::WindowInfo& window, bool populate)
{
    std::string json = "{\"id\":" + std::to_string(window.id);
    json += std::string(",\"focused\":") + (window.focused ? "true" : "false");
    json += ",\"top\":" + std::to_string(window.bounds.top) + ",\"left\":" + std::to_string(window.bounds.left);
    json += ",\"width\":" + std::to_string(window.bounds.right - window.bounds.left);
    json += ",\"height\":" + std::to_string(window.bounds.bottom - window.bounds.top);
    json += ",\"incognito\":false,\"alwaysOnTop\":false,\"type\":\"normal\"";
    json += std::string(",\"state\":") + (window.state == 1 ? "\"minimized\"" : window.state == 2 ? "\"maximized\"" : "\"normal\"");
    if (populate) {
        json += ",\"tabs\":[";
        for (size_t i = 0; i < window.tabs.size(); ++i)
            json += (i ? "," : "") + tabJSON(ext, window.tabs[i]);
        json += "]";
    }
    return json + "}";
}

// ---------------------------------------------------------------------------
// Contexts

enum class Kind { Content, Background, Popup, Page };

const char* kindName(Kind kind)
{
    switch (kind) {
    case Kind::Content: return "content";
    case Kind::Background: return "background";
    case Kind::Popup: return "popup";
    case Kind::Page: return "tab";
    }
    return "";
}

struct Context {
    int id { 0 };
    std::string extId;
    Kind kind { Kind::Content };
    WKPageRef page { };
    bool mainFrame { true };
    int frameId { 0 };
    std::string url;
    Ref<WKCompletionListenerRef> poll;
    ULONGLONG pollSince { 0 };
    ULONGLONG repollDeadline { 0 }; // must ask again by then, or it's gone
    std::vector<std::string> queue;
};

std::map<int, Context> g_contexts;
int g_nextContext = 1;
int g_nextFrameId = 1;

struct PendingMessage {
    Ref<WKCompletionListenerRef> listener;
    std::set<int> waiting;
};
std::map<int, PendingMessage> g_pending;
int g_nextMessage = 1;

struct Port {
    std::string extId;
    int opener { 0 };
    std::set<int> receivers;
};
std::map<int, Port> g_ports;
int g_nextPort = 1;

void deliver(Context& context, const std::string& eventJSON);
void killContext(int id);

void flush(Context& context)
{
    if (!context.poll || context.queue.empty())
        return;
    std::string batch = "[";
    for (size_t i = 0; i < context.queue.size(); ++i)
        batch += (i ? "," : "") + context.queue[i];
    batch += "]";
    context.queue.clear();
    Ref<WKCompletionListenerRef> listener = std::move(context.poll);
    context.poll = { };
    context.repollDeadline = GetTickCount64() + 8000;
    complete(listener.get(), ok(batch));
}

void deliver(Context& context, const std::string& eventJSON)
{
    // Don't let a context that isn't listening pile up messages forever.
    if (context.queue.size() > 2000)
        context.queue.erase(context.queue.begin(), context.queue.begin() + 1000);
    context.queue.push_back(eventJSON);
    flush(context);
}

void finishPending(int messageId, const std::string& result)
{
    auto found = g_pending.find(messageId);
    if (found == g_pending.end())
        return;
    Ref<WKCompletionListenerRef> listener = std::move(found->second.listener);
    g_pending.erase(found);
    complete(listener.get(), ok(result));
}

// ---- webRequest -------------------------------------------------------------------
// PrismBundle.dll asks Prism about each request a page makes
// (RequestFilter.cpp). When extensions listen to webRequest events, Prism
// sends them the request and, for blocking listeners, waits for their
// answer (cancel or redirect) — up to kWebRequestWait, so a stuck
// extension can't hang pages.

struct WebRequestListener {
    int context { 0 };
    int id { 0 };          // the page's own listener number
    std::string event;     // "onBeforeRequest", "onCompleted", "onErrorOccurred"
    std::vector<std::string> urls;
    std::set<std::string> types;
    int tabId { -1 };
    bool blocking { false };
};
std::vector<WebRequestListener> g_webRequestListeners;

struct WebRequestPending {
    std::function<void(WebRequestDecision)> done;
    ULONGLONG started { 0 };
    std::string url;
    std::map<int, std::string> extensionOf; // context -> extension name (for the performance log)
    std::set<std::pair<int, int>> waiting; // (context, listener) still to answer
    WebRequestDecision decision;
    ULONGLONG deadline { 0 };
};
std::map<std::string, WebRequestPending> g_webRequestPending;

// Requests to report the end of: request id -> details JSON (without the
// closing brace), its URL, type and tab.
struct WebRequestTracked {
    std::string details;
    std::string url;
    std::string type;
    int tabId { -1 };
    ULONGLONG since { 0 };
};
std::map<std::string, WebRequestTracked> g_webRequestTracked;
uint64_t g_nextWebRequest = 1;
UINT_PTR g_webRequestTimer = 0;
constexpr ULONGLONG kWebRequestWait = 1000;

void finishWebRequest(const std::string& requestId)
{
    auto found = g_webRequestPending.find(requestId);
    if (found == g_webRequestPending.end())
        return;
    auto done = std::move(found->second.done);
    WebRequestDecision decision = found->second.decision;
    ULONGLONG waited = GetTickCount64() - found->second.started;
    if (found->second.started && waited >= 250) {
        // Who held the page's request up: those still not answering, or all of them.
        std::set<std::string> names;
        for (auto& [contextId, listenerId] : found->second.waiting)
            names.insert(found->second.extensionOf[contextId]);
        if (names.empty())
            for (auto& [contextId, name] : found->second.extensionOf)
                names.insert(name);
        std::string who;
        for (auto& name : names)
            who += (who.empty() ? "" : ", ") + name;
        perf::noteWebRequest(who, waited, !found->second.waiting.empty(), found->second.url);
    }
    g_webRequestPending.erase(found);
    if (done)
        done(decision);
}

void CALLBACK webRequestTimeouts(HWND, UINT, UINT_PTR, DWORD)
{
    ULONGLONG now = GetTickCount64();
    std::vector<std::string> late;
    for (auto& [id, pending] : g_webRequestPending) {
        if (now >= pending.deadline)
            late.push_back(id);
    }
    for (auto& id : late)
        finishWebRequest(id);
    // Requests that never reported back (pages closed mid-load).
    for (auto it = g_webRequestTracked.begin(); it != g_webRequestTracked.end();)
        it = now - it->second.since > 5 * 60 * 1000 ? g_webRequestTracked.erase(it) : std::next(it);
    if (g_webRequestPending.empty() && g_webRequestTimer) {
        KillTimer(nullptr, g_webRequestTimer);
        g_webRequestTimer = 0;
    }
}

// ---- Right-click menu items (contextMenus / menus) -----------------------------------
// Per extension, in the order they were created. Kept while Prism runs;
// Manifest V3 extensions' items are also saved (ExtensionData\<id>\menus.json),
// since their background only adds them when they're installed.

struct MenuItem {
    std::string id;       // JSON (number or string)
    std::string parentId; // JSON, "" at the top
    std::string title;
    std::string type { "normal" };
    std::vector<std::string> contexts { "page" };
    std::vector<std::string> documentPatterns;
    std::vector<std::string> targetPatterns;
    bool enabled { true };
    bool visible { true };
    bool checked { false };
};
std::map<std::string, std::vector<MenuItem>> g_menus;
std::set<std::string> g_menusLoaded;

std::string idJSON(const json::Value& value)
{
    if (value.isString())
        return json::quote(value.asString());
    if (value.isNumber())
        return numberJSON(value.asNumber());
    return { };
}

fs::path menusFile(const Extension& ext)
{
    return fs::path(internal::extensionDataDirectory(ext.id)) / L"menus.json";
}

void applyMenuProperties(MenuItem& item, const json::Value& props)
{
    if (props["title"].isString())
        item.title = props["title"].asString();
    if (props["type"].isString())
        item.type = props["type"].asString();
    if (props["contexts"].isArray()) {
        item.contexts.clear();
        for (auto& context : props["contexts"].items())
            item.contexts.push_back(context.asString());
    } else if (props["contexts"].isString()) {
        item.contexts = { props["contexts"].asString() };
    }
    if (props["documentUrlPatterns"].isArray()) {
        item.documentPatterns.clear();
        for (auto& pattern : props["documentUrlPatterns"].items())
            item.documentPatterns.push_back(pattern.asString());
    }
    if (props["targetUrlPatterns"].isArray()) {
        item.targetPatterns.clear();
        for (auto& pattern : props["targetUrlPatterns"].items())
            item.targetPatterns.push_back(pattern.asString());
    }
    if (props["enabled"].isBool())
        item.enabled = props["enabled"].asBool();
    if (props["visible"].isBool())
        item.visible = props["visible"].asBool();
    if (props["checked"].isBool())
        item.checked = props["checked"].asBool();
    if (props.has("parentId"))
        item.parentId = idJSON(props["parentId"]);
}

std::string menuItemJSON(const MenuItem& item)
{
    auto list = [](const std::vector<std::string>& values) {
        std::string out = "[";
        for (size_t i = 0; i < values.size(); ++i)
            out += (i ? "," : "") + json::quote(values[i]);
        return out + "]";
    };
    std::string json = "{\"id\":" + item.id + ",\"title\":" + json::quote(item.title) + ",\"type\":" + json::quote(item.type)
        + ",\"contexts\":" + list(item.contexts) + ",\"documentUrlPatterns\":" + list(item.documentPatterns)
        + ",\"targetUrlPatterns\":" + list(item.targetPatterns)
        + ",\"enabled\":" + (item.enabled ? "true" : "false") + ",\"visible\":" + (item.visible ? "true" : "false")
        + ",\"checked\":" + (item.checked ? "true" : "false");
    if (!item.parentId.empty())
        json += ",\"parentId\":" + item.parentId;
    return json + "}";
}

std::vector<MenuItem>& menusFor(const Extension& ext)
{
    std::vector<MenuItem>& items = g_menus[ext.id];
    if (g_menusLoaded.insert(ext.id).second && ext.manifestVersion >= 3) {
        json::Value saved;
        if (json::parse(util::readFile(menusFile(ext).wstring()), saved)) {
            for (auto& entry : saved.items()) {
                MenuItem item;
                item.id = idJSON(entry["id"]);
                if (item.id.empty())
                    continue;
                applyMenuProperties(item, entry);
                items.push_back(std::move(item));
            }
        }
    }
    return items;
}

void saveMenus(const Extension& ext)
{
    if (ext.manifestVersion < 3)
        return; // their background adds them again each time it starts
    std::string json = "[";
    bool first = true;
    for (auto& item : g_menus[ext.id]) {
        json += (first ? "" : ",") + menuItemJSON(item);
        first = false;
    }
    json += "]";
    util::ensureDirectory(internal::extensionDataDirectory(ext.id));
    util::writeFileAtomically(menusFile(ext).wstring(), json);
}

void forgetWebRequestListeners(int contextId)
{
    bool changed = false;
    for (auto it = g_webRequestListeners.begin(); it != g_webRequestListeners.end();) {
        if (it->context == contextId) {
            it = g_webRequestListeners.erase(it);
            changed = true;
        } else
            ++it;
    }
    // Answers it still owed.
    std::vector<std::string> ready;
    for (auto& [id, pending] : g_webRequestPending) {
        for (auto w = pending.waiting.begin(); w != pending.waiting.end();)
            w = w->first == contextId ? pending.waiting.erase(w) : std::next(w);
        if (pending.waiting.empty())
            ready.push_back(id);
    }
    for (auto& id : ready)
        finishWebRequest(id);
    if (changed)
        requestfilter::rulesChanged();
}

void killContext(int id)
{
    forgetWebRequestListeners(id);
    auto found = g_contexts.find(id);
    if (found == g_contexts.end())
        return;
    if (found->second.poll)
        complete(found->second.poll.get(), ok("[]"));
    // A Manifest V2 background adds its menu items again when it restarts.
    if (found->second.kind == Kind::Background) {
        Extension* ext = internal::find(found->second.extId);
        if (ext && ext->manifestVersion < 3)
            g_menus.erase(ext->id);
    }
    g_contexts.erase(found);

    // Messages it was supposed to answer.
    std::vector<int> unanswered;
    for (auto& [messageId, pending] : g_pending) {
        if (pending.waiting.erase(id) && pending.waiting.empty())
            unanswered.push_back(messageId);
    }
    for (int messageId : unanswered)
        finishPending(messageId, "{\"none\":true}");

    // Its ports.
    std::vector<int> closed;
    for (auto& [portId, port] : g_ports) {
        if (port.opener == id) {
            for (int receiver : port.receivers) {
                auto other = g_contexts.find(receiver);
                if (other != g_contexts.end())
                    deliver(other->second, "{\"type\":\"portDisconnect\",\"port\":" + std::to_string(portId) + "}");
            }
            closed.push_back(portId);
        } else if (port.receivers.erase(id) && port.receivers.empty()) {
            auto opener = g_contexts.find(port.opener);
            if (opener != g_contexts.end())
                deliver(opener->second, "{\"type\":\"portDisconnect\",\"port\":" + std::to_string(portId) + "}");
            closed.push_back(portId);
        }
    }
    for (int portId : closed)
        g_ports.erase(portId);
}

void killContextsOfPage(WKPageRef page)
{
    std::vector<int> ids;
    for (auto& [id, context] : g_contexts) {
        if (context.page == page)
            ids.push_back(id);
    }
    for (int id : ids)
        killContext(id);
}

void killContextsOfExtension(const std::string& extId)
{
    std::vector<int> ids;
    for (auto& [id, context] : g_contexts) {
        if (context.extId == extId)
            ids.push_back(id);
    }
    for (int id : ids)
        killContext(id);
}

// Background page, popups and extension tabs.
bool isExtensionPage(const Context& context)
{
    return context.kind != Kind::Content;
}

void broadcastToPages(const std::string& extId, const std::string& eventJSON)
{
    for (auto& [id, context] : g_contexts) {
        if (context.extId == extId && isExtensionPage(context))
            deliver(context, eventJSON);
    }
}

std::string eventJSON(const std::string& name, const std::string& argsJSON)
{
    return "{\"type\":\"event\",\"name\":" + json::quote(name) + ",\"args\":" + argsJSON + "}";
}

std::string senderJSON(const Context& context)
{
    std::string json = "{\"id\":" + json::quote(context.extId);
    if (!context.url.empty()) {
        json += ",\"url\":" + json::quote(context.url);
        size_t scheme = context.url.find("://");
        size_t end = scheme == std::string::npos ? std::string::npos : context.url.find('/', scheme + 3);
        if (scheme != std::string::npos)
            json += ",\"origin\":" + json::quote(context.url.substr(0, end));
    }
    tabs::Info tab;
    Extension* ext = internal::findEnabled(context.extId);
    if (ext && tabs::findByPage(context.page, tab)) {
        json += ",\"tab\":" + tabJSON(*ext, tab);
        json += ",\"frameId\":" + std::to_string(context.frameId);
    }
    return json + "}";
}

// ---------------------------------------------------------------------------
// Host windows: background pages (hidden) and popups

struct HostView {
    std::string extId;
    Kind kind { Kind::Background };
    HWND hwnd { };
    Ref<WKViewRef> view;
    HWND viewHwnd { };
    WKPageRef page { };
    // Popups
    HWND owner { };
    POINT anchor { };
    double scale { 1 };
    int tabId { -1 };
    bool shown { false };
    bool inDialog { false };
    bool inspecting { false }; // "Inspect popup": stays open when it loses focus
    int crashes { 0 };
    ULONGLONG lastCrash { 0 };
    ULONGLONG loadedAt { 0 };      // background page finished loading
    bool reportedSilent { false }; // "didn't start" already logged
    // chrome.offscreen documents (hidden, like backgrounds)
    bool offscreen { false };
    Ref<WKCompletionListenerRef> created; // createDocument() waits for the load
    ULONGLONG closeAt { 0 };              // closeDocument() was called; kept a while for reuse
};

std::map<std::string, std::unique_ptr<HostView>> g_backgrounds;
std::map<std::string, std::unique_ptr<HostView>> g_offscreen;
std::unique_ptr<HostView> g_popup;
void closeOffscreen(const std::string& extId);
ULONGLONG g_popupClosedAt = 0;
std::string g_popupClosedExt;

constexpr UINT WM_HOST_CLOSE = WM_APP + 1;
constexpr UINT_PTR kShowPopupTimer = 1;

HostView* hostForPage(WKPageRef page)
{
    if (g_popup && g_popup->page == page)
        return g_popup.get();
    for (auto& [id, host] : g_backgrounds) {
        if (host->page == page)
            return host.get();
    }
    for (auto& [id, host] : g_offscreen) {
        if (host->page == page)
            return host.get();
    }
    return nullptr;
}

void detach(WKPageRef page)
{
    WKPageUIClientV6 ui = { };
    ui.base.version = 6;
    WKPageSetPageUIClient(page, &ui.base);
    WKPageNavigationClientV3 navigation = { };
    navigation.base.version = 3;
    WKPageSetPageNavigationClient(page, &navigation.base);
}

// Pages of popups that were just closed: anything they still send is ignored.
std::vector<std::pair<WKPageRef, ULONGLONG>> g_closedPopupPages;

bool isClosedPopupPage(WKPageRef page)
{
    if (g_popup && g_popup->page == page)
        return false; // a new popup that happens to reuse the address
    ULONGLONG now = GetTickCount64();
    for (auto& [closed, at] : g_closedPopupPages) {
        if (closed == page && now - at < 10000)
            return true;
    }
    return false;
}

void closePopup()
{
    if (!g_popup)
        return;
    std::unique_ptr<HostView> popup = std::move(g_popup);
    g_popupClosedAt = GetTickCount64();
    g_popupClosedExt = popup->extId;
    // Stop the page first: once its ports are disconnected, a page that's
    // still running (it closes a moment later) would report errors about
    // them, which Chrome's popups never get to do.
    g_closedPopupPages.push_back({ popup->page, GetTickCount64() });
    if (g_closedPopupPages.size() > 8)
        g_closedPopupPages.erase(g_closedPopupPages.begin());
    WKPageClose(popup->page);
    killContextsOfPage(popup->page);
    detach(popup->page);
    SetWindowLongPtrW(popup->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(popup->hwnd);
    // The view goes with `popup`, after its window.
    BrowserWindow::refreshToolbars(); // the button isn't "open" any more
}

void placePopup(HostView& popup, int cssWidth, int cssHeight)
{
    cssWidth = std::clamp(cssWidth, 25, 800);
    cssHeight = std::clamp(cssHeight, 25, 600);
    int width = static_cast<int>(cssWidth * popup.scale) + 2;
    int height = static_cast<int>(cssHeight * popup.scale) + 2;
    int x = popup.anchor.x - width;
    int y = popup.anchor.y;
    MONITORINFO monitor = { sizeof(monitor) };
    if (GetMonitorInfoW(MonitorFromPoint(popup.anchor, MONITOR_DEFAULTTONEAREST), &monitor)) {
        x = std::max<int>(monitor.rcWork.left, std::min<int>(x, monitor.rcWork.right - width));
        y = std::min<int>(y, monitor.rcWork.bottom - height);
    }
    SetWindowPos(popup.hwnd, HWND_TOP, x, y, width, height, SWP_NOACTIVATE);
    if (!popup.shown) {
        popup.shown = true;
        KillTimer(popup.hwnd, kShowPopupTimer);
        ShowWindow(popup.hwnd, SW_SHOW);
        SetForegroundWindow(popup.hwnd);
    }
}

LRESULT CALLBACK hostWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* host = reinterpret_cast<HostView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_ACTIVATE:
        if (host && host->kind == Kind::Popup) {
            if (LOWORD(wParam) == WA_INACTIVE) {
                if (!host->inDialog && !host->inspecting)
                    PostMessageW(hwnd, WM_HOST_CLOSE, 0, 0);
            } else if (host->viewHwnd)
                SetFocus(host->viewHwnd);
        }
        return 0;
    case WM_TIMER:
        if (host && wParam == kShowPopupTimer)
            placePopup(*host, 360, 420); // the page never said how big it is
        return 0;
    case WM_HOST_CLOSE:
        if (host && host == g_popup.get())
            closePopup();
        return 0;
    case WM_SIZE:
        if (host && host->viewHwnd) {
            int inset = host->kind == Kind::Popup ? 1 : 0;
            MoveWindow(host->viewHwnd, inset, inset, std::max(0, LOWORD(lParam) - 2 * inset), std::max(0, HIWORD(lParam) - 2 * inset), TRUE);
        }
        return 0;
    case WM_ERASEBKGND: {
        RECT rect;
        GetClientRect(hwnd, &rect);
        HBRUSH border = CreateSolidBrush(RGB(90, 90, 100));
        FillRect(reinterpret_cast<HDC>(wParam), &rect, border);
        DeleteObject(border);
        return 1;
    }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void registerHostClass()
{
    static bool registered = false;
    if (registered)
        return;
    WNDCLASSEXW windowClass = { sizeof(windowClass) };
    windowClass.style = CS_DROPSHADOW;
    windowClass.lpfnWndProc = hostWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"PrismExtensionHost";
    RegisterClassExW(&windowClass);
    registered = true;
}

void openInTab(const std::wstring& url, HWND nearWindow)
{
    int windowId = -1;
    if (BrowserWindow* window = BrowserWindow::fromHWND(nearWindow))
        windowId = window->id();
    std::wstring target = url;
    if (Extension* ext = internal::extensionForURL(util::toUTF8(url))) {
        std::string path;
        internal::extensionForURL(util::toUTF8(url), &path);
        target = util::fromUTF8(baseURL(*ext) + path);
    }
    tabs::create(windowId, target, true);
}

// ---- WebKit callbacks for host pages -----------------------------------------------

HostView* hostFromClientInfo(const void* clientInfo)
{
    return const_cast<HostView*>(static_cast<const HostView*>(clientInfo));
}

WKPageRef hostCreateNewPage(WKPageRef, WKPageConfigurationRef, WKNavigationActionRef action, WKWindowFeaturesRef, const void* clientInfo)
{
    // window.open() and target="_blank" links from extension pages open as tabs.
    HostView* host = hostFromClientInfo(clientInfo);
    auto request = Ref<WKURLRequestRef>::adopt(WKNavigationActionCopyRequest(action));
    auto requestURL = Ref<WKURLRef>::adopt(WKURLRequestCopyURL(request.get()));
    std::wstring url = toWide(requestURL.get());
    HWND nearWindow = host->kind == Kind::Popup ? host->owner : nullptr;
    util::postToMainThread([url, nearWindow] { openInTab(url, nearWindow); });
    if (host->kind == Kind::Popup)
        PostMessageW(host->hwnd, WM_HOST_CLOSE, 0, 0);
    return nullptr;
}

void hostClose(WKPageRef, const void* clientInfo)
{
    HostView* host = hostFromClientInfo(clientInfo);
    if (host->kind == Kind::Popup)
        PostMessageW(host->hwnd, WM_HOST_CLOSE, 0, 0);
    else if (host->offscreen) {
        std::string extId = host->extId;
        util::postToMainThread([extId] { closeOffscreen(extId); });
    }
}

void hostAlert(WKPageRef, WKStringRef text, WKFrameRef, WKSecurityOriginRef, WKPageRunJavaScriptAlertResultListenerRef listener, const void* clientInfo)
{
    HostView* host = hostFromClientInfo(clientInfo);
    if (host->kind == Kind::Popup) {
        host->inDialog = true;
        Extension* ext = internal::find(host->extId);
        MessageBoxW(host->hwnd, toWide(text).c_str(), ext ? util::fromUTF8(ext->name).c_str() : L"Extension", MB_OK);
        host->inDialog = false;
    }
    WKPageRunJavaScriptAlertResultListenerCall(listener);
}

void hostConfirm(WKPageRef, WKStringRef text, WKFrameRef, WKSecurityOriginRef, WKPageRunJavaScriptConfirmResultListenerRef listener, const void* clientInfo)
{
    HostView* host = hostFromClientInfo(clientInfo);
    bool answer = false;
    if (host->kind == Kind::Popup) {
        host->inDialog = true;
        Extension* ext = internal::find(host->extId);
        answer = MessageBoxW(host->hwnd, toWide(text).c_str(), ext ? util::fromUTF8(ext->name).c_str() : L"Extension", MB_OKCANCEL) == IDOK;
        host->inDialog = false;
    }
    WKPageRunJavaScriptConfirmResultListenerCall(listener, answer);
}

// The background page's importScripts() needs files synchronously, which
// only window.prompt() offers: "prism-ext:read\n<path>".
void hostPrompt(WKPageRef, WKStringRef message, WKStringRef, WKFrameRef frame, WKSecurityOriginRef, WKPageRunJavaScriptPromptResultListenerRef listener, const void* clientInfo)
{
    HostView* host = hostFromClientInfo(clientInfo);
    std::string text = util::toUTF8(toWide(message));
    std::string answer;
    bool answered = false;
    if (text.rfind("prism-ext:read\n", 0) == 0 && frame && WKFrameIsMainFrame(frame)) {
        Extension* ext = internal::findEnabled(host->extId);
        std::string path;
        if (ext && internal::extensionForURL(text.substr(15), &path) == ext) {
            fs::path file = internal::resolve(*ext, path);
            if (!file.empty()) {
                answer = util::readFile(file.wstring());
                answered = true;
            }
        }
    }
    WKPageRunJavaScriptPromptResultListenerCall(listener, answered ? stringUTF8(answer).get() : nullptr);
}

void hostDecidePolicy(WKPageRef, WKNavigationActionRef action, WKFramePolicyListenerRef listener, WKTypeRef, const void* clientInfo)
{
    HostView* host = hostFromClientInfo(clientInfo);
    auto request = Ref<WKURLRequestRef>::adopt(WKNavigationActionCopyRequest(action));
    auto requestURL = Ref<WKURLRef>::adopt(WKURLRequestCopyURL(request.get()));
    std::wstring url = toWide(requestURL.get());
    std::string utf8 = util::toUTF8(url);
    auto frame = Ref<WKFrameInfoRef>::adopt(WKNavigationActionCopyTargetFrameInfo(action));
    bool mainFrame = !frame || WKFrameInfoGetIsMainFrame(frame.get());
    // The extension's own pages, and anything in frames (sign-in frames and
    // the like). A main-frame link to a website opens in a tab instead.
    if (internal::extensionForURL(utf8) == internal::find(host->extId) || utf8.rfind("about:", 0) == 0
        || utf8.rfind("data:", 0) == 0 || utf8.rfind("blob:", 0) == 0 || !mainFrame) {
        WKFramePolicyListenerUse(listener);
        return;
    }
    WKFramePolicyListenerIgnore(listener);
    if (utf8.rfind("http://", 0) == 0 || utf8.rfind("https://", 0) == 0 || utf8.rfind("chrome-extension://", 0) == 0 || utf8.rfind("moz-extension://", 0) == 0) {
        HWND nearWindow = host->kind == Kind::Popup ? host->owner : nullptr;
        util::postToMainThread([url, nearWindow] { openInTab(url, nearWindow); });
        if (host->kind == Kind::Popup)
            PostMessageW(host->hwnd, WM_HOST_CLOSE, 0, 0);
    }
}

void startBackground(Extension& ext);

void ensureTick();

// A line in the extension's console log (Extensions page, "Background page").
void consoleNote(const std::string& extId, const std::string& level, const std::string& text)
{
    Extension* ext = internal::find(extId);
    if (!ext)
        return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    char stamp[16];
    snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d", now.wHour, now.wMinute, now.wSecond);
    ext->consoleLog.push_back(std::string(stamp) + "\t" + level + "\t" + text);
    while (ext->consoleLog.size() > 400)
        ext->consoleLog.pop_front();
}

// Per extension, this session: how often its background was restarted.
struct BackgroundHistory {
    int restarts { 0 };
    int selfReloads { 0 };
    std::vector<ULONGLONG> recentReloads;
    bool stopped { false }; // gave up after crashes
};
std::map<std::string, BackgroundHistory> g_backgroundHistory;

void logBackgroundProblem(const std::string& extId, const std::string& message)
{
    consoleNote(extId, "prism", message);
    if (Extension* ext = internal::findEnabled(extId))
        internal::logError(*ext, "background", message, "");
}

void closeOffscreen(const std::string& extId);

void hostProcessTerminated(WKPageRef, uint32_t reason, const void* clientInfo)
{
    HostView* host = hostFromClientInfo(clientInfo);
    std::string extId = host->extId;
    if (host->kind == Kind::Popup) {
        PostMessageW(host->hwnd, WM_HOST_CLOSE, 0, 0);
        return;
    }
    if (host->offscreen) {
        util::postToMainThread([extId] { closeOffscreen(extId); });
        return;
    }
    // Restart a crashed background page a few times (counting again once it
    // has run for 5 minutes).
    ULONGLONG now = GetTickCount64();
    if (host->lastCrash && now - host->lastCrash > 5 * 60 * 1000)
        host->crashes = 0;
    host->lastCrash = now;
    host->loadedAt = 0;
    int crashes = host->crashes + 1;
    const char* why = reason == 0 ? "used too much memory" : reason == 1 ? "used too much processor time" : reason == 2 ? "was stopped" : "crashed";
    logBackgroundProblem(extId, std::string("The background page ") + why
        + (crashes > 3 ? ". Prism stopped restarting it after 3 tries; disable and enable the extension to start it again." : ". Prism restarted it."));
    killContextsOfPage(host->page);
    g_backgroundHistory[extId].stopped = crashes > 3;
    util::postToMainThread([extId, crashes] {
        auto found = g_backgrounds.find(extId);
        Extension* ext = internal::findEnabled(extId);
        if (found == g_backgrounds.end() || !ext || crashes > 3)
            return;
        g_backgroundHistory[extId].restarts++;
        WKPageReload(found->second->page);
        found->second->crashes = crashes;
    });
}

void hostDidFailNavigation(WKPageRef, WKNavigationRef, WKErrorRef error, WKTypeRef, const void* clientInfo)
{
    HostView* host = hostFromClientInfo(clientInfo);
    if (host && host->offscreen && host->created) {
        Ref<WKCompletionListenerRef> listener = std::move(host->created);
        complete(listener.get(), fail("The offscreen document couldn't load."));
        return;
    }
    if (!host || host->kind != Kind::Background)
        return;
    std::string description;
    int code = error ? WKErrorGetErrorCode(error) : 0;
    if (error) {
        if (WKStringRef text = WKErrorCopyLocalizedDescription(error)) {
            description = util::toUTF8(toWide(text));
            WKRelease(text);
        }
    }
    logBackgroundProblem(host->extId, "The background page couldn't load (" + (description.empty() ? std::string("error") : description)
        + ", code " + std::to_string(code) + ").");
}

void hostDidFinishNavigation(WKPageRef, WKNavigationRef, WKTypeRef, const void* clientInfo)
{
    HostView* host = hostFromClientInfo(clientInfo);
    if (host && host->offscreen && host->created) {
        Ref<WKCompletionListenerRef> listener = std::move(host->created);
        complete(listener.get(), ok());
        return;
    }
    if (host && host->kind == Kind::Background && !host->offscreen) {
        consoleNote(host->extId, "prism", "Background page loaded.");
        host->loadedAt = GetTickCount64();
        host->reportedSilent = false;
        ensureTick(); // checks that it connects
    }
}

void attachHostClients(HostView& host)
{
    WKPageUIClientV6 ui = { };
    ui.base.version = 6;
    ui.base.clientInfo = &host;
    ui.createNewPage = hostCreateNewPage;
    ui.close = hostClose;
    ui.runJavaScriptAlert = hostAlert;
    ui.runJavaScriptConfirm = hostConfirm;
    ui.runJavaScriptPrompt = hostPrompt;
    WKPageSetPageUIClient(host.page, &ui.base);

    WKPageNavigationClientV3 navigation = { };
    navigation.base.version = 3;
    navigation.base.clientInfo = &host;
    navigation.decidePolicyForNavigationAction = hostDecidePolicy;
    navigation.webProcessDidTerminate = hostProcessTerminated;
    navigation.didFailProvisionalNavigation = hostDidFailNavigation;
    navigation.didFailNavigation = reinterpret_cast<void*>(hostDidFailNavigation);
    navigation.didFinishNavigation = reinterpret_cast<void*>(hostDidFinishNavigation);
    WKPageSetPageNavigationClient(host.page, &navigation.base);
}

std::unique_ptr<HostView> createHost(const std::string& extId, Kind kind, HWND owner)
{
    registerHostClass();
    auto host = std::make_unique<HostView>();
    host->extId = extId;
    host->kind = kind;
    host->owner = owner;
    DWORD style = WS_POPUP | WS_CLIPCHILDREN;
    host->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"PrismExtensionHost", L"", style,
        0, 0, kind == Kind::Background ? 1024 : 10, kind == Kind::Background ? 768 : 10, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!host->hwnd)
        return nullptr;
    SetWindowLongPtrW(host->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host.get()));
    RECT rect = { 0, 0, kind == Kind::Background ? 1024 : 10, kind == Kind::Background ? 768 : 10 };
    WKPageConfigurationRef configuration = engine::extensionPageConfiguration();
    host->view = Ref<WKViewRef>::adopt(WKViewCreate(rect, configuration, host->hwnd));
    WKViewSetIsInWindow(host->view.get(), true);
    host->viewHwnd = WKViewGetWindow(host->view.get());
    host->page = WKViewGetPage(host->view.get());
    engine::configurePage(host->page, kind == Kind::Popup ? host->scale : 1.0);
    attachHostClients(*host);
    return host;
}

// chrome.offscreen: one hidden extension page per extension. Prism's
// background is a page already, but MV3 extensions (Bitwarden keeps its
// localStorage there) expect a separate document they can message.
void closeOffscreen(const std::string& extId)
{
    auto found = g_offscreen.find(extId);
    if (found == g_offscreen.end())
        return;
    std::unique_ptr<HostView> host = std::move(found->second);
    g_offscreen.erase(found);
    if (host->created) {
        Ref<WKCompletionListenerRef> listener = std::move(host->created);
        complete(listener.get(), fail("The offscreen document was closed."));
    }
    killContextsOfPage(host->page);
    detach(host->page);
    SetWindowLongPtrW(host->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(host->hwnd);
}

void stopBackground(const std::string& extId)
{
    closeOffscreen(extId);
    auto found = g_backgrounds.find(extId);
    if (found == g_backgrounds.end())
        return;
    std::unique_ptr<HostView> host = std::move(found->second);
    g_backgrounds.erase(found);
    killContextsOfPage(host->page);
    detach(host->page);
    SetWindowLongPtrW(host->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(host->hwnd);
}

// MV2 "scripts" and MV3 service workers run in a page Prism writes into the
// extension's folder.
std::string backgroundPageFor(Extension& ext)
{
    if (!ext.backgroundPage.empty())
        return ext.backgroundPage;
    std::vector<std::string> scripts = ext.backgroundScripts;
    bool module = false;
    if (scripts.empty() && !ext.serviceWorker.empty()) {
        scripts.push_back(ext.serviceWorker);
        module = ext.serviceWorkerModule;
    }
    if (scripts.empty())
        return { };
    std::string html = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>" + ext.name + "</title></head><body>\n";
    for (auto& script : scripts) {
        std::string src = script;
        while (!src.empty() && src[0] == '/')
            src.erase(0, 1);
        std::string escaped;
        for (char c : src)
            escaped += c == '"' ? std::string("&quot;") : c == '<' ? std::string("&lt;") : std::string(1, c);
        html += std::string("<script") + (module ? " type=\"module\"" : "") + " src=\"" + escaped + "\"></script>\n";
    }
    html += "</body></html>\n";
    util::writeFileAtomically((ext.dir / L"_prism_background.html").wstring(), html);
    return "_prism_background.html";
}

void startBackground(Extension& ext)
{
    if (g_backgrounds.count(ext.id))
        return;
    std::string page = backgroundPageFor(ext);
    if (page.empty())
        return;
    auto host = createHost(ext.id, Kind::Background, nullptr);
    if (!host) {
        logBackgroundProblem(ext.id, "Prism couldn't create the background page's window.");
        return;
    }
    consoleNote(ext.id, "prism", "Starting the background page (" + page + (ext.serviceWorker.empty() ? "" : ", from service worker " + ext.serviceWorker) + ").");
    g_backgroundHistory[ext.id].stopped = false;
    WKPageLoadURL(host->page, url(util::fromUTF8(baseURL(ext) + page)).get());
    g_backgrounds[ext.id] = std::move(host);
}

std::string backgroundStatus(const std::string& extId)
{
    Extension* ext = internal::findEnabled(extId);
    auto found = g_backgrounds.find(extId);
    BackgroundHistory& history = g_backgroundHistory[extId];
    std::string state;
    if (!ext || (ext->backgroundPage.empty() && ext->backgroundScripts.empty() && ext->serviceWorker.empty()))
        state = "none";
    else if (found == g_backgrounds.end() || !found->second)
        state = "stopped";
    else if (std::any_of(g_contexts.begin(), g_contexts.end(), [&](const auto& entry) { return entry.second.extId == extId && entry.second.kind == Kind::Background; }))
        state = "running";
    else if (history.stopped)
        state = "stopped";
    else if (found->second->loadedAt)
        state = "silent";
    else
        state = "loading";
    return "{\"state\":\"" + state + "\",\"restarts\":" + std::to_string(history.restarts) + ",\"selfReloads\":" + std::to_string(history.selfReloads)
        + ",\"offscreen\":" + (g_offscreen.count(extId) && !g_offscreen[extId]->closeAt ? "true" : "false") + "}";
}

// ---------------------------------------------------------------------------
// Toolbar buttons (chrome.action / browserAction / pageAction)

struct ActionState {
    std::map<std::string, std::string> global;
    std::map<int, std::map<std::string, std::string>> perTab;
};
std::map<std::string, ActionState> g_actions;

std::string actionValue(const Extension& ext, const std::string& property, int tabId)
{
    auto& state = g_actions[ext.id];
    if (tabId >= 0) {
        auto tab = state.perTab.find(tabId);
        if (tab != state.perTab.end()) {
            auto value = tab->second.find(property);
            if (value != tab->second.end())
                return value->second;
        }
    }
    auto value = state.global.find(property);
    if (value != state.global.end())
        return value->second;
    if (property == "title")
        return ext.actionTitle;
    if (property == "popup")
        return ext.actionPopup;
    if (property == "icon") // Prism's toolbar is a file:// page: give it the file
        return ext.actionIcon.empty() ? ext.iconDataURL : internal::fileURL(ext, ext.actionIcon);
    if (property == "enabled")
        return "1";
    if (property == "badgeColor")
        return "#d93025";
    if (property == "badgeTextColor")
        return "#ffffff";
    return { };
}

void refreshToolbars()
{
    BrowserWindow::refreshToolbars();
}

// ---------------------------------------------------------------------------
// Alarms

struct Alarm {
    std::string name;
    double scheduledTime { 0 };
    double periodInMinutes { 0 };
};
std::map<std::string, std::map<std::string, Alarm>> g_alarms;

std::string alarmJSON(const Alarm& alarm)
{
    std::string json = "{\"name\":" + json::quote(alarm.name) + ",\"scheduledTime\":" + numberJSON(alarm.scheduledTime);
    if (alarm.periodInMinutes > 0)
        json += ",\"periodInMinutes\":" + numberJSON(alarm.periodInMinutes);
    return json + "}";
}

// ---------------------------------------------------------------------------
// Startup events wait until the background page is listening.

std::map<std::string, std::vector<std::string>> g_waitingForBackground;
bool g_browserStarted = false;

void sendToBackground(const std::string& extId, const std::string& event)
{
    for (auto& [id, context] : g_contexts) {
        if (context.extId == extId && context.kind == Kind::Background) {
            deliver(context, event);
            return;
        }
    }
    g_waitingForBackground[extId].push_back(event);
}

// ---------------------------------------------------------------------------
// The once-a-second tick: alarms, and noticing contexts that went away.

void CALLBACK tick(HWND, UINT, UINT_PTR, DWORD)
{
    ULONGLONG now = GetTickCount64();
    std::vector<int> dead;
    for (auto& [id, context] : g_contexts) {
        // Backgrounds and popups live as long as Prism's own view of them
        // (they're dropped when it goes), and a busy one (an ad blocker
        // compiling its lists, or handling every request of a busy page)
        // can take a while to ask again. Only other pages expire.
        bool ownView = context.kind == Kind::Background || context.kind == Kind::Popup || hostForPage(context.page); // offscreen documents too
        if (!ownView && !context.poll && context.repollDeadline && now > context.repollDeadline)
            dead.push_back(id);
        else if (context.poll && now - context.pollSince > 20000)
            deliver(context, "{\"type\":\"ping\"}"); // are you still there?
    }
    for (int id : dead)
        killContext(id);

    // Offscreen documents closed a while ago.
    std::vector<std::string> expired;
    for (auto& [extId, host] : g_offscreen) {
        if (host->closeAt && now >= host->closeAt)
            expired.push_back(extId);
    }
    for (auto& extId : expired)
        closeOffscreen(extId);

    // A background page that loaded but never connected (its Prism runtime
    // didn't run): say so on the Extensions page.
    for (auto& [extId, host] : g_backgrounds) {
        if (!host || !host->loadedAt || host->reportedSilent || now - host->loadedAt < 15000)
            continue;
        bool connected = std::any_of(g_contexts.begin(), g_contexts.end(), [&](const auto& entry) {
            return entry.second.extId == extId && entry.second.kind == Kind::Background;
        });
        if (!connected) {
            host->reportedSilent = true;
            logBackgroundProblem(extId, "The background page loaded but didn't connect to Prism.");
        }
    }

    double time = nowMilliseconds();
    for (auto& [extId, alarms] : g_alarms) {
        std::vector<std::string> fired;
        for (auto& [name, alarm] : alarms) {
            if (alarm.scheduledTime > time)
                continue;
            std::string json = alarmJSON(alarm);
            broadcastToPages(extId, eventJSON("alarms.onAlarm", "[" + json + "]"));
            if (alarm.periodInMinutes > 0) {
                while (alarm.scheduledTime <= time)
                    alarm.scheduledTime += alarm.periodInMinutes * 60000.0;
            } else
                fired.push_back(name);
        }
        for (auto& name : fired)
            alarms.erase(name);
    }
}

void ensureTick()
{
    static UINT_PTR timer = 0;
    if (!timer)
        timer = SetTimer(nullptr, 0, 1000, tick);
}

// ---------------------------------------------------------------------------
// Messaging

// Sends a message to `targets` and answers `listener` with the first
// response (or none).
void sendToTargets(const Context& from, const std::vector<int>& targets, const std::string& messageJSON, WKCompletionListenerRef listener)
{
    if (targets.empty()) {
        complete(listener, fail("Could not establish connection. Receiving end does not exist."));
        return;
    }
    int messageId = g_nextMessage++;
    PendingMessage& pending = g_pending[messageId];
    pending.listener = Ref<WKCompletionListenerRef>::retain(listener);
    std::string event = "{\"type\":\"message\",\"id\":" + std::to_string(messageId) + ",\"message\":" + messageJSON + ",\"sender\":" + senderJSON(from) + "}";
    for (int target : targets) {
        pending.waiting.insert(target);
        deliver(g_contexts[target], event);
    }
}

std::vector<int> extensionPageTargets(const std::string& extId, int except)
{
    std::vector<int> out;
    for (auto& [id, context] : g_contexts) {
        if (context.extId == extId && isExtensionPage(context) && id != except)
            out.push_back(id);
    }
    return out;
}

std::vector<int> contentTargets(const std::string& extId, int tabId, int frameId)
{
    tabs::Info tab;
    std::vector<int> out;
    if (!tabs::find(tabId, tab))
        return out;
    for (auto& [id, context] : g_contexts) {
        if (context.extId == extId && context.kind == Kind::Content && context.page == tab.page && (frameId < 0 || context.frameId == frameId))
            out.push_back(id);
    }
    return out;
}

// Operations every context can use: waiting for events, answering messages,
// sending messages and using ports. Returns false if `op` isn't one of them.
bool commonOperation(Context& context, const std::string& op, const std::string& text, const json::Value& message, WKCompletionListenerRef listener)
{
    if (op == "poll") {
        if (context.poll)
            complete(context.poll.get(), ok("[]"));
        context.poll = Ref<WKCompletionListenerRef>::retain(listener);
        context.pollSince = GetTickCount64();
        context.repollDeadline = 0;
        flush(context);
        return true;
    }

    if (op == "respond") {
        int messageId = static_cast<int>(message["id"].asNumber());
        auto found = g_pending.find(messageId);
        if (found != g_pending.end() && found->second.waiting.erase(context.id)) {
            if (message["handled"].asBool())
                finishPending(messageId, "{\"response\":" + raw(text, message["response"]) + "}");
            else if (found->second.waiting.empty())
                finishPending(messageId, "{\"none\":true}");
        }
        complete(listener, ok());
        return true;
    }

    if (op == "runtime.sendMessage") {
        sendToTargets(context, extensionPageTargets(context.extId, context.id), raw(text, message["message"]), listener);
        return true;
    }

    if (op == "tabs.sendMessage") {
        if (!isExtensionPage(context)) {
            complete(listener, fail("tabs.sendMessage is for extension pages."));
            return true;
        }
        int frameId = message["frameId"].isNumber() ? static_cast<int>(message["frameId"].asNumber()) : -1;
        sendToTargets(context, contentTargets(context.extId, static_cast<int>(message["tabId"].asNumber(-1)), frameId), raw(text, message["message"]), listener);
        return true;
    }

    if (op == "port.connect") {
        std::vector<int> targets;
        if (message["tabId"].isNumber() && isExtensionPage(context)) {
            int frameId = message["frameId"].isNumber() ? static_cast<int>(message["frameId"].asNumber()) : -1;
            targets = contentTargets(context.extId, static_cast<int>(message["tabId"].asNumber()), frameId);
        } else
            targets = extensionPageTargets(context.extId, context.id);
        int portId = g_nextPort++;
        if (targets.empty()) {
            complete(listener, ok("{\"port\":" + std::to_string(portId) + ",\"disconnected\":true}"));
            return true;
        }
        Port& port = g_ports[portId];
        port.extId = context.extId;
        port.opener = context.id;
        std::string event = "{\"type\":\"connect\",\"port\":" + std::to_string(portId) + ",\"name\":" + json::quote(message["name"].asString())
            + ",\"sender\":" + senderJSON(context) + "}";
        for (int target : targets) {
            port.receivers.insert(target);
            deliver(g_contexts[target], event);
        }
        complete(listener, ok("{\"port\":" + std::to_string(portId) + "}"));
        return true;
    }

    if (op == "port.post" || op == "port.disconnect") {
        int portId = static_cast<int>(message["port"].asNumber());
        auto found = g_ports.find(portId);
        if (found == g_ports.end()) {
            complete(listener, op == "port.post" ? fail("Attempting to use a disconnected port object") : ok());
            return true;
        }
        Port& port = found->second;
        std::vector<int> recipients;
        if (port.opener == context.id)
            recipients.assign(port.receivers.begin(), port.receivers.end());
        else if (port.receivers.count(context.id))
            recipients.push_back(port.opener);
        else {
            complete(listener, fail("Not your port"));
            return true;
        }
        if (op == "port.post") {
            std::string event = "{\"type\":\"portMessage\",\"port\":" + std::to_string(portId) + ",\"message\":" + raw(text, message["message"]) + "}";
            for (int recipient : recipients) {
                auto other = g_contexts.find(recipient);
                if (other != g_contexts.end())
                    deliver(other->second, event);
            }
        } else {
            std::string event = "{\"type\":\"portDisconnect\",\"port\":" + std::to_string(portId) + "}";
            if (port.opener == context.id) {
                for (int recipient : recipients) {
                    auto other = g_contexts.find(recipient);
                    if (other != g_contexts.end())
                        deliver(other->second, event);
                }
                g_ports.erase(found);
            } else {
                port.receivers.erase(context.id);
                if (port.receivers.empty()) {
                    auto opener = g_contexts.find(port.opener);
                    if (opener != g_contexts.end())
                        deliver(opener->second, event);
                    g_ports.erase(found);
                }
            }
        }
        complete(listener, ok());
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Running code in tabs (tabs.executeScript, scripting.executeScript, insertCSS)

struct ScriptCall {
    Ref<WKCompletionListenerRef> listener;
    bool wrapInArray { false }; // tabs.executeScript returns [result]
    int frameId { 0 };
    bool scripting { false };   // scripting.executeScript returns [{frameId, result}]
};

void scriptFinished(WKTypeRef result, WKErrorRef, void* context)
{
    std::unique_ptr<ScriptCall> call(static_cast<ScriptCall*>(context));
    std::string text;
    if (result && WKGetTypeID(result) == WKStringGetTypeID())
        text = util::toUTF8(toWide(static_cast<WKStringRef>(result)));
    json::Value parsed;
    std::string value = "null";
    if (!text.empty() && json::parse(text, parsed) && parsed.isObject()) {
        if (!parsed["ok"].asBool()) {
            complete(call->listener.get(), fail(parsed["error"].asString()));
            return;
        }
        value = raw(text, parsed["value"]);
    }
    if (call->scripting)
        value = "[{\"frameId\":" + std::to_string(call->frameId) + ",\"result\":" + value + "}]";
    else if (call->wrapInArray)
        value = "[" + value + "]";
    complete(call->listener.get(), ok(value));
}

// Wraps `body` (statements, last one an expression or "return") so its
// result comes back as JSON text.
std::string resultWrapper(const std::string& expression)
{
    return "(() => { try { const __prismResult = (" + expression + ");"
        " return JSON.stringify({ ok: true, value: (__prismResult === undefined || typeof __prismResult === 'function' || (__prismResult && typeof __prismResult.then === 'function')) ? null : __prismResult });"
        " } catch (error) { return JSON.stringify({ ok: false, error: String((error && error.message) || error) }); } })()";
}

bool mayScript(const Extension& ext, const tabs::Info& tab)
{
    std::string url = util::toUTF8(tab.url);
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
        return internal::extensionForURL(url) == &ext;
    if (g_activeTab.count({ ext.id, tab.id }))
        return true;
    for (auto& host : ext.hosts) {
        if (matchesPattern(host, url))
            return true;
    }
    return false;
}

void runInTab(Extension& ext, int tabId, const std::string& script, std::unique_ptr<ScriptCall> call)
{
    tabs::Info tab;
    if (!tabs::find(tabId, tab)) {
        complete(call->listener.get(), fail("No tab with id: " + std::to_string(tabId) + "."));
        return;
    }
    if (!mayScript(ext, tab)) {
        complete(call->listener.get(), fail("Cannot access contents of the page. Extension manifest must request permission to access the respective host."));
        return;
    }
    WKPageEvaluateJavaScriptInMainFrame(tab.page, stringUTF8(script).get(), call.release(), scriptFinished);
}

std::string readFiles(const Extension& ext, const json::Value& files)
{
    std::string code;
    for (auto& file : files.items()) {
        fs::path path = internal::resolve(ext, file.asString());
        if (!path.empty())
            code += util::readFile(path.wstring()) + "\n;\n";
    }
    return code;
}

std::string cssScript(const std::string& extId, const std::string& css, bool remove)
{
    std::string quoted = json::quote(css);
    if (remove) {
        return "(() => { for (const style of document.querySelectorAll('style[data-prism-extension]')) {"
            " if (style.dataset.prismExtension === " + json::quote(extId) + " && style.textContent === " + quoted + ") style.remove(); }"
            " return JSON.stringify({ ok: true, value: null }); })()";
    }
    return "(() => { const style = document.createElement('style'); style.dataset.prismExtension = " + json::quote(extId) + ";"
        " style.textContent = " + quoted + "; (document.head || document.documentElement).appendChild(style);"
        " return JSON.stringify({ ok: true, value: null }); })()";
}

// ---------------------------------------------------------------------------
// Tab events (for chrome.tabs.onUpdated and friends)

struct TabSnapshot {
    std::wstring url, title;
    bool loading { false };
    bool known { false };
};
std::map<int, TabSnapshot> g_tabSnapshots;

void broadcastTabEvent(const std::string& name, const std::function<std::string(const Extension&)>& args)
{
    for (Extension* ext : internal::enabledExtensions()) {
        std::string json = args(*ext);
        if (!json.empty())
            broadcastToPages(ext->id, eventJSON(name, json));
    }
}

} // namespace

// ===========================================================================
// Called from Extensions.cpp

namespace host {

std::string backgroundStatusJSON(const std::string& id)
{
    return backgroundStatus(id);
}

void extensionsChanged()
{
    ensureTick();
    // Background pages of extensions that were turned off or removed.
    std::vector<std::string> stale;
    for (auto& [id, host] : g_backgrounds) {
        if (!internal::findEnabled(id))
            stale.push_back(id);
    }
    for (auto& id : stale) {
        stopBackground(id);
        killContextsOfExtension(id);
    }
    if (g_popup && !internal::findEnabled(g_popup->extId))
        closePopup();
    for (Extension* ext : internal::enabledExtensions())
        startBackground(*ext);
    if (!g_browserStarted) {
        g_browserStarted = true;
        for (Extension* ext : internal::enabledExtensions())
            sendToBackground(ext->id, eventJSON("runtime.onStartup", "[]"));
    }
    std::vector<server::Site> sites;
    for (Extension* ext : internal::enabledExtensions())
        sites.push_back({ ext->id, ext->dir, ext->webAccessible });
    server::setSites(std::move(sites));
    engine::setUserScripts("extension-pages", { });
    std::vector<std::string> scripts;
    std::string runtime = util::readFile(util::exeDirectory() + L"\\ui\\extension-page.js");
    while (!runtime.empty() && (runtime.back() == '\n' || runtime.back() == '\r' || runtime.back() == ' '))
        runtime.pop_back();
    if (!runtime.empty()) {
        for (Extension* ext : internal::enabledExtensions()) {
            std::string config = "{\"id\":" + json::quote(ext->id) + ",\"name\":" + json::quote(ext->name)
                + ",\"base\":" + json::quote(baseURL(*ext)) + ",\"uiLanguage\":" + json::quote(ext->uiLanguage)
                + ",\"manifest\":" + ext->manifestJSON + ",\"messages\":" + ext->messagesJSON
                + ",\"family\":\"" + (ext->source == "firefox" ? "firefox" : "chrome") + "\""
                + ",\"worker\":" + (ext->backgroundPage.empty() && ext->backgroundScripts.empty() && !ext->serviceWorker.empty() ? "true" : "false") + "}";
            scripts.push_back("/* Prism extension pages: " + ext->name + " */\n" + runtime + "(" + config + ");\n");
        }
    }
    engine::setUserScripts("extension-pages", std::move(scripts));
    refreshToolbars();
}

void installed(const std::string& id, bool update, const std::string& previousVersion)
{
    std::string details = update ? "{\"reason\":\"update\",\"previousVersion\":" + json::quote(previousVersion) + "}" : "{\"reason\":\"install\"}";
    sendToBackground(id, eventJSON("runtime.onInstalled", "[" + details + "]"));
}

void stop(const std::string& id)
{
    if (g_popup && g_popup->extId == id)
        closePopup();
    stopBackground(id);
    killContextsOfExtension(id);
    g_waitingForBackground.erase(id);
}

void removed(const std::string& id)
{
    stop(id);
    g_actions.erase(id);
    g_alarms.erase(id);
    refreshToolbars();
}

void storageChanged(Extension& ext, const std::string& area, const std::string& changesJSON, int fromContext)
{
    if (changesJSON == "{}" || changesJSON.empty())
        return;
    std::string event = "{\"type\":\"storageChanged\",\"area\":" + json::quote(area) + ",\"changes\":" + changesJSON + "}";
    for (auto& [id, context] : g_contexts) {
        if (context.extId == ext.id && id != fromContext)
            deliver(context, event);
    }
}

void handleContentMessage(Extension& ext, WKScriptMessageRef message, const std::string& text, const json::Value& parsed, WKCompletionListenerRef listener)
{
    std::string op = parsed["op"].asString();
    perf::Scope busy("an extension's content script message", ext.name + ": " + op);
    WKFrameInfoRef frame = WKScriptMessageGetFrameInfo(message);
    WKPageRef page = frame ? WKFrameInfoGetPage(frame) : nullptr;

    if (op == "hello") {
        Context context;
        context.id = g_nextContext++;
        context.extId = ext.id;
        context.kind = Kind::Content;
        context.page = page;
        context.mainFrame = frame && WKFrameInfoGetIsMainFrame(frame);
        context.frameId = context.mainFrame ? 0 : g_nextFrameId++;
        context.url = parsed["url"].asString();
        context.repollDeadline = GetTickCount64() + 10000;
        int id = context.id;
        g_contexts[id] = std::move(context);
        tabs::Info tab;
        int tabId = tabs::findByPage(page, tab) ? tab.id : -1;
        complete(listener, ok("{\"ctx\":" + std::to_string(id) + ",\"frameId\":" + std::to_string(g_contexts[id].frameId) + ",\"tabId\":" + std::to_string(tabId) + "}"));
        return;
    }

    auto found = g_contexts.find(static_cast<int>(parsed["ctx"].asNumber()));
    if (found == g_contexts.end() || found->second.extId != ext.id || found->second.page != page || found->second.kind != Kind::Content) {
        complete(listener, fail("Unknown context"));
        return;
    }
    if (!commonOperation(found->second, op, text, parsed, listener))
        complete(listener, fail("Unsupported operation: " + op));
}

} // namespace host

// ===========================================================================
// Extension pages (ui/extension-page.js): background, popup, options tabs

namespace {

Extension* extensionForPage(WKPageRef page, Kind& kind, int& tabId)
{
    tabId = -1;
    if (HostView* host = hostForPage(page)) {
        kind = host->kind;
        tabId = host->tabId;
        return internal::findEnabled(host->extId);
    }
    tabs::Info tab;
    if (tabs::findByPage(page, tab)) {
        Extension* ext = internal::extensionForURL(util::toUTF8(tab.url));
        if (ext && ext->enabled) {
            kind = Kind::Page;
            tabId = tab.id;
            return ext;
        }
    }
    return nullptr;
}

int targetTab(const Context& context, const json::Value& value)
{
    if (value.isNumber())
        return static_cast<int>(value.asNumber());
    // The active tab of the window the call came from.
    tabs::Info tab;
    int windowId = tabs::findByPage(context.page, tab) ? tab.windowId : tabs::lastFocusedWindow();
    for (auto& window : tabs::windows()) {
        if (window.id != windowId)
            continue;
        for (auto& candidate : window.tabs) {
            if (candidate.active)
                return candidate.id;
        }
    }
    return -1;
}

int currentWindow(const Context& context)
{
    tabs::Info tab;
    if (tabs::findByPage(context.page, tab))
        return tab.windowId;
    if (g_popup && g_popup->page == context.page)
        if (BrowserWindow* window = BrowserWindow::fromHWND(g_popup->owner))
            return window->id();
    return tabs::lastFocusedWindow();
}

std::wstring extensionTarget(Extension& ext, const std::string& url)
{
    // Relative URLs from extension pages are inside the extension.
    if (url.find(':') == std::string::npos)
        return util::fromUTF8(baseURL(ext) + (url.empty() || url[0] != '/' ? url : url.substr(1)));
    std::string path;
    if (Extension* owner = internal::extensionForURL(url, &path))
        return util::fromUTF8(baseURL(*owner) + path);
    return util::fromUTF8(url);
}

bool tabMatchesQuery(const Extension& ext, const tabs::Info& tab, const tabs::WindowInfo& window, const Context& context, const json::Value& query)
{
    if (query["active"].isBool() && query["active"].asBool() != tab.active)
        return false;
    if (query["highlighted"].isBool() && query["highlighted"].asBool() != tab.active)
        return false;
    if (query["pinned"].isBool() && query["pinned"].asBool())
        return false;
    if (query["audible"].isBool() && query["audible"].asBool())
        return false;
    if (query["discarded"].isBool() && query["discarded"].asBool())
        return false;
    if (query["status"].isString() && query["status"].asString() != (tab.loading ? "loading" : "complete"))
        return false;
    if (query["windowId"].isNumber()) {
        int windowId = static_cast<int>(query["windowId"].asNumber());
        if (windowId == -2)
            windowId = currentWindow(context);
        if (windowId != window.id)
            return false;
    }
    if (query["currentWindow"].isBool() && query["currentWindow"].asBool() != (window.id == currentWindow(context)))
        return false;
    if (query["lastFocusedWindow"].isBool() && query["lastFocusedWindow"].asBool() != (window.id == tabs::lastFocusedWindow()))
        return false;
    if (query["index"].isNumber() && static_cast<int>(query["index"].asNumber()) != tab.index)
        return false;
    std::string url = util::toUTF8(tab.url);
    if (query.has("url") && !query["url"].isNull()) {
        if (!canSee(ext, tab.id, url))
            return false;
        bool any = false;
        if (query["url"].isString())
            any = matchesPattern(query["url"].asString(), url);
        for (auto& pattern : query["url"].items())
            any = any || matchesPattern(pattern.asString(), url);
        if (!any)
            return false;
    }
    if (query["title"].isString()) {
        if (!canSee(ext, tab.id, url))
            return false;
        std::string pattern = query["title"].asString(), title = util::toUTF8(tab.title);
        std::string regex;
        for (char c : pattern)
            regex += c == '*' ? std::string(".*") : (strchr(".+?^${}()|[]\\", c) ? std::string("\\") + c : std::string(1, c));
        try {
            if (!std::regex_match(title, std::regex(regex)))
                return false;
        } catch (...) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> permissionItems(const json::Value& value)
{
    std::vector<std::string> out;
    for (auto& item : value["permissions"].items())
        out.push_back(item.asString());
    for (auto& item : value["origins"].items())
        out.push_back(item.asString());
    return out;
}

void pageOperation(Extension& ext, Context& context, const std::string& op, const std::string& text, const json::Value& message, WKCompletionListenerRef listener)
{
    if (commonOperation(context, op, text, message, listener))
        return;

    if (op.rfind("storage.", 0) == 0) {
        std::string error;
        std::string result = internal::storage(ext, op, text, message, error);
        if (!error.empty()) {
            complete(listener, fail(error));
            return;
        }
        if (op != "storage.get" && op != "storage.bytes") {
            json::Value changed;
            if (json::parse(result, changed))
                host::storageChanged(ext, message["area"].asString(), raw(result, changed["changes"]), context.id);
        }
        complete(listener, ok(result));
        return;
    }

    // ---- runtime ----
    if (op == "runtime.openOptionsPage") {
        if (ext.optionsPage.empty()) {
            complete(listener, fail("Could not create an options page."));
            return;
        }
        std::wstring target = util::fromUTF8(baseURL(ext) + ext.optionsPage);
        // Switch to it if it's already open.
        for (auto& window : tabs::windows()) {
            for (auto& tab : window.tabs) {
                if (internal::extensionForURL(util::toUTF8(tab.url)) == &ext && tab.url.find(util::fromUTF8(ext.optionsPage)) != std::wstring::npos) {
                    tabs::activate(tab.id);
                    tabs::focusWindow(window.id);
                    complete(listener, ok());
                    return;
                }
            }
        }
        tabs::create(currentWindow(context), target, true);
        if (g_popup && g_popup->extId == ext.id)
            PostMessageW(g_popup->hwnd, WM_HOST_CLOSE, 0, 0);
        complete(listener, ok());
        return;
    }
    if (op == "runtime.getContexts") {
        // Extension pages Prism has open (not content scripts), as Chrome
        // describes them; the page filters them.
        std::string out = "[";
        for (auto& [id, candidate] : g_contexts) {
            if (candidate.extId != ext.id || candidate.kind == Kind::Content)
                continue;
            HostView* host = hostForPage(candidate.page);
            const char* type = candidate.kind == Kind::Background ? "BACKGROUND" : candidate.kind == Kind::Popup ? "POPUP"
                : host && host->offscreen ? "OFFSCREEN_DOCUMENT" : "TAB";
            tabs::Info tab;
            int tabId = tabs::findByPage(candidate.page, tab) ? tab.id : -1;
            if (out.size() > 1)
                out += ",";
            out += "{\"contextType\":\"" + std::string(type) + "\",\"contextId\":\"prism-" + std::to_string(id) + "\",\"tabId\":" + std::to_string(tabId)
                + ",\"windowId\":" + std::to_string(tabId >= 0 ? tab.windowId : -1) + ",\"frameId\":" + std::to_string(tabId >= 0 ? 0 : -1)
                + ",\"documentId\":\"prism-doc-" + std::to_string(id) + "\",\"documentUrl\":" + json::quote(candidate.url)
                + ",\"documentOrigin\":" + json::quote(candidate.url.substr(0, candidate.url.find('/', candidate.url.find("://") == std::string::npos ? 0 : candidate.url.find("://") + 3))) + ",\"incognito\":false}";
        }
        complete(listener, ok(out + "]"));
        return;
    }
    if (op == "offscreen.create") {
        auto existing = g_offscreen.find(ext.id);
        if (existing != g_offscreen.end() && existing->second->closeAt && !existing->second->created) {
            // Closed a moment ago and still loaded: reuse it (extensions like
            // Bitwarden open and close one around every storage call).
            existing->second->closeAt = 0;
            complete(listener, ok());
            return;
        }
        if (existing != g_offscreen.end()) {
            complete(listener, fail("Only a single offscreen document may be created."));
            return;
        }
        std::string path = message["url"].asString();
        std::string full = path.rfind(baseURL(ext), 0) == 0 ? path : baseURL(ext) + (path.size() && path[0] == '/' ? path.substr(1) : path);
        if (full.rfind(baseURL(ext), 0) != 0 || full.find("..") != std::string::npos) {
            complete(listener, fail("The offscreen document must be a page of the extension."));
            return;
        }
        auto host = createHost(ext.id, Kind::Page, nullptr);
        if (!host) {
            complete(listener, fail("Couldn't create the offscreen document."));
            return;
        }
        host->offscreen = true;
        host->created = Ref<WKCompletionListenerRef>::retain(listener);
        consoleNote(ext.id, "prism", "Opening offscreen document " + full.substr(baseURL(ext).size()) + ".");
        WKPageLoadURL(host->page, url(util::fromUTF8(full)).get());
        g_offscreen[ext.id] = std::move(host);
        return;
    }
    if (op == "offscreen.close") {
        auto found = g_offscreen.find(ext.id);
        if (found == g_offscreen.end() || found->second->closeAt) {
            complete(listener, fail("No current offscreen document."));
            return;
        }
        found->second->closeAt = GetTickCount64() + 30000;
        ensureTick();
        complete(listener, ok());
        return;
    }
    if (op == "offscreen.has") {
        auto found = g_offscreen.find(ext.id);
        complete(listener, ok(found != g_offscreen.end() && !found->second->created && !found->second->closeAt ? "true" : "false"));
        return;
    }
    if (op == "runtime.reload") {
        std::string id = ext.id;
        complete(listener, ok());
        // An extension that keeps reloading itself is usually stuck starting up.
        BackgroundHistory& history = g_backgroundHistory[id];
        history.selfReloads++;
        ULONGLONG now = GetTickCount64();
        history.recentReloads.push_back(now);
        history.recentReloads.erase(std::remove_if(history.recentReloads.begin(), history.recentReloads.end(), [&](ULONGLONG at) { return now - at > 60000; }),
            history.recentReloads.end());
        consoleNote(id, "prism", "The extension asked to reload itself (runtime.reload, from its " + std::string(kindName(context.kind)) + ").");
        if (history.recentReloads.size() == 3)
            logBackgroundProblem(id, "The extension reloaded itself 3 times within a minute. It may be failing to start; see Background page › Show log.");
        util::postToMainThread([id] {
            // Like Chrome, a reload closes the extension's popup too.
            if (g_popup && g_popup->extId == id)
                closePopup();
            stopBackground(id);
            killContextsOfExtension(id);
            if (Extension* again = internal::findEnabled(id))
                startBackground(*again);
        });
        return;
    }
    if (op == "runtime.readFile") {
        std::string path;
        Extension* owner = internal::extensionForURL(message["url"].asString(), &path);
        fs::path file = owner == &ext ? internal::resolve(ext, path) : fs::path();
        std::error_code error;
        if (file.empty() || !fs::is_regular_file(file, error)) {
            complete(listener, fail("Not found"));
            return;
        }
        complete(listener, ok("{\"base64\":\"" + util::base64(util::readFile(file.wstring())) + "\"}"));
        return;
    }

    if (op == "runtime.fetch") {
        // fetch() to another site: Prism makes the request (without cookies)
        // if the extension has permission for that site, like Chrome lets
        // extensions skip CORS there.
        std::string target = message["url"].asString();
        bool allowed = target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0;
        bool permitted = false;
        for (auto& host : ext.hosts)
            permitted = permitted || matchesPattern(host, target);
        if (!allowed || !permitted) {
            complete(listener, ok("{\"fallback\":true}"));
            return;
        }
        http::Request request;
        request.url = util::fromUTF8(target);
        request.method = util::fromUTF8(message["method"].asString().empty() ? std::string("GET") : message["method"].asString());
        for (auto& pair : message["headers"].items())
            request.headers.emplace_back(util::fromUTF8(pair[0].asString()), util::fromUTF8(pair[1].asString()));
        request.body = decodeBase64(message["body"].asString());
        auto held = Ref<WKCompletionListenerRef>::retain(listener);
        std::thread([request, held] {
            http::Response response;
            std::wstring error;
            bool done = http::request(request, response, error);
            std::string json;
            if (!done)
                json = fail(util::toUTF8(error));
            else {
                std::string headers = "[";
                for (size_t i = 0; i < response.headers.size(); ++i)
                    headers += (i ? "," : "") + std::string("[") + json::quote(util::toUTF8(response.headers[i].first)) + "," + json::quote(util::toUTF8(response.headers[i].second)) + "]";
                json = ok("{\"status\":" + std::to_string(response.status) + ",\"statusText\":" + json::quote(util::toUTF8(response.statusText))
                    + ",\"url\":" + json::quote(util::toUTF8(response.url)) + ",\"headers\":" + headers + "],\"base64\":\"" + util::base64(response.body) + "\"}");
            }
            util::postToMainThread([held, json] { complete(held.get(), json); });
        }).detach();
        return;
    }

    // ---- tabs ----
    if (op == "tabs.query") {
        std::string json = "[";
        bool first = true;
        for (auto& window : tabs::windows()) {
            for (auto& tab : window.tabs) {
                if (!tabMatchesQuery(ext, tab, window, context, message["query"]))
                    continue;
                json += (first ? "" : ",") + tabJSON(ext, tab);
                first = false;
            }
        }
        complete(listener, ok(json + "]"));
        return;
    }
    if (op == "tabs.get") {
        tabs::Info tab;
        if (!tabs::find(static_cast<int>(message["tabId"].asNumber(-1)), tab)) {
            complete(listener, fail("No tab with id: " + std::to_string(static_cast<int>(message["tabId"].asNumber(-1))) + "."));
            return;
        }
        complete(listener, ok(tabJSON(ext, tab)));
        return;
    }
    if (op == "tabs.getCurrent") {
        tabs::Info tab;
        complete(listener, ok(context.kind == Kind::Page && tabs::findByPage(context.page, tab) ? tabJSON(ext, tab) : "null"));
        return;
    }
    if (op == "tabs.create") {
        const json::Value& properties = message["properties"];
        int windowId = properties["windowId"].isNumber() ? static_cast<int>(properties["windowId"].asNumber()) : currentWindow(context);
        if (windowId == -2)
            windowId = currentWindow(context);
        std::wstring target = properties["url"].isString() ? extensionTarget(ext, properties["url"].asString()) : L"prism://newtab";
        int index = properties["index"].isNumber() ? static_cast<int>(properties["index"].asNumber()) : -1;
        int tabId = tabs::create(windowId, target, properties["active"].asBool(true) && properties["selected"].asBool(true), index);
        tabs::Info tab;
        complete(listener, tabs::find(tabId, tab) ? ok(tabJSON(ext, tab)) : fail("Couldn't open a tab."));
        if (g_popup && g_popup->extId == ext.id && properties["active"].asBool(true))
            PostMessageW(g_popup->hwnd, WM_HOST_CLOSE, 0, 0);
        return;
    }
    if (op == "tabs.update") {
        int tabId = targetTab(context, message["tabId"]);
        const json::Value& properties = message["properties"];
        tabs::Info tab;
        if (!tabs::find(tabId, tab)) {
            complete(listener, fail("No tab with id: " + std::to_string(tabId) + "."));
            return;
        }
        if (properties["url"].isString())
            tabs::navigate(tabId, extensionTarget(ext, properties["url"].asString()));
        if (properties["active"].asBool() || properties["highlighted"].asBool() || properties["selected"].asBool())
            tabs::activate(tabId);
        tabs::find(tabId, tab);
        complete(listener, ok(tabJSON(ext, tab)));
        return;
    }
    if (op == "tabs.remove") {
        for (auto& id : message["tabIds"].items())
            tabs::close(static_cast<int>(id.asNumber()));
        complete(listener, ok());
        return;
    }
    if (op == "tabs.reload") {
        tabs::reload(targetTab(context, message["tabId"]), message["bypassCache"].asBool());
        complete(listener, ok());
        return;
    }
    if (op == "tabs.goBack" || op == "tabs.goForward") {
        int tabId = targetTab(context, message["tabId"]);
        if (op == "tabs.goBack")
            tabs::goBack(tabId);
        else
            tabs::goForward(tabId);
        complete(listener, ok());
        return;
    }
    if (op == "tabs.executeScript" || op == "tabs.insertCSS" || op == "tabs.removeCSS") {
        int tabId = targetTab(context, message["tabId"]);
        const json::Value& details = message["details"];
        auto call = std::make_unique<ScriptCall>();
        call->listener = Ref<WKCompletionListenerRef>::retain(listener);
        if (op == "tabs.executeScript") {
            call->wrapInArray = true;
            std::string code = details["code"].isString() ? details["code"].asString() : readFiles(ext, details["files"]);
            // Files get the chrome.* API, like content scripts.
            std::string script = details["code"].isString()
                ? resultWrapper("(0, eval)(" + json::quote(code) + ")")
                : resultWrapper(internal::injectionScript(ext, code));
            runInTab(ext, tabId, script, std::move(call));
        } else {
            std::string css = details["code"].isString() ? details["code"].asString() : readFiles(ext, details["files"]);
            runInTab(ext, tabId, cssScript(ext.id, css, op == "tabs.removeCSS"), std::move(call));
        }
        return;
    }

    // ---- scripting (MV3) ----
    if (op == "scripting.executeScript" || op == "scripting.insertCSS" || op == "scripting.removeCSS") {
        const json::Value& details = message["details"];
        int tabId = static_cast<int>(details["target"]["tabId"].asNumber(-1));
        auto call = std::make_unique<ScriptCall>();
        call->listener = Ref<WKCompletionListenerRef>::retain(listener);
        if (op == "scripting.executeScript") {
            call->scripting = true;
            std::string script;
            if (details["func"].isString())
                script = resultWrapper("(" + details["func"].asString() + ").apply(null, " + (details["args"].isArray() ? raw(text, details["args"]) : "[]") + ")");
            else
                script = resultWrapper(internal::injectionScript(ext, readFiles(ext, details["files"])));
            runInTab(ext, tabId, script, std::move(call));
        } else {
            std::string css = details["css"].isString() ? details["css"].asString() : readFiles(ext, details["files"]);
            runInTab(ext, tabId, cssScript(ext.id, css, op == "scripting.removeCSS"), std::move(call));
        }
        return;
    }

    // ---- windows ----
    if (op == "windows.getAll" || op == "windows.get" || op == "windows.getCurrent" || op == "windows.getLastFocused") {
        bool populate = message["populate"].asBool();
        int wanted = op == "windows.get" ? static_cast<int>(message["windowId"].asNumber(-1))
            : op == "windows.getCurrent" ? currentWindow(context)
            : op == "windows.getLastFocused" ? tabs::lastFocusedWindow() : -100;
        if (wanted == -2)
            wanted = currentWindow(context);
        std::string json = op == "windows.getAll" ? "[" : "";
        bool first = true;
        for (auto& window : tabs::windows()) {
            if (op == "windows.getAll") {
                json += (first ? "" : ",") + windowJSON(ext, window, populate);
                first = false;
            } else if (window.id == wanted)
                json = windowJSON(ext, window, populate);
        }
        if (op == "windows.getAll")
            json += "]";
        complete(listener, json.empty() ? fail("No window with id: " + std::to_string(wanted) + ".") : ok(json));
        return;
    }
    if (op == "windows.create") {
        const json::Value& data = message["data"];
        std::vector<std::wstring> urls;
        if (data["url"].isString())
            urls.push_back(extensionTarget(ext, data["url"].asString()));
        for (auto& item : data["url"].items())
            urls.push_back(extensionTarget(ext, item.asString()));
        int windowId = tabs::createWindow(urls, data["focused"].asBool(true));
        std::string json = "null";
        for (auto& window : tabs::windows()) {
            if (window.id == windowId)
                json = windowJSON(ext, window, true);
        }
        complete(listener, ok(json));
        return;
    }
    if (op == "windows.update") {
        int windowId = static_cast<int>(message["windowId"].asNumber(-1));
        if (windowId == -2)
            windowId = currentWindow(context);
        const json::Value& info = message["info"];
        HWND hwnd = tabs::windowHandle(windowId);
        if (hwnd) {
            std::string state = info["state"].asString();
            if (state == "minimized")
                ShowWindow(hwnd, SW_MINIMIZE);
            else if (state == "maximized")
                ShowWindow(hwnd, SW_MAXIMIZE);
            else if (state == "normal")
                ShowWindow(hwnd, SW_RESTORE);
            if (info["focused"].asBool() || info["drawAttention"].asBool())
                tabs::focusWindow(windowId);
        }
        std::string json = "null";
        for (auto& window : tabs::windows()) {
            if (window.id == windowId)
                json = windowJSON(ext, window, false);
        }
        complete(listener, ok(json));
        return;
    }
    if (op == "windows.remove") {
        tabs::closeWindow(static_cast<int>(message["windowId"].asNumber(-1)));
        complete(listener, ok());
        return;
    }

    // ---- action (toolbar button) ----
    if (op == "action.set") {
        std::string property = message["property"].asString();
        static const std::set<std::string> allowed = { "title", "icon", "popup", "badgeText", "badgeColor", "badgeTextColor", "enabled" };
        if (!allowed.count(property)) {
            complete(listener, fail("Unknown property"));
            return;
        }
        std::string value = message["value"].asString();
        if (property == "popup" && !value.empty() && value.find(':') != std::string::npos) {
            std::string path;
            value = internal::extensionForURL(value, &path) == &ext ? path : "";
        }
        if (property == "icon" && value.rfind("data:image/", 0) != 0) {
            std::string path;
            if (value.find(':') == std::string::npos)
                path = value[0] == '/' ? value.substr(1) : value;
            else if (internal::extensionForURL(value, &path) != &ext)
                path.clear();
            value = internal::resolve(ext, path).empty() ? std::string() : internal::fileURL(ext, path);
        }
        auto& state = g_actions[ext.id];
        bool reset = message["value"].isNull();
        if (message["tabId"].isNumber()) {
            auto& tab = state.perTab[static_cast<int>(message["tabId"].asNumber())];
            if (reset)
                tab.erase(property);
            else
                tab[property] = value;
        } else if (reset)
            state.global.erase(property);
        else
            state.global[property] = value;
        refreshToolbars();
        complete(listener, ok());
        return;
    }
    if (op == "action.get") {
        int tabId = message["tabId"].isNumber() ? static_cast<int>(message["tabId"].asNumber()) : -1;
        std::string value = actionValue(ext, message["property"].asString(), tabId);
        if (message["property"].asString() == "enabled")
            complete(listener, ok(value == "0" ? "false" : "true"));
        else
            complete(listener, ok(json::quote(value)));
        return;
    }
    if (op == "action.openPopup") {
        int windowId = currentWindow(context);
        HWND hwnd = tabs::windowHandle(windowId);
        int tabId = targetTab(context, json::Value());
        complete(listener, ok());
        std::string id = ext.id;
        util::postToMainThread([hwnd, tabId, id] { actionClicked(hwnd, tabId, id, -1, -1); });
        return;
    }

    // ---- alarms ----
    if (op == "alarms.create") {
        const json::Value& info = message["info"];
        Alarm alarm;
        alarm.name = message["name"].asString();
        double now = nowMilliseconds();
        alarm.periodInMinutes = info["periodInMinutes"].asNumber(0);
        if (info["when"].isNumber())
            alarm.scheduledTime = info["when"].asNumber();
        else if (info["delayInMinutes"].isNumber())
            alarm.scheduledTime = now + info["delayInMinutes"].asNumber() * 60000.0;
        else
            alarm.scheduledTime = now + alarm.periodInMinutes * 60000.0;
        // Chrome's minimum is 30 seconds.
        alarm.scheduledTime = std::max(alarm.scheduledTime, now + 1000.0);
        if (alarm.periodInMinutes > 0)
            alarm.periodInMinutes = std::max(alarm.periodInMinutes, 0.5);
        g_alarms[ext.id][alarm.name] = alarm;
        complete(listener, ok());
        return;
    }
    if (op == "alarms.get" || op == "alarms.getAll") {
        auto& alarms = g_alarms[ext.id];
        if (op == "alarms.get") {
            auto found = alarms.find(message["name"].asString());
            complete(listener, ok(found == alarms.end() ? "null" : alarmJSON(found->second)));
            return;
        }
        std::string json = "[";
        bool first = true;
        for (auto& [name, alarm] : alarms) {
            json += (first ? "" : ",") + alarmJSON(alarm);
            first = false;
        }
        complete(listener, ok(json + "]"));
        return;
    }
    if (op == "alarms.clear") {
        bool removed = g_alarms[ext.id].erase(message["name"].asString()) > 0;
        complete(listener, ok(removed ? "true" : "false"));
        return;
    }
    if (op == "alarms.clearAll") {
        bool any = !g_alarms[ext.id].empty();
        g_alarms[ext.id].clear();
        complete(listener, ok(any ? "true" : "false"));
        return;
    }

    // ---- permissions ----
    if (op == "permissions.getAll") {
        std::string json = "{\"permissions\":[";
        for (size_t i = 0; i < ext.permissions.size(); ++i)
            json += (i ? "," : "") + json::quote(ext.permissions[i]);
        json += "],\"origins\":[";
        for (size_t i = 0; i < ext.hosts.size(); ++i)
            json += (i ? "," : "") + json::quote(ext.hosts[i]);
        complete(listener, ok(json + "]}"));
        return;
    }
    if (op == "permissions.contains") {
        bool all = true;
        for (auto& item : message["permissions"]["permissions"].items())
            all = all && ext.hasPermission(item.asString());
        for (auto& item : message["permissions"]["origins"].items()) {
            bool covered = std::find(ext.hosts.begin(), ext.hosts.end(), item.asString()) != ext.hosts.end();
            for (auto& host : ext.hosts)
                covered = covered || host == "<all_urls>" || host == "*://*/*" || matchesPattern(host, item.asString());
            all = all && covered;
        }
        complete(listener, ok(all ? "true" : "false"));
        return;
    }
    if (op == "permissions.request") {
        std::vector<std::string> items = permissionItems(message["permissions"]);
        std::vector<std::string> missing;
        for (auto& item : items) {
            bool optional = std::find(ext.optionalPermissions.begin(), ext.optionalPermissions.end(), item) != ext.optionalPermissions.end()
                || std::find(ext.optionalHosts.begin(), ext.optionalHosts.end(), item) != ext.optionalHosts.end();
            bool have = ext.hasPermission(item) || std::find(ext.hosts.begin(), ext.hosts.end(), item) != ext.hosts.end();
            if (have)
                continue;
            if (!optional) {
                complete(listener, fail("Only permissions specified in the manifest may be requested."));
                return;
            }
            missing.push_back(item);
        }
        if (missing.empty()) {
            complete(listener, ok("true"));
            return;
        }
        std::wstring list;
        for (auto& item : missing)
            list += L"\n  • " + util::fromUTF8(item);
        HWND owner = g_popup ? g_popup->hwnd : tabs::windowHandle(currentWindow(context));
        if (g_popup)
            g_popup->inDialog = true;
        bool yes = MessageBoxW(owner, (util::fromUTF8(ext.name) + L" asks for more permissions:" + list + L"\n\nAllow?").c_str(),
            L"Extension permissions", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
        if (g_popup)
            g_popup->inDialog = false;
        if (yes) {
            internal::grant(ext, missing);
            internal::contentScriptsChanged();
        }
        complete(listener, ok(yes ? "true" : "false"));
        return;
    }
    if (op == "permissions.remove") {
        internal::revoke(ext, permissionItems(message["permissions"]));
        complete(listener, ok("true"));
        return;
    }

    // ---- declarativeNetRequest ----
    if (op == "dnr.getEnabledRulesets") {
        std::string json = "[";
        bool first = true;
        for (auto& ruleset : ext.rulesets) {
            if (!ext.rulesetEnabled(ruleset))
                continue;
            json += (first ? "" : ",") + json::quote(ruleset.id);
            first = false;
        }
        complete(listener, ok(json + "]"));
        return;
    }
    if (op == "dnr.updateEnabledRulesets") {
        for (auto& id : message["options"]["disableRulesetIds"].items())
            internal::setRulesetEnabled(ext, id.asString(), false);
        for (auto& id : message["options"]["enableRulesetIds"].items())
            internal::setRulesetEnabled(ext, id.asString(), true);
        complete(listener, ok());
        return;
    }

    // ---- Right-click menu items ----
    if (op == "menus.create") {
        const json::Value& props = message["props"];
        MenuItem item;
        item.id = idJSON(props["id"]);
        if (item.id.empty()) {
            complete(listener, fail("A menu item needs an id."));
            return;
        }
        applyMenuProperties(item, props);
        auto& items = menusFor(ext);
        if (std::any_of(items.begin(), items.end(), [&](const MenuItem& other) { return other.id == item.id; })) {
            complete(listener, fail("Cannot create item with duplicate id " + item.id));
            return;
        }
        if (!item.parentId.empty() && std::none_of(items.begin(), items.end(), [&](const MenuItem& other) { return other.id == item.parentId; })) {
            complete(listener, fail("Cannot find menu item with id " + item.parentId));
            return;
        }
        if (items.size() >= 1000) {
            complete(listener, fail("Too many menu items."));
            return;
        }
        items.push_back(std::move(item));
        saveMenus(ext);
        complete(listener, ok());
        return;
    }
    if (op == "menus.update" || op == "menus.remove") {
        std::string id = idJSON(message["id"]);
        auto& items = menusFor(ext);
        auto found = std::find_if(items.begin(), items.end(), [&](const MenuItem& item) { return item.id == id; });
        if (found == items.end()) {
            complete(listener, fail("Cannot find menu item with id " + id));
            return;
        }
        if (op == "menus.update") {
            applyMenuProperties(*found, message["props"]);
        } else {
            // The item and everything under it.
            std::set<std::string> gone { id };
            for (bool more = true; more;) {
                more = false;
                for (auto& item : items) {
                    if (!item.parentId.empty() && gone.count(item.parentId) && gone.insert(item.id).second)
                        more = true;
                }
            }
            items.erase(std::remove_if(items.begin(), items.end(), [&](const MenuItem& item) { return gone.count(item.id) > 0; }), items.end());
        }
        saveMenus(ext);
        complete(listener, ok());
        return;
    }
    if (op == "menus.removeAll") {
        menusFor(ext).clear();
        saveMenus(ext);
        complete(listener, ok());
        return;
    }

    // ---- Keyboard shortcuts ----
    if (op == "commands.getAll") {
        std::string json = "[";
        for (size_t i = 0; i < ext.commands.size(); ++i) {
            auto& command = ext.commands[i];
            json += (i ? "," : "") + std::string("{\"name\":") + json::quote(command.name) + ",\"description\":" + json::quote(command.description)
                + ",\"shortcut\":" + json::quote(command.shortcut) + "}";
        }
        complete(listener, ok(json + "]"));
        return;
    }

    // ---- webRequest ----
    if (op == "webRequest.addListener") {
        WebRequestListener entry;
        entry.context = context.id;
        entry.id = static_cast<int>(message["id"].asNumber(0));
        entry.event = message["event"].asString();
        for (auto& url : message["urls"].items())
            entry.urls.push_back(url.asString());
        for (auto& type : message["types"].items())
            entry.types.insert(type.asString());
        entry.tabId = message["tabId"].isNumber() ? static_cast<int>(message["tabId"].asNumber()) : -1;
        entry.blocking = message["blocking"].asBool();
        if (entry.event == "onBeforeRequest" || entry.event == "onCompleted" || entry.event == "onErrorOccurred") {
            bool first = g_webRequestListeners.empty();
            g_webRequestListeners.push_back(std::move(entry));
            if (first)
                requestfilter::rulesChanged(); // pages start asking
        }
        complete(listener, ok());
        return;
    }
    if (op == "webRequest.removeListener") {
        int id = static_cast<int>(message["id"].asNumber(0));
        g_webRequestListeners.erase(std::remove_if(g_webRequestListeners.begin(), g_webRequestListeners.end(),
            [&](const WebRequestListener& entry) { return entry.context == context.id && entry.id == id; }), g_webRequestListeners.end());
        complete(listener, ok());
        return;
    }
    if (op == "webRequest.respond") {
        std::string requestId = message["requestId"].asString();
        int id = static_cast<int>(message["listener"].asNumber(0));
        auto found = g_webRequestPending.find(requestId);
        if (found != g_webRequestPending.end() && found->second.waiting.erase({ context.id, id })) {
            const json::Value& response = message["response"];
            if (response.isObject()) {
                if (response["cancel"].isBool() && response["cancel"].asBool())
                    found->second.decision.cancel = true;
                const json::Value& redirect = response["redirectUrl"];
                if (redirect.isString() && found->second.decision.redirectURL.empty()) {
                    // chrome-extension:// addresses become the extension's own address.
                    std::wstring resolved = resolveExtensionURL(util::fromUTF8(redirect.asString()));
                    std::string target = util::toUTF8(resolved.empty() ? util::fromUTF8(redirect.asString()) : resolved);
                    if (target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0 || target.rfind("data:", 0) == 0)
                        found->second.decision.redirectURL = target;
                }
            }
            if (found->second.waiting.empty())
                finishWebRequest(requestId);
        }
        complete(listener, ok());
        return;
    }

    complete(listener, fail("Unsupported operation: " + op));
}

} // namespace

void handlePageMessage(WKScriptMessageRef message, WKCompletionListenerRef listener)
{
    WKTypeRef body = WKScriptMessageGetBody(message);
    WKFrameInfoRef frame = WKScriptMessageGetFrameInfo(message);
    if (!body || WKGetTypeID(body) != WKStringGetTypeID() || !frame || !WKFrameInfoGetIsMainFrame(frame)) {
        complete(listener, fail("Invalid message"));
        return;
    }
    WKPageRef page = WKFrameInfoGetPage(frame);
    Kind kind = Kind::Page;
    int tabId = -1;
    Extension* ext = extensionForPage(page, kind, tabId);
    if (!ext) {
        complete(listener, fail("Not an extension page"));
        return;
    }
    std::string text = util::toUTF8(toWide(static_cast<WKStringRef>(body)));
    json::Value parsed;
    if (text.size() > 64 * 1024 * 1024 || !json::parse(text, parsed) || !parsed.isObject()) {
        complete(listener, fail("Invalid message"));
        return;
    }
    std::string op = parsed["op"].asString();
    perf::Scope busy("an extension page's message", ext->name + ": " + op + (text.size() > 100000 ? ", " + std::to_string(text.size() / 1024) + " KB" : ""));

    // One context per extension page.
    Context* context = nullptr;
    for (auto& [id, candidate] : g_contexts) {
        if (candidate.page == page && candidate.kind == kind)
            context = &candidate;
    }
    if (op == "hello") {
        if (context)
            killContext(context->id); // the page reloaded
        Context fresh;
        fresh.id = g_nextContext++;
        fresh.extId = ext->id;
        fresh.kind = kind;
        fresh.page = page;
        fresh.url = parsed["url"].asString();
        fresh.repollDeadline = GetTickCount64() + 10000;
        int id = fresh.id;
        g_contexts[id] = std::move(fresh);
        if (kind == Kind::Background) {
            consoleNote(ext->id, "prism", "Background page connected to Prism.");
            auto waiting = g_waitingForBackground.find(ext->id);
            if (waiting != g_waitingForBackground.end()) {
                for (auto& event : waiting->second)
                    deliver(g_contexts[id], event);
                g_waitingForBackground.erase(waiting);
            }
        }
        int windowId = currentWindow(g_contexts[id]);
        complete(listener, ok("{\"ctx\":" + std::to_string(id) + ",\"kind\":\"" + kindName(kind) + "\",\"tabId\":" + std::to_string(tabId)
            + ",\"windowId\":" + std::to_string(windowId) + "}"));
        return;
    }
    if (isClosedPopupPage(page)) {
        complete(listener, fail("The popup was closed."));
        return;
    }
    if (op == "console") {
        HostView* host = hostForPage(page);
        std::string where = host && host->offscreen ? "offscreen" : kindName(kind);
        for (auto& line : parsed["lines"].items())
            consoleNote(ext->id, where + " " + line["level"].asString(), line["text"].asString().substr(0, 2000));
        complete(listener, ok());
        return;
    }
    if (op == "log") {
        consoleNote(ext->id, std::string(kindName(kind)) + " error", parsed["message"].asString() + (parsed["source"].asString().empty() ? "" : "  (" + parsed["source"].asString() + ")"));
        internal::logError(*ext, kindName(kind), parsed["message"].asString(), parsed["source"].asString());
        complete(listener, ok());
        return;
    }
    if (op == "popup.resize") {
        if (g_popup && g_popup->page == page)
            placePopup(*g_popup, static_cast<int>(parsed["width"].asNumber(360)), static_cast<int>(parsed["height"].asNumber(420)));
        complete(listener, ok());
        return;
    }
    if (!context) {
        complete(listener, fail("Say hello first"));
        return;
    }
    pageOperation(*ext, *context, op, text, parsed, listener);
}

// ---------------------------------------------------------------------------
// Toolbar

// What an extension can do on a tab's site, for the Extensions panel:
// "full" (it can read and change the page) or "none".
std::string siteAccess(const Extension& ext, int tabId)
{
    tabs::Info tab;
    if (!tabs::find(tabId, tab))
        return "none";
    std::string url = util::toUTF8(tab.url);
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
        return "none";
    for (auto& host : ext.hosts)
        if (matchesPattern(host, url))
            return "full";
    for (auto& script : ext.contentScripts)
        for (auto& pattern : script.matches)
            if (matchesPattern(pattern, url))
                return "full";
    return "none";
}

std::string actionsJSON(int tabId)
{
    std::string json = "[";
    bool first = true;
    for (Extension* ext : internal::enabledExtensions()) {
        if (!ext->hasAction)
            continue;
        std::string badge = actionValue(*ext, "badgeText", tabId);
        json += std::string(first ? "" : ",") + "{\"id\":" + json::quote(ext->id)
            + ",\"name\":" + json::quote(ext->name)
            + ",\"title\":" + json::quote(actionValue(*ext, "title", tabId))
            + ",\"icon\":" + json::quote(actionValue(*ext, "icon", tabId))
            + ",\"badge\":" + json::quote(badge)
            + ",\"badgeColor\":" + json::quote(actionValue(*ext, "badgeColor", tabId))
            + ",\"badgeTextColor\":" + json::quote(actionValue(*ext, "badgeTextColor", tabId))
            + ",\"enabled\":" + (actionValue(*ext, "enabled", tabId) == "0" ? "false" : "true")
            + ",\"options\":" + (ext->optionsPage.empty() ? "false" : "true")
            + ",\"access\":" + json::quote(siteAccess(*ext, tabId))
            + ",\"open\":" + (g_popup && g_popup->extId == ext->id ? "true" : "false") + "}";
        first = false;
    }
    return json + "]";
}

void actionClicked(HWND window, int tabId, const std::string& extId, int buttonRight, int buttonBottom)
{
    Extension* ext = internal::findEnabled(extId);
    if (!ext)
        return;
    // A click on the button of the open popup closes it (the click itself
    // already did, by taking focus away).
    if (g_popup) {
        bool same = g_popup->extId == extId;
        closePopup();
        if (same)
            return;
    } else if (g_popupClosedExt == extId && GetTickCount64() - g_popupClosedAt < 300)
        return;

    g_activeTab.insert({ extId, tabId }); // "activeTab": access to this tab until it navigates
    std::string popup = actionValue(*ext, "popup", tabId);
    if (actionValue(*ext, "enabled", tabId) == "0")
        return;
    tabs::Info tab;
    if (popup.empty()) {
        if (tabs::find(tabId, tab)) {
            std::string event = eventJSON("action.onClicked", "[" + tabJSON(*ext, tab) + "]");
            sendToBackground(extId, event);
        }
        return;
    }

    BrowserWindow* browser = BrowserWindow::fromHWND(window);
    if (!browser)
        return;
    auto host = createHost(extId, Kind::Popup, browser->hwnd());
    if (!host)
        return;
    host->tabId = tabId;
    host->scale = browser->scale();
    engine::configurePage(host->page, host->scale);
    if (buttonRight < 0) {
        // action.openPopup(): under the right end of the toolbar.
        RECT rect;
        GetWindowRect(browser->hwnd(), &rect);
        host->anchor = { rect.right - static_cast<LONG>(16 * host->scale), rect.top + static_cast<LONG>(80 * host->scale) };
    } else
        browser->anchorForExtension(buttonRight, buttonBottom, host->anchor);
    std::string path = popup;
    while (!path.empty() && path[0] == '/')
        path.erase(0, 1);
    WKPageLoadURL(host->page, url(util::fromUTF8(baseURL(*ext) + path)).get());
    SetTimer(host->hwnd, kShowPopupTimer, 1500, nullptr);
    g_popup = std::move(host);
    refreshToolbars();
}

// Web Inspector for an extension's hidden pages (the Extensions page and the
// button's right-click menu), so problems that don't end up as errors can be
// looked at in the console.
bool showInspector(WKPageRef page)
{
    if (!page || !WKPageGetInspector || !WKInspectorShow)
        return false;
    WKInspectorRef inspector = WKPageGetInspector(page);
    if (!inspector)
        return false;
    WKInspectorShow(inspector);
    return true;
}

bool inspectBackground(const std::string& extId)
{
    auto found = g_backgrounds.find(extId);
    return found != g_backgrounds.end() && found->second && showInspector(found->second->page);
}

void inspectPopup(HWND window, int tabId, const std::string& extId, int buttonRight, int buttonBottom)
{
    if (!g_popup || g_popup->extId != extId)
        actionClicked(window, tabId, extId, buttonRight, buttonBottom);
    if (g_popup && g_popup->extId == extId) {
        g_popup->inspecting = true;
        showInspector(g_popup->page);
    }
}

void showActionMenu(HWND window, const std::string& extId, int x, int y)
{
    Extension* ext = internal::findEnabled(extId);
    BrowserWindow* browser = BrowserWindow::fromHWND(window);
    if (!ext || !browser)
        return;
    POINT point;
    browser->anchorForExtension(x, y, point);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, util::fromUTF8(ext->name).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (ext->optionsPage.empty() ? MF_GRAYED : 0), 1, L"Options");
    AppendMenuW(menu, MF_STRING, 2, L"Manage extension");
    int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
    DestroyMenu(menu);
    if (choice == 1)
        openOptionsPage(extId, browser->id());
    else if (choice == 2)
        browser->openInternalPage(L"extensions");
}

void openOptionsPage(const std::string& extId, int windowId)
{
    Extension* ext = internal::findEnabled(extId);
    if (!ext || ext->optionsPage.empty())
        return;
    tabs::create(windowId, util::fromUTF8(baseURL(*ext) + ext->optionsPage), true);
}

std::wstring displayURL(const std::wstring& url)
{
    std::string path;
    Extension* ext = internal::extensionForURL(util::toUTF8(url), &path);
    // The extension's own http://<id>.localhost address and older file://
    // addresses both show as chrome-extension://<id>/<path>.
    if (!ext || url.rfind(L"chrome-extension:", 0) == 0 || url.rfind(L"moz-extension:", 0) == 0)
        return url;
    return util::fromUTF8("chrome-extension://" + ext->id + "/" + path);
}

std::wstring resolveExtensionURL(const std::wstring& url)
{
    std::string path;
    Extension* ext = internal::extensionForURL(util::toUTF8(url), &path);
    if (!ext || !ext->enabled)
        return { };
    return util::fromUTF8(baseURL(*ext) + path);
}

// ---------------------------------------------------------------------------
// Tab and window events

void tabCreated(int tabId)
{
    tabs::Info tab;
    if (!tabs::find(tabId, tab))
        return;
    g_tabSnapshots[tabId] = { tab.url, tab.title, tab.loading, true };
    broadcastTabEvent("tabs.onCreated", [&](const Extension& ext) { return "[" + tabJSON(ext, tab) + "]"; });
}

// ---------------------------------------------------------------------------
// webRequest

namespace {

std::string originOf(const std::string& url)
{
    size_t scheme = url.find("://");
    if (scheme == std::string::npos)
        return { };
    size_t end = url.find_first_of("/?#", scheme + 3);
    return url.substr(0, end);
}

// Listeners of `event` in extensions allowed to see `url` (webRequest
// permission plus a host permission for the address).
std::vector<const WebRequestListener*> webRequestTargets(const std::string& event, const std::string& url, const std::string& type, int tabId)
{
    std::vector<const WebRequestListener*> targets;
    for (auto& listener : g_webRequestListeners) {
        if (listener.event != event)
            continue;
        if (listener.tabId >= 0 && listener.tabId != tabId)
            continue;
        if (!listener.types.empty() && !listener.types.count(type))
            continue;
        auto context = g_contexts.find(listener.context);
        if (context == g_contexts.end())
            continue;
        Extension* ext = internal::findEnabled(context->second.extId);
        if (!ext || !ext->hasPermission("webRequest"))
            continue;
        bool hostAllowed = std::any_of(ext->hosts.begin(), ext->hosts.end(), [&](const std::string& host) { return matchesPattern(host, url); });
        if (!hostAllowed)
            continue;
        bool urlMatches = listener.urls.empty()
            || std::any_of(listener.urls.begin(), listener.urls.end(), [&](const std::string& pattern) { return matchesPattern(pattern, url); });
        if (urlMatches)
            targets.push_back(&listener);
    }
    return targets;
}

std::string webRequestEvent(const WebRequestListener& listener, const std::string& detailsJSON, bool wantsAnswer)
{
    return "{\"type\":\"webRequest\",\"listener\":" + std::to_string(listener.id) + ",\"blocking\":" + (wantsAnswer ? "true" : "false")
        + ",\"details\":" + detailsJSON + "}";
}

void ensureWebRequestTimer()
{
    if (!g_webRequestTimer)
        g_webRequestTimer = SetTimer(nullptr, 0, 100, webRequestTimeouts);
}

} // namespace

bool webRequestListening()
{
    return !g_webRequestListeners.empty();
}

void webRequestBefore(const WebRequestDetails& details, std::string& requestId, std::function<void(WebRequestDecision)> done)
{
    requestId.clear();
    auto before = webRequestTargets("onBeforeRequest", details.url, details.type, details.tabId);
    bool wantsEnd = !webRequestTargets("onCompleted", details.url, details.type, details.tabId).empty()
        || !webRequestTargets("onErrorOccurred", details.url, details.type, details.tabId).empty();
    if (before.empty() && !wantsEnd) {
        done({ });
        return;
    }

    std::string id = std::to_string(g_nextWebRequest++);
    std::string fields = "{\"requestId\":" + json::quote(id) + ",\"url\":" + json::quote(details.url)
        + ",\"method\":" + json::quote(details.method.empty() ? "GET" : details.method)
        + ",\"frameId\":" + std::to_string(details.frameId) + ",\"parentFrameId\":" + std::to_string(details.parentFrameId)
        + ",\"tabId\":" + std::to_string(details.tabId) + ",\"type\":" + json::quote(details.type)
        + ",\"timeStamp\":" + numberJSON(nowMilliseconds())
        + ",\"frameType\":" + json::quote(details.frameId == 0 ? "outermost_frame" : "sub_frame")
        + ",\"documentLifecycle\":\"active\"";
    if (!details.documentURL.empty()) {
        std::string origin = originOf(details.documentURL);
        fields += ",\"documentUrl\":" + json::quote(details.documentURL) + ",\"originUrl\":" + json::quote(details.documentURL);
        if (!origin.empty())
            fields += ",\"initiator\":" + json::quote(origin);
    }
    std::string detailsJSON = fields + "}";

    // Any extension that saw it: the page mustn't remember the answer.
    requestId = id;
    if (wantsEnd) {
        ULONGLONG now = GetTickCount64();
        if (g_webRequestTracked.size() > 5000) {
            // Requests whose end never came (pages closed mid-load).
            for (auto it = g_webRequestTracked.begin(); it != g_webRequestTracked.end();)
                it = now - it->second.since > 60 * 1000 ? g_webRequestTracked.erase(it) : std::next(it);
            if (g_webRequestTracked.size() > 5000)
                g_webRequestTracked.clear();
        }
        g_webRequestTracked[id] = { fields, details.url, details.type, details.tabId, now };
    }

    WebRequestPending pending;
    for (auto* listener : before) {
        auto context = g_contexts.find(listener->context);
        if (context == g_contexts.end())
            continue;
        Extension* ext = internal::findEnabled(context->second.extId);
        bool wantsAnswer = listener->blocking && ext && ext->hasPermission("webRequestBlocking");
        deliver(context->second, webRequestEvent(*listener, detailsJSON, wantsAnswer));
        if (wantsAnswer) {
            pending.waiting.insert({ listener->context, listener->id });
            pending.extensionOf[listener->context] = ext->name;
        }
    }
    if (pending.waiting.empty()) {
        done({ });
        return;
    }
    pending.done = std::move(done);
    pending.started = GetTickCount64();
    pending.url = details.url;
    pending.deadline = pending.started + kWebRequestWait;
    g_webRequestPending[id] = std::move(pending);
    ensureWebRequestTimer();
}

void webRequestEnded(const std::string& requestId, bool succeeded, const std::string& error)
{
    auto found = g_webRequestTracked.find(requestId);
    if (found == g_webRequestTracked.end())
        return;
    WebRequestTracked tracked = std::move(found->second);
    g_webRequestTracked.erase(found);
    const char* event = succeeded ? "onCompleted" : "onErrorOccurred";
    std::string detailsJSON = tracked.details + ",\"timeStamp\":" + numberJSON(nowMilliseconds());
    if (succeeded)
        detailsJSON += ",\"fromCache\":false,\"statusCode\":200,\"statusLine\":\"HTTP/1.1 200 OK\"}";
    else
        detailsJSON += ",\"fromCache\":false,\"error\":" + json::quote(error.empty() ? "net::ERR_FAILED" : error) + "}";
    for (auto* listener : webRequestTargets(event, tracked.url, tracked.type, tracked.tabId)) {
        auto context = g_contexts.find(listener->context);
        if (context != g_contexts.end())
            deliver(context->second, webRequestEvent(*listener, detailsJSON, false));
    }
}

void webRequestMainFrame(int tabId, const std::string& url)
{
    if (g_webRequestListeners.empty())
        return;
    WebRequestDetails details;
    details.tabId = tabId;
    details.url = url;
    details.type = "main_frame";
    std::string requestId;
    // Pages are decided before this (Prism doesn't wait for extensions).
    webRequestBefore(details, requestId, [](WebRequestDecision) { });
    g_webRequestTracked.erase(requestId);
}

// ---------------------------------------------------------------------------
// Right-click menu items

namespace {

// The contexts of a right-click, as contextMenus names them.
std::set<std::string> clickContexts(const MenuContext& context)
{
    std::set<std::string> contexts;
    if (!context.selectionText.empty())
        contexts.insert("selection");
    if (!context.linkURL.empty())
        contexts.insert("link");
    if (context.editable)
        contexts.insert("editable");
    if (!context.mediaType.empty())
        contexts.insert(context.mediaType);
    // "page" only when nothing more specific was clicked.
    if (contexts.empty())
        contexts.insert("page");
    return contexts;
}

bool anyPatternMatches(const std::vector<std::string>& patterns, const std::string& url)
{
    return std::any_of(patterns.begin(), patterns.end(), [&](const std::string& pattern) { return matchesPattern(pattern, url); });
}

bool itemApplies(const MenuItem& item, const std::set<std::string>& contexts, const MenuContext& context)
{
    if (!item.visible)
        return false;
    bool contextMatches = false;
    for (auto& name : item.contexts) {
        if ((name == "all" && !contexts.count("action")) || contexts.count(name))
            contextMatches = true;
    }
    if (!contextMatches)
        return false;
    if (!item.documentPatterns.empty() && !anyPatternMatches(item.documentPatterns, context.pageURL))
        return false;
    if (!item.targetPatterns.empty()) {
        std::string target = !context.linkURL.empty() ? context.linkURL : context.srcURL;
        if (!target.empty() && !anyPatternMatches(item.targetPatterns, target))
            return false;
    }
    return true;
}

std::string menuTitle(const std::string& title, const std::string& selection)
{
    std::string shown = selection;
    if (shown.size() > 60)
        shown = shown.substr(0, 57) + "...";
    std::string out;
    for (size_t i = 0; i < title.size(); ++i) {
        if (title[i] == '%' && i + 1 < title.size() && title[i + 1] == 's') {
            out += shown;
            ++i;
        } else {
            out += title[i];
        }
    }
    return out;
}

std::vector<MenuNode> buildNodes(const Extension& ext, const std::vector<MenuItem>& items, const std::string& parent,
    const std::set<std::string>& contexts, const MenuContext& context, int depth)
{
    std::vector<MenuNode> nodes;
    if (depth > 8)
        return nodes;
    for (auto& item : items) {
        if (item.parentId != parent || !itemApplies(item, contexts, context))
            continue;
        MenuNode node;
        node.extensionId = ext.id;
        node.itemId = item.id;
        node.title = menuTitle(item.title, context.selectionText);
        node.type = item.type;
        node.enabled = item.enabled;
        node.checked = item.checked;
        node.children = buildNodes(ext, items, item.id, contexts, context, depth + 1);
        nodes.push_back(std::move(node));
    }
    return nodes;
}

} // namespace

bool hasMenuItems()
{
    for (Extension* ext : internal::enabledExtensions()) {
        if (!menusFor(*ext).empty())
            return true;
    }
    return false;
}

std::vector<MenuNode> pageMenu(const MenuContext& context)
{
    std::vector<MenuNode> out;
    std::set<std::string> contexts = clickContexts(context);
    for (Extension* ext : internal::enabledExtensions()) {
        std::vector<MenuNode> nodes = buildNodes(*ext, menusFor(*ext), "", contexts, context, 0);
        if (nodes.empty())
            continue;
        if (nodes.size() == 1) {
            out.push_back(std::move(nodes[0]));
        } else {
            // Several items: under the extension's name, as in Chrome.
            MenuNode group;
            group.extensionId = ext->id;
            group.title = ext->name;
            group.children = std::move(nodes);
            out.push_back(std::move(group));
        }
    }
    return out;
}

std::vector<MenuNode> actionMenu(const std::string& extensionId)
{
    Extension* ext = internal::findEnabled(extensionId);
    if (!ext)
        return { };
    MenuContext context;
    std::set<std::string> contexts { "action", "browser_action", "page_action" };
    std::vector<MenuNode> nodes = buildNodes(*ext, menusFor(*ext), "", contexts, context, 0);
    if (nodes.size() > 6)
        nodes.resize(6); // Chrome's limit for the button's menu
    return nodes;
}

void menuItemClicked(const std::string& extensionId, const std::string& itemId, const MenuContext& context, int tabId)
{
    Extension* ext = internal::findEnabled(extensionId);
    if (!ext)
        return;
    auto& items = menusFor(*ext);
    auto found = std::find_if(items.begin(), items.end(), [&](const MenuItem& item) { return item.id == itemId; });
    if (found == items.end() || !found->enabled)
        return;

    bool wasChecked = found->checked;
    if (found->type == "checkbox") {
        found->checked = !found->checked;
    } else if (found->type == "radio") {
        // The other radio items next to it (same parent) turn off.
        size_t index = static_cast<size_t>(found - items.begin());
        for (size_t i = index; i-- > 0;) {
            if (items[i].parentId != found->parentId) continue;
            if (items[i].type != "radio") break;
            items[i].checked = false;
        }
        for (size_t i = index + 1; i < items.size(); ++i) {
            if (items[i].parentId != found->parentId) continue;
            if (items[i].type != "radio") break;
            items[i].checked = false;
        }
        found->checked = true;
    }
    if (found->type == "checkbox" || found->type == "radio")
        saveMenus(*ext);

    std::string info = "{\"menuItemId\":" + found->id;
    if (!found->parentId.empty())
        info += ",\"parentMenuItemId\":" + found->parentId;
    info += ",\"editable\":" + std::string(context.editable ? "true" : "false");
    if (!context.pageURL.empty())
        info += ",\"pageUrl\":" + json::quote(context.pageURL);
    if (!context.linkURL.empty())
        info += ",\"linkUrl\":" + json::quote(context.linkURL);
    if (!context.linkText.empty())
        info += ",\"linkText\":" + json::quote(context.linkText);
    if (!context.srcURL.empty())
        info += ",\"srcUrl\":" + json::quote(context.srcURL);
    if (!context.mediaType.empty())
        info += ",\"mediaType\":" + json::quote(context.mediaType);
    if (!context.selectionText.empty())
        info += ",\"selectionText\":" + json::quote(context.selectionText);
    if (found->type == "checkbox" || found->type == "radio")
        info += ",\"wasChecked\":" + std::string(wasChecked ? "true" : "false") + ",\"checked\":" + (found->checked ? "true" : "false");
    info += "}";

    tabs::Info tab;
    std::string tabArg = tabId >= 0 && tabs::find(tabId, tab) ? tabJSON(*ext, tab) : "null";
    broadcastToPages(ext->id, eventJSON("menus.onClicked", "[" + info + "," + tabArg + "]"));
}

// ---------------------------------------------------------------------------
// Keyboard shortcuts

bool runShortcut(HWND window, int tabId, unsigned key, bool ctrl, bool alt, bool shift)
{
    for (Extension* ext : internal::enabledExtensions()) {
        for (auto& command : ext->commands) {
            if (!command.key || command.key != key || command.ctrl != ctrl || command.alt != alt || command.shift != shift)
                continue;
            if (command.name == "_execute_action" || command.name == "_execute_browser_action" || command.name == "_execute_page_action") {
                // Same as clicking its toolbar button (the popup opens under the toolbar's right side).
                RECT bounds { };
                GetClientRect(window, &bounds);
                actionClicked(window, tabId, ext->id, bounds.right - 8, 76);
                return true;
            }
            tabs::Info tab;
            std::string tabArg = tabId >= 0 && tabs::find(tabId, tab) ? tabJSON(*ext, tab) : "null";
            broadcastToPages(ext->id, eventJSON("commands.onCommand", "[" + json::quote(command.name) + "," + tabArg + "]"));
            return true;
        }
    }
    return false;
}

void tabUpdated(int tabId)
{
    tabs::Info tab;
    if (!tabs::find(tabId, tab))
        return;
    TabSnapshot& snapshot = g_tabSnapshots[tabId];
    bool urlChanged = snapshot.url != tab.url, titleChanged = snapshot.title != tab.title, statusChanged = snapshot.loading != tab.loading || !snapshot.known;
    if (!urlChanged && !titleChanged && !statusChanged)
        return;
    if (urlChanged) {
        // "activeTab" access ends when the tab goes somewhere else.
        for (auto it = g_activeTab.begin(); it != g_activeTab.end();) {
            if (it->second == tabId)
                it = g_activeTab.erase(it);
            else
                ++it;
        }
        for (auto& [extId, state] : g_actions) {
            if (state.perTab.erase(tabId))
                refreshToolbars();
        }
    }
    snapshot = { tab.url, tab.title, tab.loading, true };
    broadcastTabEvent("tabs.onUpdated", [&](const Extension& ext) {
        std::string url = util::toUTF8(tab.url);
        bool visible = canSee(ext, tabId, url);
        std::string change = "{";
        bool first = true;
        auto add = [&](const std::string& item) { change += (first ? "" : ",") + item; first = false; };
        if (statusChanged)
            add(std::string("\"status\":") + (tab.loading ? "\"loading\"" : "\"complete\""));
        if (urlChanged && visible)
            add("\"url\":" + json::quote(url));
        if (titleChanged && visible)
            add("\"title\":" + json::quote(util::toUTF8(tab.title)));
        if (first)
            return std::string(); // nothing this extension may see
        return "[" + std::to_string(tabId) + "," + change + "}," + tabJSON(ext, tab) + "]";
    });
}

void tabActivated(int tabId, int windowId)
{
    broadcastTabEvent("tabs.onActivated", [&](const Extension&) {
        return "[{\"tabId\":" + std::to_string(tabId) + ",\"windowId\":" + std::to_string(windowId) + "}]";
    });
    refreshToolbars();
}

void tabRemoved(int tabId, int windowId, bool windowClosing)
{
    tabs::Info tab;
    if (tabs::find(tabId, tab))
        killContextsOfPage(tab.page);
    g_tabSnapshots.erase(tabId);
    for (auto& [extId, state] : g_actions)
        state.perTab.erase(tabId);
    for (auto it = g_activeTab.begin(); it != g_activeTab.end();) {
        if (it->second == tabId)
            it = g_activeTab.erase(it);
        else
            ++it;
    }
    broadcastTabEvent("tabs.onRemoved", [&](const Extension&) {
        return "[" + std::to_string(tabId) + ",{\"windowId\":" + std::to_string(windowId) + ",\"isWindowClosing\":" + (windowClosing ? "true" : "false") + "}]";
    });
}

void windowCreated(int windowId)
{
    broadcastTabEvent("windows.onCreated", [&](const Extension&) {
        return "[{\"id\":" + std::to_string(windowId) + ",\"focused\":true,\"incognito\":false,\"type\":\"normal\",\"state\":\"normal\",\"alwaysOnTop\":false}]";
    });
}

void windowRemoved(int windowId)
{
    if (g_popup && BrowserWindow::fromHWND(g_popup->owner) && BrowserWindow::fromHWND(g_popup->owner)->id() == windowId)
        closePopup();
    broadcastTabEvent("windows.onRemoved", [&](const Extension&) { return "[" + std::to_string(windowId) + "]"; });
}

void windowFocused(int windowId)
{
    broadcastTabEvent("windows.onFocusChanged", [&](const Extension&) { return "[" + std::to_string(windowId) + "]"; });
}

} // namespace extensions
