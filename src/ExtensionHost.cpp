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
#include "Json.h"
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
    // Extension pages show as chrome-extension:// addresses.
    std::string path;
    if (Extension* owner = internal::extensionForURL(url, &path))
        url = "chrome-extension://" + owner->id + "/" + path;
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

void killContext(int id)
{
    auto found = g_contexts.find(id);
    if (found == g_contexts.end())
        return;
    if (found->second.poll)
        complete(found->second.poll.get(), ok("[]"));
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
    int crashes { 0 };
};

std::map<std::string, std::unique_ptr<HostView>> g_backgrounds;
std::unique_ptr<HostView> g_popup;
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

void closePopup()
{
    if (!g_popup)
        return;
    std::unique_ptr<HostView> popup = std::move(g_popup);
    g_popupClosedAt = GetTickCount64();
    g_popupClosedExt = popup->extId;
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
                if (!host->inDialog)
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

void hostProcessTerminated(WKPageRef, uint32_t, const void* clientInfo)
{
    HostView* host = hostFromClientInfo(clientInfo);
    std::string extId = host->extId;
    if (host->kind == Kind::Popup) {
        PostMessageW(host->hwnd, WM_HOST_CLOSE, 0, 0);
        return;
    }
    // Restart a crashed background page a few times.
    int crashes = host->crashes + 1;
    killContextsOfPage(host->page);
    util::postToMainThread([extId, crashes] {
        auto found = g_backgrounds.find(extId);
        Extension* ext = internal::findEnabled(extId);
        if (found == g_backgrounds.end() || !ext || crashes > 3)
            return;
        WKPageReload(found->second->page);
        found->second->crashes = crashes;
    });
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

void stopBackground(const std::string& extId)
{
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
    if (!host)
        return;
    WKPageLoadURL(host->page, url(util::fromUTF8(baseURL(ext) + page)).get());
    g_backgrounds[ext.id] = std::move(host);
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
    if (property == "icon")
        return ext.actionIcon.empty() ? ext.iconDataURL : baseURL(ext) + ext.actionIcon;
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
        if (!context.poll && context.repollDeadline && now > context.repollDeadline)
            dead.push_back(id);
        else if (context.poll && now - context.pollSince > 20000)
            deliver(context, "{\"type\":\"ping\"}"); // are you still there?
    }
    for (int id : dead)
        killContext(id);

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
    if (op == "runtime.reload") {
        std::string id = ext.id;
        complete(listener, ok());
        util::postToMainThread([id] {
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
            value = internal::resolve(ext, path).empty() ? std::string() : baseURL(ext) + path;
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
    if (!ext || url.rfind(L"file:", 0) != 0)
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
