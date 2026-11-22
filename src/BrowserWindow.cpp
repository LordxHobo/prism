// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "BrowserWindow.h"

#include "Dialogs.h"
#include "Blocker.h"
#include "Bookmarks.h"
#include "Engine.h"
#include "Extensions.h"
#include "History.h"
#include "Http.h"
#include "Icons.h"
#include "Permissions.h"
#include "MenuIcons.h"
#include "Passwords.h"
#include "Privacy.h"
#include "RequestFilter.h"
#include "Shields.h"
#include "ShieldsPanel.h"
#include "Suggestions.h"
#include "UserCSS.h"
#include "Json.h"
#include "Tabs.h"
#include "Settings.h"
#include "Util.h"
#include "Version.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>

using namespace wk;

namespace {

constexpr wchar_t kWindowClass[] = L"PrismBrowserWindow";
constexpr UINT WM_PRISM_CLOSE_TAB = WM_APP + 1;
constexpr UINT_PTR kSessionTimer = 1;
constexpr UINT_PTR kStatsTimer = 2;
// Must match --toolbar-height in ui/toolbar.css (normal and compact).
constexpr int kToolbarCSSHeight = 76;
constexpr int kCompactToolbarCSSHeight = 66;
// Rows that appear under the address bar; must match ui/toolbar.css.
constexpr int kBookmarksBarCSSHeight = 30;
constexpr int kInfobarCSSHeight = 40;
constexpr int kFindbarCSSHeight = 38;
// The tab strip's part of the toolbar, and the second row of the
// Vivaldi-style strip (the selected group's tabs).
constexpr int kTabStripCSSHeight = 36;
constexpr int kCompactTabStripCSSHeight = 30;
constexpr int kSubTabsCSSHeight = 32;
constexpr int kCompactSubTabsCSSHeight = 28;
// Vertical tabs: the sidebar's width, open and narrowed to icons.
constexpr int kSidebarCSSWidth = 240;
constexpr int kCollapsedSidebarCSSWidth = 48;
// Chrome's tab group colors.
const wchar_t* const kGroupColors[] = { L"#8ab4f8", L"#f28b82", L"#fdd663", L"#81c995", L"#ff8bcb", L"#c58af9", L"#78d9ec", L"#fcad70", L"#9aa0a6" };
const wchar_t* const kGroupColorNames[] = { L"Blue", L"Red", L"Yellow", L"Green", L"Pink", L"Purple", L"Cyan", L"Orange", L"Grey" };
unsigned long long s_activationCounter = 0;
int s_nextGroupId = 0;
bool s_sessionDirty = false;

std::vector<BrowserWindow*> s_windows;
int s_nextTabId = 0;
int s_nextWindowId = 0;
int s_lastFocusedWindowId = -1;

Tab* tabFromClientInfo(const void* clientInfo)
{
    return const_cast<Tab*>(static_cast<const Tab*>(clientInfo));
}

// ---- Extensions' right-click menu items -------------------------------------------
// WebKit asks for the menu before showing it (getContextMenuFromProposedMenuAsync).
// When an extension has items, Prism first asks the page for the selected
// text (contextMenus "selection" items show it), then adds the items after
// WebKit's own.

struct PendingMenu {
    Ref<WKContextMenuListenerRef> listener;
    Ref<WKArrayRef> proposed;
    extensions::MenuContext context;
    int tabId { -1 };
};

struct MenuTag {
    std::string extensionId;
    std::string itemId;
};
std::vector<MenuTag> g_menuTags;              // tag - kWKContextMenuItemBaseApplicationTag -> item
extensions::MenuContext g_menuContext;         // of the menu on screen
int g_menuTabId = -1;

std::string urlText(WKURLRef url)
{
    if (!url)
        return { };
    std::string text = util::toUTF8(toWide(url));
    WKRelease(url);
    return text;
}

void appendMenuNodes(WKMutableArrayRef array, const std::vector<extensions::MenuNode>& nodes)
{
    for (auto& node : nodes) {
        WKContextMenuItemRef item = nullptr;
        auto title = stringUTF8(node.title);
        if (node.type == "separator") {
            item = WKContextMenuItemSeparatorItem();
        } else if (!node.children.empty() || node.itemId.empty()) {
            WKMutableArrayRef children = WKMutableArrayCreate();
            appendMenuNodes(children, node.children);
            item = WKContextMenuItemCreateAsSubmenu(title.get(), node.enabled, children);
            WKRelease(children);
        } else {
            WKContextMenuItemTag tag = kWKContextMenuItemBaseApplicationTag + static_cast<WKContextMenuItemTag>(g_menuTags.size());
            g_menuTags.push_back({ node.extensionId, node.itemId });
            if (node.type == "checkbox" || node.type == "radio")
                item = WKContextMenuItemCreateAsCheckableAction(tag, title.get(), node.enabled, node.checked);
            else
                item = WKContextMenuItemCreateAsAction(tag, title.get(), node.enabled);
        }
        if (item) {
            WKArrayAppendItem(array, item);
            WKRelease(item);
        }
    }
}

void finishMenu(PendingMenu& pending)
{
    WKMutableArrayRef items = WKMutableArrayCreate();
    size_t count = WKArrayGetSize(pending.proposed.get());
    for (size_t i = 0; i < count; ++i)
        WKArrayAppendItem(items, WKArrayGetItemAtIndex(pending.proposed.get(), i));
    std::vector<extensions::MenuNode> nodes = extensions::pageMenu(pending.context);
    g_menuTags.clear();
    if (!nodes.empty()) {
        if (count) {
            WKContextMenuItemRef separator = WKContextMenuItemSeparatorItem();
            WKArrayAppendItem(items, separator);
            WKRelease(separator);
        }
        appendMenuNodes(items, nodes);
    }
    g_menuContext = pending.context;
    g_menuTabId = pending.tabId;
    WKContextMenuListenerUseContextMenuItems(pending.listener.get(), items);
    WKRelease(items);
}

void selectionForMenu(WKTypeRef result, WKErrorRef, void* context)
{
    std::unique_ptr<PendingMenu> pending(static_cast<PendingMenu*>(context));
    if (result && WKGetTypeID(result) == WKStringGetTypeID()) {
        std::string text = util::toUTF8(toWide(static_cast<WKStringRef>(result)));
        if (text.size() > 10000)
            text.resize(10000);
        pending->context.selectionText = text;
    }
    finishMenu(*pending);
}

bool isAudioAddress(const std::string& url)
{
    std::string lower = url.substr(0, url.find_first_of("?#"));
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* ext : { ".mp3", ".m4a", ".aac", ".ogg", ".oga", ".opus", ".wav", ".flac" }) {
        size_t length = std::strlen(ext);
        if (lower.size() > length && lower.compare(lower.size() - length, length, ext) == 0)
            return true;
    }
    return false;
}

void getContextMenuAsync(WKPageRef page, WKArrayRef proposed, WKContextMenuListenerRef listener, WKHitTestResultRef hit, WKTypeRef, const void* clientInfo)
{
    Tab* tab = tabFromClientInfo(clientInfo);
    if (!tab || tab->extensionPages || !extensions::hasMenuItems()) {
        WKContextMenuListenerUseContextMenuItems(listener, proposed);
        return;
    }
    auto pending = std::make_unique<PendingMenu>();
    pending->listener = Ref<WKContextMenuListenerRef>::retain(listener);
    pending->proposed = Ref<WKArrayRef>::retain(proposed);
    pending->tabId = tab->id;
    extensions::MenuContext& context = pending->context;
    context.pageURL = util::toUTF8(tab->url);
    if (hit) {
        context.linkURL = urlText(WKHitTestResultCopyAbsoluteLinkURL(hit));
        if (!context.linkURL.empty()) {
            if (WKStringRef label = WKHitTestResultCopyLinkLabel(hit)) {
                context.linkText = util::toUTF8(toWide(label));
                WKRelease(label);
            }
        }
        std::string image = urlText(WKHitTestResultCopyAbsoluteImageURL(hit));
        std::string media = urlText(WKHitTestResultCopyAbsoluteMediaURL(hit));
        if (!image.empty()) {
            context.srcURL = image;
            context.mediaType = "image";
        } else if (!media.empty()) {
            context.srcURL = media;
            context.mediaType = isAudioAddress(media) ? "audio" : "video";
        }
        context.editable = WKHitTestResultIsContentEditable(hit);
    }
    WKPageEvaluateJavaScriptInMainFrame(page, stringUTF8("String(window.getSelection ? window.getSelection() : '')").get(),
        pending.release(), selectionForMenu);
}

void contextMenuItemSelected(WKPageRef, WKContextMenuItemRef item, const void*)
{
    WKContextMenuItemTag tag = WKContextMenuItemGetTag(item);
    if (tag < kWKContextMenuItemBaseApplicationTag)
        return;
    size_t index = tag - kWKContextMenuItemBaseApplicationTag;
    if (index < g_menuTags.size())
        extensions::menuItemClicked(g_menuTags[index].extensionId, g_menuTags[index].itemId, g_menuContext, g_menuTabId);
}

BrowserWindow* windowFromClientInfo(const void* clientInfo)
{
    return const_cast<BrowserWindow*>(static_cast<const BrowserWindow*>(clientInfo));
}

std::wstring lower(std::wstring value)
{
    for (auto& c : value)
        c = static_cast<wchar_t>(towlower(c));
    return value;
}

std::wstring schemeOf(const std::wstring& url)
{
    size_t colon = url.find(L':');
    if (colon == std::wstring::npos)
        return { };
    return lower(url.substr(0, colon));
}

std::wstring hostOf(const std::wstring& url)
{
    size_t start = url.find(L"://");
    if (start == std::wstring::npos)
        return url;
    start += 3;
    size_t end = url.find_first_of(L"/?#", start);
    return url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
}

bool systemUsesDarkTheme()
{
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
        return value == 0;
    return false;
}

// Internal prism:// pages that may be opened from the address bar.
bool isKnownInternalPage(const std::wstring& name)
{
    return name == L"newtab" || name == L"whatsnew" || name == L"settings" || name == L"extensions"
        || name == L"history" || name == L"bookmarks" || name == L"passwords";
}

struct DownloadInfo {
    HWND window { };
    std::wstring path;
};

void downloadToast(HWND window, const std::wstring& text)
{
    if (auto* browser = BrowserWindow::fromHWND(window))
        browser->showToast(text);
}

std::wstring fileNameOf(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

WKStringRef downloadDecideDestination(WKDownloadRef, WKURLResponseRef response, WKStringRef suggestedFilename, const void* clientInfo)
{
    auto* info = static_cast<DownloadInfo*>(const_cast<void*>(clientInfo));
    std::wstring name = toWide(suggestedFilename);
    if (name.empty() && response) {
        auto responseURL = Ref<WKURLRef>::adopt(WKURLResponseCopyURL(response));
        std::wstring urlText = toWide(responseURL.get());
        size_t end = urlText.find_first_of(L"?#");
        urlText = urlText.substr(0, end);
        name = fileNameOf(urlText);
    }
    info->path = util::uniqueFilePath(settings::downloadDirectory(), name);
    downloadToast(info->window, L"Downloading " + fileNameOf(info->path) + L"…");
    // WebKit takes ownership of the returned string.
    return WKStringCreateWithUTF8CString(util::toUTF8(info->path).c_str());
}

void downloadDidFinish(WKDownloadRef, const void* clientInfo)
{
    auto* info = static_cast<DownloadInfo*>(const_cast<void*>(clientInfo));
    downloadToast(info->window, L"Downloaded " + fileNameOf(info->path) + L" — click to open your Downloads folder");
    delete info;
}

void downloadDidFail(WKDownloadRef, WKErrorRef error, WKDataRef, const void* clientInfo)
{
    auto* info = static_cast<DownloadInfo*>(const_cast<void*>(clientInfo));
    std::wstring reason = error ? toWide(Ref<WKStringRef>::adopt(WKErrorCopyLocalizedDescription(error)).get()) : L"";
    downloadToast(info->window, L"Download failed: " + fileNameOf(info->path) + (reason.empty() ? L"" : L" (" + reason + L")"));
    delete info;
}

} // namespace

// ---------------------------------------------------------------------------
// Window setup

bool BrowserWindow::registerWindowClass(HINSTANCE instance)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = util::appIcon(false);
    wc.hIconSm = util::appIcon(true);
    wc.lpszClassName = kWindowClass;
    return RegisterClassExW(&wc) != 0;
}

BrowserWindow* BrowserWindow::open(const std::vector<std::wstring>& startURLs, int activeIndex)
{
    return open(startURLs, activeIndex, { }, { });
}

BrowserWindow* BrowserWindow::open(const std::vector<std::wstring>& startURLs, int activeIndex,
    const std::vector<TabGroup>& groups, const std::vector<int>& tabGroups)
{
    auto* window = new BrowserWindow();
    window->m_pendingStartURLs = startURLs;
    window->m_pendingActiveIndex = activeIndex;
    window->m_pendingGroups = groups;
    window->m_pendingTabGroups = tabGroups;
    if (window->m_pendingStartURLs.empty())
        window->m_pendingStartURLs.push_back(L"prism://newtab");

    UINT dpi = GetDpiForSystem();
    int width = MulDiv(1280, dpi, 96), height = MulDiv(820, dpi, 96);
    HWND hwnd = CreateWindowExW(0, kWindowClass, L"Prism", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), window);
    if (!hwnd)
        return nullptr; // windowProc deleted `window` in WM_NCDESTROY.
    // Like the last window that was closed: maximized (also the first time),
    // or at its size and place.
    std::wstring saved = util::readSetting(L"WindowPlacement", L"");
    int maximized = 1, left = 0, top = 0, right = 0, bottom = 0;
    if (!saved.empty())
        swscanf_s(saved.c_str(), L"%d,%d,%d,%d,%d", &maximized, &left, &top, &right, &bottom);
    RECT normal = { left, top, right, bottom };
    static bool s_placedFirst = false;
    if (!s_placedFirst && right - left >= 200 && bottom - top >= 150 && MonitorFromRect(&normal, MONITOR_DEFAULTTONULL)) {
        WINDOWPLACEMENT placement = { sizeof(placement) };
        GetWindowPlacement(hwnd, &placement);
        placement.rcNormalPosition = normal;
        placement.showCmd = SW_HIDE;
        SetWindowPlacement(hwnd, &placement);
    }
    s_placedFirst = true;
    ShowWindow(hwnd, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    return window;
}

void BrowserWindow::savePlacement()
{
    WINDOWPLACEMENT placement = { sizeof(placement) };
    if (!GetWindowPlacement(m_hwnd, &placement))
        return;
    bool maximized = placement.showCmd == SW_SHOWMAXIMIZED || (placement.showCmd == SW_SHOWMINIMIZED && (placement.flags & WPF_RESTORETOMAXIMIZED));
    const RECT& r = placement.rcNormalPosition;
    util::writeSetting(L"WindowPlacement", std::to_wstring(maximized ? 1 : 0) + L"," + std::to_wstring(r.left) + L"," + std::to_wstring(r.top)
        + L"," + std::to_wstring(r.right) + L"," + std::to_wstring(r.bottom));
}

BrowserWindow* BrowserWindow::fromHWND(HWND hwnd)
{
    if (!hwnd)
        return nullptr;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    for (auto* window : s_windows) {
        if (window->m_hwnd == root)
            return window;
    }
    return nullptr;
}

size_t BrowserWindow::windowCount()
{
    return s_windows.size();
}

LRESULT CALLBACK BrowserWindow::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    BrowserWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        window = static_cast<BrowserWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        window->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    } else
        window = reinterpret_cast<BrowserWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (!window)
        return DefWindowProcW(hwnd, message, wParam, lParam);

    if (message == WM_NCDESTROY) {
        // Child windows (the WebKit views) are gone by now, so it's safe to
        // release our references to them.
        LRESULT result = DefWindowProcW(hwnd, message, wParam, lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete window;
        return result;
    }
    return window->handleMessage(message, wParam, lParam);
}

LRESULT BrowserWindow::handleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: {
        m_id = ++s_nextWindowId;
        s_lastFocusedWindowId = m_id;
        s_windows.push_back(this);
        extensions::windowCreated(m_id);
        updateTheme();
        updateIcon();
        createToolbar();
        auto urls = std::move(m_pendingStartURLs);
        int active = m_pendingActiveIndex >= 0 && m_pendingActiveIndex < static_cast<int>(urls.size())
            ? m_pendingActiveIndex : static_cast<int>(urls.size()) - 1;
        // Saved tab groups get new ids in this window.
        std::vector<int> groupIds;
        for (auto& group : m_pendingGroups) {
            TabGroup copy = group;
            copy.id = ++s_nextGroupId;
            m_groups.push_back(copy);
            groupIds.push_back(copy.id);
        }
        Tab* selected = nullptr;
        for (size_t i = 0; i < urls.size(); ++i) {
            Tab* tab = newTab(urls[i], false);
            if (i < m_pendingTabGroups.size() && m_pendingTabGroups[i] > 0 && m_pendingTabGroups[i] <= static_cast<int>(groupIds.size()))
                tab->groupId = groupIds[static_cast<size_t>(m_pendingTabGroups[i] - 1)];
            if (static_cast<int>(i) == active)
                selected = tab;
        }
        m_pendingGroups.clear();
        m_pendingTabGroups.clear();
        normalizeGroups();
        if (settings::verticalTabs())
            createSidebar();
        if (selected)
            selectTab(selected->id);
        return 0;
    }
    case WM_SIZE:
        suggestions::hide();
        layout();
        return 0;
    case WM_MOVE:
        suggestions::hide();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        UINT dpi = GetDpiForWindow(m_hwnd);
        info->ptMinTrackSize.x = MulDiv(480, dpi, 96);
        info->ptMinTrackSize.y = MulDiv(320, dpi, 96);
        return 0;
    }
    case WM_DPICHANGED: {
        auto* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(m_hwnd, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        applyScaleFactor();
        layout();
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rect;
        GetClientRect(m_hwnd, &rect);
        HBRUSH brush = CreateSolidBrush(m_darkTheme ? RGB(28, 28, 32) : RGB(246, 246, 248));
        FillRect(reinterpret_cast<HDC>(wParam), &rect, brush);
        DeleteObject(brush);
        return 1;
    }
    case WM_SETTINGCHANGE:
        if (lParam && !lstrcmpW(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet")) {
            updateTheme();
            pushState();
            // The theme's accent may have changed with it.
            evaluateInInternalPages(L"settings", "window.prismSettingsChanged && prismSettingsChanged();");
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
            suggestions::hide();
        if (LOWORD(wParam) != WA_INACTIVE) {
            focusContent();
            if (s_lastFocusedWindowId != m_id) {
                s_lastFocusedWindowId = m_id;
                extensions::windowFocused(m_id);
            }
        }
        return 0;
    case WM_APPCOMMAND:
        switch (GET_APPCOMMAND_LPARAM(lParam)) {
        case APPCOMMAND_BROWSER_BACKWARD:
            if (auto* tab = activeTab()) WKPageGoBack(tab->page);
            return TRUE;
        case APPCOMMAND_BROWSER_FORWARD:
            if (auto* tab = activeTab()) WKPageGoForward(tab->page);
            return TRUE;
        case APPCOMMAND_BROWSER_REFRESH:
            if (auto* tab = activeTab()) WKPageReload(tab->page);
            return TRUE;
        case APPCOMMAND_BROWSER_STOP:
            if (auto* tab = activeTab()) WKPageStopLoading(tab->page);
            return TRUE;
        case APPCOMMAND_BROWSER_HOME:
            if (auto* tab = activeTab()) navigate(tab, L"prism://newtab");
            return TRUE;
        }
        break;
    case WM_TIMER:
        if (wParam == kSessionTimer) {
            KillTimer(m_hwnd, kSessionTimer);
            saveSession();
            return 0;
        }
        if (wParam == kStatsTimer) {
            // Keep the shield's "blocked" count fresh.
            uint64_t blocked = blocker::blockedCount();
            Tab* active = activeTab();
            uint32_t here = active ? requestfilter::blockedOnPage(active->page) : 0;
            if (blocked != m_lastBlockedCount || here != m_lastBlockedHere) {
                m_lastBlockedCount = blocked;
                m_lastBlockedHere = here;
                runToolbarScript("window.prism && prism.stats(" + std::to_string(blocked) + "," + std::to_string(here) + ");");
            }
            return 0;
        }
        break;
    case WM_PRISM_CLOSE_TAB:
        closeTab(static_cast<int>(wParam));
        return 0;
    case WM_CLOSE:
        DestroyWindow(m_hwnd);
        return 0;
    case WM_DESTROY:
        savePlacement();
        onDestroy();
        return 0;
    }
    return DefWindowProcW(m_hwnd, message, wParam, lParam);
}

// Stops WebKit from calling back into a tab or window we're about to free.
static void detachClients(WKPageRef page)
{
    if (!page)
        return;
    WKPageUIClientV6 uiClient = { };
    uiClient.base.version = 6;
    WKPageSetPageUIClient(page, &uiClient.base);
    WKPageNavigationClientV3 navigationClient = { };
    navigationClient.base.version = 3;
    WKPageSetPageNavigationClient(page, &navigationClient.base);
    WKPageSetPageStateClient(page, nullptr);
    if (WKPageSetPageFindClient) {
        WKPageFindClientV0 findClient = { };
        WKPageSetPageFindClient(page, &findClient.base);
    }
}

void BrowserWindow::onDestroy()
{
    suggestions::hide();
    m_toolbarReady = false;
    for (auto& tab : m_tabs)
        denyPrompts(tab.get());
    detachClients(m_toolbarPage);
    detachClients(m_sidebarPage);
    m_sidebarReady = false;
    for (auto& tab : m_tabs)
        detachClients(tab->page);
    m_toolbarReady = false;
    KillTimer(m_hwnd, kSessionTimer);
    KillTimer(m_hwnd, kStatsTimer);
    for (auto& tab : m_tabs)
        extensions::tabRemoved(tab->id, m_id, true);
    extensions::windowRemoved(m_id);

    // Closing the last window quits Prism: remember its tabs for next time.
    // Closing one of several windows forgets that window's tabs.
    bool last = s_windows.size() == 1;
    if (last)
        saveSession();
    s_windows.erase(std::remove(s_windows.begin(), s_windows.end(), this), s_windows.end());
    if (last)
        PostQuitMessage(0);
    else
        saveSession();
}

double BrowserWindow::scaleFactor() const
{
    double scale = GetDpiForWindow(m_hwnd) / 96.0;
    // Like WebKit's own MiniBrowser, round to whole numbers when we can: WebKit's
    // Windows port draws fractional scales (125%, 150%) less cleanly.
    if (WKPageSetCustomBackingScaleFactor)
        scale = std::max(1.0, std::round(scale));
    return scale;
}

int BrowserWindow::toolbarHeight() const
{
    return static_cast<int>(std::lround(toolbarCSSHeight() * scaleFactor()));
}

int BrowserWindow::toolbarCSSHeight() const
{
    int height = settings::compactTabs() ? kCompactToolbarCSSHeight : kToolbarCSSHeight;
    // Vertical tabs: no tab strip on top. Vivaldi-style groups: a second row.
    if (settings::verticalTabs())
        height -= settings::compactTabs() ? kCompactTabStripCSSHeight : kTabStripCSSHeight;
    else if (settings::groupStyle() == L"vivaldi")
        height += settings::compactTabs() ? kCompactSubTabsCSSHeight : kSubTabsCSSHeight;
    if (settings::showBookmarksBar())
        height += kBookmarksBarCSSHeight;
    if (Tab* tab = activeTab()) {
        passwords::Offer offer;
        if (!tab->prompts.empty() || passwords::offerFor(tab->id, offer))
            height += kInfobarCSSHeight;
        if (tab->findOpen)
            height += kFindbarCSSHeight;
    }
    return height;
}

void BrowserWindow::applyScaleFactor()
{
    double scale = scaleFactor();
    if (!WKPageSetCustomBackingScaleFactor)
        return;
    if (m_toolbarPage)
        WKPageSetCustomBackingScaleFactor(m_toolbarPage, scale);
    if (m_sidebarPage)
        WKPageSetCustomBackingScaleFactor(m_sidebarPage, scale);
    for (auto& tab : m_tabs)
        WKPageSetCustomBackingScaleFactor(tab->page, scale);
}

void BrowserWindow::updateIcon()
{
    SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(util::appIcon(false)));
    SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(util::appIcon(true)));
}

void BrowserWindow::updateTheme()
{
    std::wstring setting = settings::theme();
    if (setting == L"dark")
        m_darkTheme = true;
    else if (setting == L"light")
        m_darkTheme = false;
    else
        m_darkTheme = systemUsesDarkTheme();
    engine::setDarkTheme(m_darkTheme);

    BOOL dark = m_darkTheme ? TRUE : FALSE;
    constexpr DWORD kUseImmersiveDarkMode = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE
    DwmSetWindowAttribute(m_hwnd, kUseImmersiveDarkMode, &dark, sizeof(dark));
    InvalidateRect(m_hwnd, nullptr, TRUE);
}

void BrowserWindow::layout()
{
    RECT client;
    GetClientRect(m_hwnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    int toolbar = std::min(toolbarHeight(), height);

    int side = std::min(sidebarWidth(), width / 2);

    if (m_toolbarHwnd)
        MoveWindow(m_toolbarHwnd, 0, 0, width, toolbar, TRUE);
    if (m_sidebarHwnd)
        MoveWindow(m_sidebarHwnd, 0, toolbar, side, std::max(0, height - toolbar), TRUE);
    if (auto* tab = activeTab())
        MoveWindow(tab->hwnd, side, toolbar, std::max(0, width - side), std::max(0, height - toolbar), TRUE);
}

// ---------------------------------------------------------------------------
// Toolbar (the browser's own interface, drawn by WebKit from ui/toolbar.html)

void BrowserWindow::createToolbar()
{
    RECT rect = { };
    m_toolbarView = Ref<WKViewRef>::adopt(WKViewCreate(rect, engine::pageConfiguration(), m_hwnd));
    WKViewSetIsInWindow(m_toolbarView.get(), true);
    m_toolbarHwnd = WKViewGetWindow(m_toolbarView.get());
    m_toolbarPage = WKViewGetPage(m_toolbarView.get());
    engine::configurePage(m_toolbarPage, scaleFactor());

    WKPageUIClientV6 uiClient = { };
    uiClient.base.version = 6;
    uiClient.base.clientInfo = this;
    uiClient.runJavaScriptPrompt = toolbarPrompt;
    WKPageSetPageUIClient(m_toolbarPage, &uiClient.base);

    WKPageNavigationClientV3 navigationClient = { };
    navigationClient.base.version = 3;
    navigationClient.base.clientInfo = this;
    navigationClient.decidePolicyForNavigationAction = toolbarDecidePolicyForNavigationAction;
    WKPageSetPageNavigationClient(m_toolbarPage, &navigationClient.base);

    engine::loadInternalPage(m_toolbarPage, L"toolbar");
    layout();
    SetTimer(m_hwnd, kStatsTimer, 2000, nullptr);
}

// Vertical tabs: a second view of the toolbar page down the left side
// (toolbar.html?layout=sidebar), getting the same state and sending the same
// commands as the toolbar.
void BrowserWindow::createSidebar()
{
    if (m_sidebarHwnd)
        return;
    RECT rect = { };
    m_sidebarView = Ref<WKViewRef>::adopt(WKViewCreate(rect, engine::pageConfiguration(), m_hwnd));
    WKViewSetIsInWindow(m_sidebarView.get(), true);
    m_sidebarHwnd = WKViewGetWindow(m_sidebarView.get());
    m_sidebarPage = WKViewGetPage(m_sidebarView.get());
    engine::configurePage(m_sidebarPage, scaleFactor());

    WKPageUIClientV6 uiClient = { };
    uiClient.base.version = 6;
    uiClient.base.clientInfo = this;
    uiClient.runJavaScriptPrompt = toolbarPrompt;
    WKPageSetPageUIClient(m_sidebarPage, &uiClient.base);

    WKPageNavigationClientV3 navigationClient = { };
    navigationClient.base.version = 3;
    navigationClient.base.clientInfo = this;
    navigationClient.decidePolicyForNavigationAction = toolbarDecidePolicyForNavigationAction;
    WKPageSetPageNavigationClient(m_sidebarPage, &navigationClient.base);

    engine::loadInternalPage(m_sidebarPage, L"toolbar", L"layout=sidebar");
    layout();
}

void BrowserWindow::destroySidebar()
{
    if (!m_sidebarHwnd)
        return;
    detachClients(m_sidebarPage);
    m_sidebarReady = false;
    HWND hwnd = m_sidebarHwnd;
    m_sidebarHwnd = nullptr;
    m_sidebarPage = nullptr;
    m_sidebarView.reset();
    DestroyWindow(hwnd);
    layout();
}

int BrowserWindow::sidebarWidth() const
{
    if (!m_sidebarHwnd)
        return 0;
    int css = settings::verticalTabsCollapsed() ? kCollapsedSidebarCSSWidth : kSidebarCSSWidth;
    return static_cast<int>(std::lround(css * scaleFactor()));
}

void BrowserWindow::toolbarDecidePolicyForNavigationAction(WKPageRef, WKNavigationActionRef action, WKFramePolicyListenerRef listener, WKTypeRef, const void*)
{
    // The toolbar may only ever show toolbar.html.
    auto request = Ref<WKURLRequestRef>::adopt(WKNavigationActionCopyRequest(action));
    auto requestURL = Ref<WKURLRef>::adopt(WKURLRequestCopyURL(request.get()));
    std::wstring target = toWide(requestURL.get());
    if (engine::isInternalPage(target, L"toolbar"))
        WKFramePolicyListenerUse(listener);
    else
        WKFramePolicyListenerIgnore(listener);
}

// The toolbar talks to Prism through window.prompt("prism:<command>\n<argument>").
// It's synchronous, needs no extra WebKit plumbing, and only the toolbar's own
// page is listened to, so websites can't send Prism commands.
void BrowserWindow::toolbarPrompt(WKPageRef, WKStringRef message, WKStringRef, WKFrameRef, WKSecurityOriginRef, WKPageRunJavaScriptPromptResultListenerRef listener, const void* clientInfo)
{
    auto* window = windowFromClientInfo(clientInfo);
    std::wstring text = toWide(message);
    // Suggestions are answered straight away (the toolbar needs the list);
    // everything else answers first, so menus and dialogs opened by a
    // command don't keep the toolbar waiting.
    if (text.rfind(L"prism:suggest\n", 0) == 0) {
        std::string answer = window->suggest(text.substr(14));
        WKPageRunJavaScriptPromptResultListenerCall(listener, stringUTF8(answer).get());
        return;
    }
    WKPageRunJavaScriptPromptResultListenerCall(listener, nullptr);

    if (text.rfind(L"prism:", 0) != 0)
        return;
    text = text.substr(6);
    size_t newline = text.find(L'\n');
    std::wstring command = text.substr(0, newline);
    std::wstring argument = newline == std::wstring::npos ? L"" : text.substr(newline + 1);
    window->handleToolbarCommand(command, argument);
}

void BrowserWindow::handleToolbarCommand(const std::wstring& command, const std::wstring& argument)
{
    Tab* tab = activeTab();
    if (command == L"ready" || command == L"sidebarReady") {
        if (command == L"ready")
            m_toolbarReady = true;
        else
            m_sidebarReady = true;
        runToolbarScript("window.prism && prism.setUserCSS(" + json::quote(usercss::interfaceCSS()) + ");");
        pushState();
        if (GetForegroundWindow() == m_hwnd)
            focusContent();
    } else if (command == L"navigate") {
        if (tab && !argument.empty()) {
            navigate(tab, argument);
            focusContent();
        }
    } else if (command == L"back") {
        if (tab) WKPageGoBack(tab->page);
    } else if (command == L"forward") {
        if (tab) WKPageGoForward(tab->page);
    } else if (command == L"reload") {
        if (tab) WKPageReload(tab->page);
    } else if (command == L"stop") {
        if (tab) WKPageStopLoading(tab->page);
    } else if (command == L"newtab") {
        newTab(L"prism://newtab", true);
    } else if (command == L"select") {
        selectTab(_wtoi(argument.c_str()));
    } else if (command == L"close") {
        closeTab(_wtoi(argument.c_str()));
    } else if (command == L"menu") {
        int x = 0, y = 0;
        swscanf_s(argument.c_str(), L"%d,%d", &x, &y);
        showMenu(x, y);
    } else if (command == L"openDownloads") {
        ShellExecuteW(m_hwnd, L"open", settings::downloadDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (command == L"settings") {
        openInternalPage(L"settings");
    } else if (command == L"extensions") {
        openInternalPage(L"extensions");
    } else if (command == L"extensionsMenu") {
        showExtensionsMenu(toolbarPoint(argument));
    } else if (command == L"home") {
        if (tab) {
            navigate(tab, L"prism://newtab");
            focusContent();
        }
    } else if (command == L"moveTab") {
        // "<tab id>\n<index>\n<group id>" (index in the whole tab list)
        int id = 0, index = 0, group = 0;
        swscanf_s(argument.c_str(), L"%d\n%d\n%d", &id, &index, &group);
        moveTabTo(id, index, group);
    } else if (command == L"moveUnit") {
        // "<unit>\n<index among units>"
        int unit = 0, index = 0;
        swscanf_s(argument.c_str(), L"%d\n%d", &unit, &index);
        moveUnit(unit, index);
    } else if (command == L"selectUnit") {
        selectUnit(_wtoi(argument.c_str()), false);
    } else if (command == L"tabMenu" || command == L"groupMenu") {
        size_t newline = argument.find(L'\n');
        int id = _wtoi(argument.substr(0, newline).c_str());
        POINT point = toolbarPoint(newline == std::wstring::npos ? L"" : argument.substr(newline + 1));
        if (command == L"tabMenu")
            showTabMenu(id, point);
        else
            showGroupMenu(id, point);
    } else if (command == L"groupToggle") {
        if (TabGroup* group = findGroup(_wtoi(argument.c_str())))
            setGroupCollapsed(group->id, !group->collapsed);
    } else if (command == L"newTabInGroup") {
        int groupId = _wtoi(argument.c_str());
        if (findGroup(groupId)) {
            Tab* fresh = newTab(L"prism://newtab", true);
            addTabToGroup(fresh, groupId);
        }
    } else if (command == L"sidebarToggle") {
        settings::set(L"verticalTabsCollapsed", settings::verticalTabsCollapsed() ? L"0" : L"1");
        layout();
        pushState();
    } else if (command == L"addExtension") {
        // "Add to Prism" on a store page: open the Extensions page, which
        // picks the link up and starts the install.
        if (tab && extensions::isStoreURL(tab->url)) {
            extensions::queueInstall(tab->url);
            openInternalPage(L"extensions");
            evaluateInInternalPages(L"extensions", "window.prismExtensionsEvent && prismExtensionsEvent({\"type\":\"checkPending\"});");
        }
    } else if (command == L"shields") {
        if (!tab)
            return;
        int x = 0, y = 0;
        swscanf_s(argument.c_str(), L"%d,%d", &x, &y);
        double scale = scaleFactor();
        POINT anchor = { static_cast<LONG>(x * scale), static_cast<LONG>(y * scale) };
        ClientToScreen(m_toolbarHwnd, &anchor);
        HWND hwnd = m_hwnd;
        int tabId = tab->id;
        shieldspanel::Callbacks callbacks;
        callbacks.reloadTab = [hwnd, tabId] {
            auto* window = BrowserWindow::fromHWND(hwnd);
            if (!window)
                return;
            for (auto& candidate : window->m_tabs) {
                if (candidate->id == tabId)
                    WKPageReload(candidate->page);
            }
            window->pushState();
        };
        callbacks.openSettings = [hwnd] {
            if (auto* window = BrowserWindow::fromHWND(hwnd))
                window->openInternalPage(L"settings", L"shields");
        };
        callbacks.pageStats = [hwnd, tabId]() -> std::string {
            auto* window = BrowserWindow::fromHWND(hwnd);
            if (!window)
                return "null";
            for (auto& candidate : window->m_tabs) {
                if (candidate->id != tabId)
                    continue;
                std::string json = "{\"filtering\":" + std::string(requestfilter::bundleActive() ? "true" : "false");
                json += ",\"blocked\":" + std::to_string(requestfilter::blockedOnPage(candidate->page)) + ",\"hosts\":[";
                bool first = true;
                for (auto& [host, count] : requestfilter::blockedHostsOnPage(candidate->page, 12)) {
                    json += std::string(first ? "" : ",") + "{\"domain\":" + json::quote(host) + ",\"count\":" + std::to_string(count) + "}";
                    first = false;
                }
                return json + "]}";
            }
            return "null";
        };
        shieldspanel::toggle(m_hwnd, anchor, scale, tab->url, std::move(callbacks));
    } else if (command == L"extensionAction" || command == L"extensionMenu") {
        // "<extension id>\n<x>,<y>" (the button's bottom right, CSS pixels)
        size_t newline = argument.find(L'\n');
        std::string id = util::toUTF8(argument.substr(0, newline));
        int x = 0, y = 0;
        if (newline != std::wstring::npos)
            swscanf_s(argument.c_str() + newline + 1, L"%d,%d", &x, &y);
        if (command == L"extensionAction")
            extensions::actionClicked(m_hwnd, tab ? tab->id : -1, id, x, y);
        else
            showHiddenActionMenu(id, x, y);
    } else if (command == L"focusContent") {
        focusContent();
    } else if (command == L"suggestSelect") {
        suggestions::select(_wtoi(argument.c_str()));
    } else if (command == L"suggestClose") {
        suggestions::hide();
    } else if (command == L"find") {
        if (tab) {
            tab->findQuery = argument;
            findInPage(tab, 0);
        }
    } else if (command == L"findNext" || command == L"findPrevious") {
        if (tab)
            findInPage(tab, command == L"findNext" ? 1 : -1);
    } else if (command == L"findClose") {
        if (tab)
            closeFind(tab);
        focusContent();
    } else if (command == L"permission") {
        if (tab)
            answerPrompt(tab, argument);
    } else if (command == L"bookmarkButton") {
        bookmarkButton(argument);
    } else if (command == L"bookmarkOpen") {
        // "<id>\n<current|foreground|background>"
        size_t newline = argument.find(L'\n');
        const bookmarks::Node* node = bookmarks::find(_wtoi64(argument.substr(0, newline).c_str()));
        if (node && !node->folder)
            openBookmarkURL(node->url, newline == std::wstring::npos ? L"current" : argument.substr(newline + 1));
    } else if (command == L"bookmarkFolder" || command == L"bookmarkMenu") {
        // "<id>\n<x>,<y>"
        size_t newline = argument.find(L'\n');
        long long id = _wtoi64(argument.substr(0, newline).c_str());
        POINT point = toolbarPoint(newline == std::wstring::npos ? L"" : argument.substr(newline + 1));
        if (command == L"bookmarkFolder")
            showBookmarkFolder(id, point);
        else
            showBookmarkMenu(id, point);
    } else if (command == L"bookmarkOverflow") {
        // "<x>,<y>\n<id>,<id>,..."
        size_t newline = argument.find(L'\n');
        std::vector<long long> ids;
        std::wstring list = newline == std::wstring::npos ? L"" : argument.substr(newline + 1);
        for (size_t start = 0; start < list.size();) {
            size_t comma = list.find(L',', start);
            ids.push_back(_wtoi64(list.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start).c_str()));
            if (comma == std::wstring::npos)
                break;
            start = comma + 1;
        }
        showBookmarkOverflow(ids, toolbarPoint(argument.substr(0, newline)));
    } else if (command == L"bookmarksBarMenu") {
        POINT point = toolbarPoint(argument);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"Hide bookmarks bar\tCtrl+Shift+B");
        AppendMenuW(menu, MF_STRING, 2, L"Manage bookmarks\tCtrl+Shift+O");
        int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, 0, m_hwnd, nullptr);
        DestroyMenu(menu);
        if (choice == 1)
            toggleBookmarksBar();
        else if (choice == 2)
            openInternalPage(L"bookmarks");
    } else if (command == L"history") {
        openInternalPage(L"history");
    } else if (command == L"bookmarks") {
        openInternalPage(L"bookmarks");
    }
}

void BrowserWindow::runToolbarScript(const std::string& script)
{
    if (m_toolbarPage && m_toolbarReady)
        WKPageEvaluateJavaScriptInMainFrame(m_toolbarPage, stringUTF8(script).get(), nullptr, nullptr);
    if (m_sidebarPage && m_sidebarReady)
        WKPageEvaluateJavaScriptInMainFrame(m_sidebarPage, stringUTF8(script).get(), nullptr, nullptr);
}

void BrowserWindow::pushState()
{
    if (!m_toolbarReady && !m_sidebarReady)
        return;
    std::string json = "{\"version\":" + util::jsonString(PRISM_VERSION_WSTRING);
    json += ",\"uiBase\":" + util::jsonString(engine::uiDirectoryURL());
    json += ",\"theme\":\"" + std::string(m_darkTheme ? "dark" : "light") + "\"";
    json += ",\"search\":" + util::jsonString(engine::searchURLTemplate());
    json += ",\"accent\":" + util::jsonString(settings::accent());
    json += ",\"compact\":" + std::string(settings::compactTabs() ? "true" : "false");
    json += ",\"blocked\":" + std::to_string(blocker::blockedCount());
    if (Tab* active = activeTab())
        json += ",\"blockedHere\":" + std::to_string(requestfilter::blockedOnPage(active->page));
    json += ",\"active\":" + std::to_string(m_activeTabId);
    json += ",\"actions\":" + extensions::actionsJSON(m_activeTabId);
    json += ",\"palette\":" + util::jsonString(settings::palette());
    json += ",\"vertical\":" + std::string(settings::verticalTabs() ? "true" : "false");
    json += ",\"sidebarCollapsed\":" + std::string(settings::verticalTabsCollapsed() ? "true" : "false");
    json += ",\"groupStyle\":" + util::jsonString(settings::groupStyle());
    json += ",\"homeButton\":" + std::string(settings::showHomeButton() ? "true" : "false");
    json += ",\"hiddenActions\":" + settings::hiddenActionsJSON();
    json += ",\"groups\":[";
    for (size_t i = 0; i < m_groups.size(); ++i) {
        auto& group = m_groups[i];
        json += (i ? "," : "") + std::string("{\"id\":") + std::to_string(group.id) + ",\"name\":" + util::jsonString(group.name)
            + ",\"color\":" + util::jsonString(group.color) + ",\"collapsed\":" + (group.collapsed ? "true" : "false") + "}";
    }
    json += "]";
    if (Tab* active = activeTab())
        json += ",\"activeUnit\":" + std::to_string(unitOf(active));
    json += ",\"bookmarksBar\":" + std::string(settings::showBookmarksBar() ? "true" : "false");
    if (settings::showBookmarksBar())
        json += ",\"bookmarks\":" + bookmarks::barJSON();
    json += ",\"tabs\":[";
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        Tab* tab = m_tabs[i].get();
        if (i)
            json += ',';
        char progress[32];
        snprintf(progress, sizeof(progress), "%.3f", tab->progress);
        char zoom[32];
        snprintf(zoom, sizeof(zoom), "%.3f", WKPageGetPageZoomFactor(tab->page));
        json += "{\"id\":" + std::to_string(tab->id);
        json += ",\"title\":" + util::jsonString(tab->title);
        json += ",\"url\":" + util::jsonString(extensions::displayURL(tab->url));
        json += ",\"group\":" + std::to_string(tab->groupId);
        json += ",\"icon\":" + util::jsonString(tab->extensionPages || engine::isInternalURL(tab->url) ? L"" : icons::fileURLFor(tab->url));
        json += ",\"loading\":" + std::string(tab->loading ? "true" : "false");
        json += ",\"progress\":" + std::string(progress);
        json += ",\"canBack\":" + std::string(WKPageCanGoBack(tab->page) ? "true" : "false");
        json += ",\"canForward\":" + std::string(WKPageCanGoForward(tab->page) ? "true" : "false");
        json += ",\"zoom\":" + std::string(zoom);
        json += ",\"store\":" + std::string(extensions::isStoreURL(tab->url) ? "true" : "false");
        json += ",\"shields\":" + std::string(shields::isActiveFor(tab->url) ? "true" : "false");
        bool bookmarkable = !engine::isInternalURL(tab->url) && !tab->extensionPages
            && (schemeOf(tab->url) == L"http" || schemeOf(tab->url) == L"https" || schemeOf(tab->url) == L"file");
        json += ",\"bookmarkable\":" + std::string(bookmarkable ? "true" : "false");
        // Only the active tab shows the star.
        bool bookmarked = bookmarkable && tab->id == m_activeTabId && bookmarks::findURL(tab->url);
        json += ",\"bookmarked\":" + std::string(bookmarked ? "true" : "false");
        json += ",\"find\":{\"open\":" + std::string(tab->findOpen ? "true" : "false") + ",\"query\":" + util::jsonString(tab->findQuery)
            + ",\"matches\":" + std::to_string(tab->findMatches) + ",\"index\":" + std::to_string(tab->findIndex) + "}";
        passwords::Offer offer;
        if (!tab->prompts.empty()) {
            auto& prompt = tab->prompts.front();
            json += ",\"prompt\":{\"kind\":" + util::jsonString(prompt.kind) + ",\"site\":" + util::jsonString(permissions::displayName(prompt.origin)) + "}";
        } else if (passwords::offerFor(tab->id, offer)) {
            // "Save password?" (never the password itself).
            json += ",\"prompt\":{\"kind\":\"password\",\"site\":" + util::jsonString(offer.site) + ",\"username\":" + util::jsonString(offer.username)
                + ",\"update\":" + (offer.update ? "true" : "false") + "}";
        }
        json += "}";
    }
    json += "]}";
    runToolbarScript("window.prism && prism.update(" + json + ");");
}

void BrowserWindow::showToast(const std::wstring& text)
{
    runToolbarScript("window.prism && prism.toast(" + util::jsonString(text) + ");");
}

void BrowserWindow::focusAddressBar()
{
    if (!m_toolbarHwnd)
        return;
    SetFocus(m_toolbarHwnd);
    runToolbarScript("window.prism && prism.focusAddress();");
}

void BrowserWindow::focusContent()
{
    Tab* tab = activeTab();
    if (!tab)
        return;
    if (engine::isInternalPage(tab->url, L"newtab")) {
        // New tab: start typing straight away.
        focusAddressBar();
        return;
    }
    SetFocus(tab->hwnd);
}

void BrowserWindow::updateTitle()
{
    Tab* tab = activeTab();
    std::wstring title = tab && !tab->title.empty() ? tab->title + L" - Prism" : L"Prism";
    SetWindowTextW(m_hwnd, title.c_str());
}

// ---------------------------------------------------------------------------
// Tabs

Tab* BrowserWindow::activeTab() const
{
    return findTab(m_activeTabId);
}

Tab* BrowserWindow::findTab(int id) const
{
    for (auto& tab : m_tabs) {
        if (tab->id == id)
            return tab.get();
    }
    return nullptr;
}

Tab* BrowserWindow::findTab(WKPageRef page) const
{
    for (auto& tab : m_tabs) {
        if (tab->page == page)
            return tab.get();
    }
    return nullptr;
}

int BrowserWindow::indexOf(const Tab* tab) const
{
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i].get() == tab)
            return static_cast<int>(i);
    }
    return -1;
}

Tab* BrowserWindow::newTab(const std::wstring& url, bool select, WKPageConfigurationRef configuration)
{
    auto tab = std::make_unique<Tab>();
    tab->window = this;
    tab->id = ++s_nextTabId;

    // Extension pages (options pages) get the extension configuration: it's
    // fixed for the life of the tab.
    if (!configuration && !extensions::resolveExtensionURL(url).empty()) {
        configuration = engine::extensionPageConfiguration();
        tab->extensionPages = true;
    } else if (configuration == engine::extensionPageConfiguration())
        tab->extensionPages = true;
    RECT rect = { };
    tab->view = Ref<WKViewRef>::adopt(WKViewCreate(rect, configuration ? configuration : engine::pageConfiguration(), m_hwnd));
    // Only the selected tab counts as "in the window" (see selectTab).
    WKViewSetIsInWindow(tab->view.get(), false);
    tab->hwnd = WKViewGetWindow(tab->view.get());
    tab->page = WKViewGetPage(tab->view.get());
    ShowWindow(tab->hwnd, SW_HIDE);
    engine::configurePage(tab->page, scaleFactor());

    Tab* raw = tab.get();
    attachClients(raw);

    // Popups and links open next to the tab they came from (in its group);
    // new tabs go last.
    int active = indexOf(activeTab());
    if (configuration && active >= 0) {
        raw->groupId = m_tabs[static_cast<size_t>(active)]->groupId;
        m_tabs.insert(m_tabs.begin() + active + 1, std::move(tab));
    } else
        m_tabs.push_back(std::move(tab));

    extensions::tabCreated(raw->id);
    if (!url.empty())
        navigate(raw, url);
    if (select)
        selectTab(raw->id);
    else
        pushState();
    scheduleSessionSave();
    return raw;
}

void BrowserWindow::attachClients(Tab* tab)
{
    WKPageUIClientV6 uiClient = { };
    uiClient.base.version = 6;
    uiClient.base.clientInfo = tab;
    uiClient.createNewPage = createNewPage;
    uiClient.close = closePage;
    uiClient.runJavaScriptAlert = runJavaScriptAlert;
    uiClient.runJavaScriptConfirm = runJavaScriptConfirm;
    uiClient.runJavaScriptPrompt = runJavaScriptPrompt;
    uiClient.printFrame = printFrame;
    if (WKGeolocationPermissionRequestAllow && WKGeolocationPermissionRequestDeny && WKSecurityOriginCopyToString)
        uiClient.decidePolicyForGeolocationPermissionRequest = decidePolicyForGeolocation;
    if (WKNotificationPermissionRequestAllow && WKNotificationPermissionRequestDeny && WKSecurityOriginCopyToString)
        uiClient.decidePolicyForNotificationPermissionRequest = decidePolicyForNotifications;
    WKPageSetPageUIClient(tab->page, &uiClient.base);

    WKPageNavigationClientV3 navigationClient = { };
    navigationClient.base.version = 3;
    navigationClient.base.clientInfo = tab;
    navigationClient.decidePolicyForNavigationAction = decidePolicyForNavigationAction;
    navigationClient.decidePolicyForNavigationResponse = decidePolicyForNavigationResponse;
    navigationClient.didFailProvisionalNavigation = didFailProvisionalNavigation;
    navigationClient.webProcessDidTerminate = webProcessDidTerminate;
    navigationClient.navigationActionDidBecomeDownload = navigationActionDidBecomeDownload;
    navigationClient.navigationResponseDidBecomeDownload = navigationResponseDidBecomeDownload;
    navigationClient.contextMenuDidCreateDownload = contextMenuDidCreateDownload;
    navigationClient.didCommitNavigation = didCommitNavigation;
    navigationClient.didSameDocumentNavigation = didSameDocumentNavigation;
    WKPageSetPageNavigationClient(tab->page, &navigationClient.base);
    requestfilter::attachPage(tab->page);

    if (WKPageSetPageContextMenuClient && WKContextMenuListenerUseContextMenuItems && WKMutableArrayCreate) {
        WKPageContextMenuClientV4 contextMenuClient = { };
        contextMenuClient.base.version = 4;
        contextMenuClient.base.clientInfo = tab;
        contextMenuClient.getContextMenuFromProposedMenuAsync = getContextMenuAsync;
        contextMenuClient.customContextMenuItemSelected = contextMenuItemSelected;
        WKPageSetPageContextMenuClient(tab->page, &contextMenuClient.base);
    }

    if (WKPageSetPageFindClient) {
        WKPageFindClientV0 findClient = { };
        findClient.base.version = 0;
        findClient.base.clientInfo = tab;
        findClient.didFindString = didFindString;
        findClient.didFailToFindString = didFailToFindString;
        WKPageSetPageFindClient(tab->page, &findClient.base);
    }

    WKPageStateClientV0 stateClient = { };
    stateClient.base.version = 0;
    stateClient.base.clientInfo = tab;
    stateClient.didChangeTitle = tabStateChanged;
    stateClient.didChangeIsLoading = tabStateChanged;
    stateClient.didChangeEstimatedProgress = tabStateChanged;
    stateClient.didChangeActiveURL = tabStateChanged;
    stateClient.didChangeCanGoBack = tabStateChanged;
    stateClient.didChangeCanGoForward = tabStateChanged;
    WKPageSetPageStateClient(tab->page, &stateClient.base);
}

void BrowserWindow::selectTab(int id)
{
    Tab* next = findTab(id);
    if (!next)
        return;
    Tab* previous = activeTab();
    suggestions::hide();
    next->lastActive = ++s_activationCounter;
    // A tab in a collapsed group opens the group.
    if (TabGroup* group = findGroup(next->groupId); group && group->collapsed)
        group->collapsed = false;
    m_activeTabId = id;
    // Like Safari, WebKit only lets a page start videos and sound while it's
    // in a window: background tabs (opened from a link, restored from last
    // time) wait until you look at them. Media that's already playing keeps
    // playing after you switch away.
    WKViewSetIsInWindow(next->view.get(), true);
    layout();
    ShowWindow(next->hwnd, SW_SHOW);
    if (previous && previous != next) {
        // The page that's going away shouldn't keep thinking the mouse is
        // over it (sites like YouTube play previews on hover).
        PostMessageW(previous->hwnd, WM_MOUSELEAVE, 0, 0);
        WKViewSetIsInWindow(previous->view.get(), false);
        ShowWindow(previous->hwnd, SW_HIDE);
    }
    // Nor should the new one, unless the mouse really is over it.
    POINT cursor;
    RECT area;
    if (GetCursorPos(&cursor) && GetWindowRect(next->hwnd, &area) && !PtInRect(&area, cursor))
        PostMessageW(next->hwnd, WM_MOUSELEAVE, 0, 0);
    focusContent();
    updateTitle();
    pushState();
    scheduleSessionSave();
    if (!previous || previous != next)
        extensions::tabActivated(id, m_id);
}

void BrowserWindow::selectAdjacentTab(int delta)
{
    if (m_tabs.size() < 2)
        return;
    int count = static_cast<int>(m_tabs.size());
    int index = (indexOf(activeTab()) + delta + count) % count;
    selectTab(m_tabs[index]->id);
}

void BrowserWindow::closeTab(int id)
{
    Tab* tab = findTab(id);
    if (!tab)
        return;
    passwords::tabClosed(id);
    int index = indexOf(tab);

    if (!tab->url.empty() && !engine::isInternalURL(tab->url)) {
        m_closedURLs.push_back(tab->url);
        if (m_closedURLs.size() > 25)
            m_closedURLs.erase(m_closedURLs.begin());
    }

    bool wasActive = tab->id == m_activeTabId;
    denyPrompts(tab);
    extensions::tabRemoved(tab->id, m_id, m_tabs.size() == 1);
    detachClients(tab->page);
    scheduleSessionSave();
    // Destroying the view's window makes WebKit close the page and its process.
    DestroyWindow(tab->hwnd);
    requestfilter::detachPage(m_tabs[index]->page);
    m_tabs.erase(m_tabs.begin() + index);
    normalizeGroups();

    if (m_tabs.empty()) {
        // Last tab closed: close the window, but not from inside a WebKit
        // callback (this may be running inside the toolbar's own callback).
        m_activeTabId = 0;
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        return;
    }
    if (wasActive) {
        index = std::min(index, static_cast<int>(m_tabs.size()) - 1);
        m_activeTabId = 0;
        selectTab(m_tabs[index]->id);
    } else
        pushState();
}

void BrowserWindow::reopenClosedTab()
{
    if (m_closedURLs.empty())
        return;
    std::wstring url = m_closedURLs.back();
    m_closedURLs.pop_back();
    newTab(url, true);
}

void BrowserWindow::navigate(Tab* tab, const std::wstring& url)
{
    if (url.rfind(L"prism://", 0) == 0) {
        std::wstring name = lower(url.substr(8));
        // prism://settings#shields opens Settings at its Shields section.
        std::wstring section;
        if (size_t hash = name.find(L'#'); hash != std::wstring::npos) {
            section = name.substr(hash + 1);
            name = name.substr(0, hash);
        }
        while (!name.empty() && name.back() == L'/')
            name.pop_back();
        if (!isKnownInternalPage(name))
            name = L"newtab";
        std::wstring query;
        if (name == L"newtab")
            query = L"search=" + util::urlEncode(engine::searchURLTemplate());
        else if (!section.empty())
            query = L"section=" + util::urlEncode(section);
        tab->url = engine::internalPageURL(name, query);
        engine::loadInternalPage(tab->page, name, query);
    } else {
        // chrome-extension://<id>/page.html -> the page's file.
        // (and older file:// addresses of extension pages -> their address)
        std::wstring target = url;
        std::wstring scheme = schemeOf(url);
        std::wstring mapped = extensions::resolveExtensionURL(url);
        if (!mapped.empty())
            target = mapped;
        else if (scheme == L"chrome-extension" || scheme == L"moz-extension")
            target = engine::internalPageURL(L"error", L"kind=network&url=" + util::urlEncode(url) + L"&message=" + util::urlEncode(L"There's no installed (and turned on) extension with this address."));
        // Extension pages need an extension tab (see newTab), and extension
        // tabs show nothing else.
        bool extensionPage = !mapped.empty();
        if (extensionPage != tab->extensionPages && !engine::isInternalURL(target)) {
            bool replace = tab->url.empty() || engine::isInternalPage(tab->url, L"newtab");
            int oldId = tab->id;
            Tab* fresh = newTab(target, true);
            moveTab(fresh->id, indexOf(tab) + 1);
            if (replace)
                PostMessageW(m_hwnd, WM_PRISM_CLOSE_TAB, static_cast<WPARAM>(oldId), 0);
            return;
        }
        tab->url = target;
        WKPageLoadURL(tab->page, wk::url(target).get());
    }
    tabChanged(tab);
}

void BrowserWindow::tabStateChanged(const void* clientInfo)
{
    Tab* tab = tabFromClientInfo(clientInfo);
    tab->title = toWide(Ref<WKStringRef>::adopt(WKPageCopyTitle(tab->page)).get());
    std::wstring active = toWide(Ref<WKURLRef>::adopt(WKPageCopyActiveURL(tab->page)).get());
    if (!active.empty())
        tab->url = active;
    bool wasLoading = tab->loading;
    tab->progress = WKPageGetEstimatedProgress(tab->page);
    tab->loading = tab->progress > 0 && tab->progress < 1;
    if (wasLoading && !tab->loading)
        tab->window->requestIcon(tab);
    if (!tab->extensionPages)
        history::updateTitle(tab->url, tab->title);
    tab->window->tabChanged(tab);
}

void BrowserWindow::tabChanged(Tab* tab)
{
    extensions::tabUpdated(tab->id);
    if (tab->id == m_activeTabId)
        updateTitle();
    pushState();
    if (sessionURL(tab) != tab->savedURL)
        scheduleSessionSave();
}

// ---------------------------------------------------------------------------
// Session restore
//
// %LOCALAPPDATA%\Prism\session.txt, UTF-8, one line per entry:
//   W                 starts a window
//   T <tab> 1|0 <tab> url   a tab (1 = the selected one)

static std::wstring sessionFile()
{
    return util::dataDirectory() + L"\\session.txt";
}

// The address to reopen a tab with: prism:// names for Prism's pages, and the
// original address for error pages.
std::wstring BrowserWindow::sessionURL(const Tab* tab)
{
    const std::wstring& url = tab->url;
    if (url.empty())
        return { };
    if (!engine::isInternalURL(url))
        return url;
    for (auto* name : { L"newtab", L"whatsnew", L"settings", L"extensions", L"history", L"bookmarks", L"passwords" }) {
        if (engine::isInternalPage(url, name))
            return std::wstring(L"prism://") + name;
    }
    if (engine::isInternalPage(url, L"error")) {
        size_t start = url.find(L"url=");
        if (start == std::wstring::npos)
            return { };
        start += 4;
        size_t end = url.find(L'&', start);
        return util::urlDecode(url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
    }
    return { };
}

void BrowserWindow::scheduleSessionSave()
{
    if (s_windows.empty())
        return;
    s_sessionDirty = true;
    // Save a second after things settle rather than on every change.
    SetTimer(s_windows.front()->m_hwnd, kSessionTimer, 1000, nullptr);
}

void BrowserWindow::saveSession()
{
    s_sessionDirty = false;
    std::string contents;
    for (auto* window : s_windows) {
        std::string tabs;
        // G <number> <color> <collapsed 1|0> <name>; tabs refer to the number.
        std::string groups;
        for (size_t i = 0; i < window->m_groups.size(); ++i) {
            auto& group = window->m_groups[i];
            groups += "G\t" + std::to_string(i + 1) + "\t" + util::toUTF8(group.color) + "\t" + (group.collapsed ? "1" : "0") + "\t" + util::toUTF8(group.name) + "\n";
        }
        for (auto& tab : window->m_tabs) {
            tab->savedURL = sessionURL(tab.get());
            if (tab->savedURL.empty())
                continue;
            int groupNumber = 0;
            for (size_t i = 0; i < window->m_groups.size(); ++i) {
                if (window->m_groups[i].id == tab->groupId)
                    groupNumber = static_cast<int>(i) + 1;
            }
            // T <selected 1|0> <url> [<group number>]
            tabs += std::string("T\t") + (tab->id == window->m_activeTabId ? "1" : "0") + "\t" + util::toUTF8(tab->savedURL)
                + (groupNumber ? "\t" + std::to_string(groupNumber) : "") + "\n";
        }
        if (!tabs.empty())
            contents += "W\n" + groups + tabs;
    }
    util::writeFileAtomically(sessionFile(), contents);
}

bool BrowserWindow::restoreSession(const std::vector<std::wstring>& extraURLs)
{
    struct SavedWindow {
        std::vector<std::wstring> urls;
        int active { -1 };
        std::vector<TabGroup> groups;
        std::vector<int> tabGroups;
    };
    std::vector<SavedWindow> windows;

    std::string contents = util::readFile(sessionFile());
    size_t position = 0;
    while (position < contents.size()) {
        size_t end = contents.find('\n', position);
        std::string line = contents.substr(position, end == std::string::npos ? std::string::npos : end - position);
        position = end == std::string::npos ? contents.size() : end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line == "W") {
            windows.emplace_back();
        } else if (line.rfind("G\t", 0) == 0 && !windows.empty()) {
            // G <number> <color> <collapsed> <name>
            std::vector<std::string> parts;
            size_t start = 2;
            for (int field = 0; field < 3; ++field) {
                size_t next = line.find('\t', start);
                if (next == std::string::npos)
                    break;
                parts.push_back(line.substr(start, next - start));
                start = next + 1;
            }
            if (parts.size() < 3)
                continue;
            TabGroup group;
            group.color = util::fromUTF8(parts[1]);
            if (group.color.size() != 7 || group.color[0] != L'#')
                group.color = kGroupColors[0];
            group.collapsed = parts[2] == "1";
            group.name = util::fromUTF8(line.substr(start));
            windows.back().groups.push_back(group);
        } else if (line.rfind("T\t", 0) == 0 && line.size() > 4 && !windows.empty()) {
            size_t tab = line.find('\t', 2);
            if (tab == std::string::npos)
                continue;
            std::string rest = line.substr(tab + 1);
            int groupNumber = 0;
            // URLs never contain a tab character, so a last field is the group.
            if (size_t last = rest.rfind('\t'); last != std::string::npos) {
                groupNumber = atoi(rest.c_str() + last + 1);
                rest = rest.substr(0, last);
            }
            std::wstring url = util::fromUTF8(rest);
            // Only reopen web pages and Prism's own pages.
            std::wstring scheme = schemeOf(url);
            if (scheme != L"http" && scheme != L"https" && scheme != L"prism" && scheme != L"file")
                continue;
            if (line.substr(2, tab - 2) == "1")
                windows.back().active = static_cast<int>(windows.back().urls.size());
            windows.back().urls.push_back(url);
            windows.back().tabGroups.push_back(groupNumber > 0 && groupNumber <= static_cast<int>(windows.back().groups.size()) ? groupNumber : 0);
        }
    }
    windows.erase(std::remove_if(windows.begin(), windows.end(), [](const SavedWindow& w) { return w.urls.empty(); }), windows.end());
    if (windows.empty())
        return false;

    for (auto& url : extraURLs) {
        // Prism's own pages (like What's new after an update) that are
        // already among last time's tabs are selected, not opened again.
        bool found = false;
        if (url.rfind(L"prism://", 0) == 0) {
            for (auto& window : windows) {
                for (size_t i = 0; i < window.urls.size() && !found; ++i) {
                    if (lower(window.urls[i]) == lower(url)) {
                        window.active = static_cast<int>(i);
                        found = true;
                    }
                }
                if (found)
                    break;
            }
        }
        if (found)
            continue;
        windows.front().urls.push_back(url);
        windows.front().active = static_cast<int>(windows.front().urls.size()) - 1;
        windows.front().tabGroups.push_back(0);
    }
    bool opened = false;
    for (auto& window : windows)
        opened = BrowserWindow::open(window.urls, window.active, window.groups, window.tabGroups) || opened;
    return opened;
}

// ---------------------------------------------------------------------------
// Settings

void BrowserWindow::settingsChanged()
{
    engine::applyCookiePolicy();
    privacy::settingsChanged();
    for (auto* window : s_windows) {
        window->updateTheme();
        if (settings::verticalTabs())
            window->createSidebar();
        else
            window->destroySidebar();
        window->layout();
        window->pushState();
    }
}

void BrowserWindow::applyInterfaceCSS()
{
    for (auto* window : s_windows)
        window->runToolbarScript("window.prism && prism.setUserCSS(" + json::quote(usercss::interfaceCSS()) + ");");
}

void BrowserWindow::passwordOfferChanged(int tabId)
{
    for (auto* window : s_windows) {
        if (window->findTab(tabId)) {
            if (window->m_toolbarReady)
                window->pushState();
            window->layout();
        }
    }
}

void BrowserWindow::refreshToolbars()
{
    for (auto* window : s_windows)
        window->pushState();
}

void BrowserWindow::evaluateInInternalPages(const std::wstring& name, const std::string& script)
{
    for (auto* window : s_windows) {
        for (auto& tab : window->m_tabs) {
            if (engine::isInternalPage(tab->url, name))
                WKPageEvaluateJavaScriptInMainFrame(tab->page, stringUTF8(script).get(), nullptr, nullptr);
        }
    }
}

void BrowserWindow::openInternalPage(const std::wstring& name, const std::wstring& section)
{
    for (auto& tab : m_tabs) {
        if (engine::isInternalPage(tab->url, name)) {
            selectTab(tab->id);
            if (!section.empty()) {
                std::string script = "window.prismShowSection && prismShowSection(" + util::jsonString(section) + ");";
                WKPageEvaluateJavaScriptInMainFrame(tab->page, stringUTF8(script).get(), nullptr, nullptr);
            }
            return;
        }
    }
    std::wstring target = L"prism://" + name + (section.empty() ? L"" : L"#" + section);
    Tab* active = activeTab();
    // Reuse an empty new tab instead of opening another one.
    if (active && engine::isInternalPage(active->url, L"newtab")) {
        navigate(active, target);
        focusContent();
        return;
    }
    newTab(target, true);
}

static std::wstring pickFolder(HWND owner, const std::wstring& start)
{
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return result;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose where Prism saves downloads");
    IShellItem* folder = nullptr;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
        dialog->SetFolder(folder);
        folder->Release();
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

// Commands from prism://settings. Each returns a string to the page.
// Settings as JSON, plus the theme actually in use (for "system").
// What prism://newtab shows: shortcuts (most visited sites or the bookmarks
// bar) and the background picture.
static std::wstring newTabBackgroundPath()
{
    std::wstring path = util::readSetting(L"NewTabBackground", L"");
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return { };
    return path;
}

static std::string newTabData()
{
    std::wstring mode = settings::newTabShortcuts();
    std::string json = "{\"mode\":" + util::jsonString(mode) + ",\"shortcuts\":[";
    bool first = true;
    auto add = [&](const std::wstring& url, const std::wstring& title) {
        json += (first ? "" : ",") + std::string("{\"url\":") + util::jsonString(url) + ",\"title\":" + util::jsonString(title)
            + ",\"icon\":" + util::jsonString(icons::fileURLFor(url)) + "}";
        first = false;
    };
    if (mode == L"top") {
        std::vector<std::wstring> hidden;
        std::wstring list = util::readSetting(L"NewTabHidden", L"");
        for (size_t start = 0; start < list.size();) {
            size_t comma = list.find(L',', start);
            hidden.push_back(list.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start));
            if (comma == std::wstring::npos)
                break;
            start = comma + 1;
        }
        for (auto& site : history::topSites(8, hidden))
            add(site.url, site.title);
    } else if (mode == L"bookmarks") {
        if (const bookmarks::Node* bar = bookmarks::find(bookmarks::kBarId)) {
            int count = 0;
            for (auto& child : bar->children) {
                if (child->folder || count >= 12)
                    continue;
                add(child->url, child->title);
                ++count;
            }
        }
    }
    json += "]";
    std::wstring background = newTabBackgroundPath();
    std::wstring backgroundURL;
    if (!background.empty()) {
        WIN32_FILE_ATTRIBUTE_DATA info = { };
        GetFileAttributesExW(background.c_str(), GetFileExInfoStandard, &info);
        backgroundURL = util::fileURLFromPath(background) + L"?" + std::to_wstring(info.ftLastWriteTime.dwLowDateTime);
    }
    json += ",\"background\":" + util::jsonString(backgroundURL) + "}";
    return json;
}

static std::string settingsForPage()
{
    std::string json = settings::toJSON();
    return std::string("{\"effectiveTheme\":\"") + (engine::darkTheme() ? "dark" : "light") + "\"," + json.substr(1);
}

std::string BrowserWindow::handleSettingsCommand(const std::wstring& command, const std::wstring& argument)
{
    if (command == L"get")
        return settingsForPage();

    if (command == L"set") {
        size_t newline = argument.find(L'\n');
        std::wstring key = argument.substr(0, newline);
        std::wstring value = newline == std::wstring::npos ? L"" : argument.substr(newline + 1);
        if (!settings::set(key, value))
            return "{\"error\":\"invalid\"}";
        settingsChanged();
        return settingsForPage();
    }

    if (command == L"pickDownloadFolder") {
        std::wstring folder = pickFolder(m_hwnd, settings::downloadDirectory());
        if (!folder.empty() && settings::set(L"downloadDir", folder))
            settingsChanged();
        return settingsForPage();
    }

    if (command == L"clearData") {
        bool cookies = argument.find(L"cookies") != std::wstring::npos;
        bool cache = argument.find(L"cache") != std::wstring::npos;
        if (argument.find(L"history") != std::wstring::npos) {
            history::clear();
            icons::clear();
            m_closedURLs.clear();
            if (!cookies && !cache) {
                showToast(L"History cleared");
                return "true";
            }
        }
        if (!cookies && !cache)
            return "false";
        HWND hwnd = m_hwnd;
        engine::clearBrowsingData(cookies, cache, [hwnd] {
            if (auto* window = BrowserWindow::fromHWND(hwnd))
                window->showToast(L"Browsing data cleared");
        });
        if (cookies)
            m_closedURLs.clear();
        return "true";
    }

    if (command == L"openDownloads") {
        ShellExecuteW(m_hwnd, L"open", settings::downloadDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return "true";
    }

    if (command == L"openDataFolder") {
        ShellExecuteW(m_hwnd, L"open", util::dataDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return "true";
    }

    if (command == L"version")
        return util::jsonString(PRISM_VERSION_WSTRING);

    if (command == L"pickNewTabBackground") {
        // A copy goes into Prism's folder, so moving the original is fine.
        auto picked = dialogs::openFile(m_hwnd, L"Choose a background picture", L"Pictures (*.jpg, *.png, *.webp, *.gif)", L"*.jpg;*.jpeg;*.png;*.webp;*.gif;*.bmp");
        if (!picked)
            return "false";
        std::wstring extension = picked->substr(picked->rfind(L'.') + 1);
        for (auto& c : extension)
            c = static_cast<wchar_t>(towlower(c));
        std::wstring target = util::dataDirectory() + L"\newtab-background." + extension;
        std::wstring old = newTabBackgroundPath();
        if (!old.empty() && old != target)
            DeleteFileW(old.c_str());
        if (!CopyFileW(picked->c_str(), target.c_str(), FALSE))
            return "false";
        util::writeSetting(L"NewTabBackground", target);
        return "true";
    }

    if (command == L"clearNewTabBackground") {
        std::wstring old = newTabBackgroundPath();
        if (!old.empty())
            DeleteFileW(old.c_str());
        util::writeSetting(L"NewTabBackground", L"");
        return "true";
    }

    if (command == L"newTabBackground")
        return util::jsonString(newTabBackgroundPath().empty() ? L"" : L"set");

    return "null";
}

// ---------------------------------------------------------------------------
// Page callbacks

WKPageRef BrowserWindow::createNewPage(WKPageRef, WKPageConfigurationRef configuration, WKNavigationActionRef, WKWindowFeaturesRef, const void* clientInfo)
{
    // window.open() and target="_blank" links open as a new tab. WebKit loads
    // the page into it; we only have to create it with the given configuration.
    Tab* opener = tabFromClientInfo(clientInfo);
    Tab* tab = opener->window->newTab(L"", true, configuration);
    // WebKit takes ownership of one reference.
    return static_cast<WKPageRef>(WKRetain(tab->page));
}

void BrowserWindow::closePage(WKPageRef, const void* clientInfo)
{
    // window.close(): close the tab once WebKit is done with this callback.
    Tab* tab = tabFromClientInfo(clientInfo);
    PostMessageW(tab->window->m_hwnd, WM_PRISM_CLOSE_TAB, static_cast<WPARAM>(tab->id), 0);
}

static std::wstring dialogTitle(Tab* tab)
{
    std::wstring host = engine::isInternalURL(tab->url) ? L"Prism" : hostOf(tab->url);
    return host.empty() ? L"Prism" : host + L" says";
}

void BrowserWindow::runJavaScriptAlert(WKPageRef, WKStringRef text, WKFrameRef, WKSecurityOriginRef, WKPageRunJavaScriptAlertResultListenerRef listener, const void* clientInfo)
{
    Tab* tab = tabFromClientInfo(clientInfo);
    MessageBoxW(tab->window->m_hwnd, toWide(text).c_str(), dialogTitle(tab).c_str(), MB_OK);
    WKPageRunJavaScriptAlertResultListenerCall(listener);
}

void BrowserWindow::runJavaScriptConfirm(WKPageRef, WKStringRef text, WKFrameRef, WKSecurityOriginRef, WKPageRunJavaScriptConfirmResultListenerRef listener, const void* clientInfo)
{
    Tab* tab = tabFromClientInfo(clientInfo);
    bool result = MessageBoxW(tab->window->m_hwnd, toWide(text).c_str(), dialogTitle(tab).c_str(), MB_OKCANCEL) == IDOK;
    WKPageRunJavaScriptConfirmResultListenerCall(listener, result);
}

void BrowserWindow::runJavaScriptPrompt(WKPageRef, WKStringRef text, WKStringRef defaultValue, WKFrameRef frame, WKSecurityOriginRef, WKPageRunJavaScriptPromptResultListenerRef listener, const void* clientInfo)
{
    Tab* tab = tabFromClientInfo(clientInfo);
    std::wstring message = toWide(text);

    // prism://settings talks to Prism the same way the toolbar does. Only the
    // settings page itself (its main frame, checked by the frame's own URL)
    // gets an answer, so websites can't read or change settings.
    if (message.rfind(L"prism:", 0) == 0 && frame && WKFrameIsMainFrame(frame)) {
        std::wstring frameURL = toWide(Ref<WKURLRef>::adopt(WKFrameCopyURL(frame)).get());
        bool isSettings = engine::isInternalPage(frameURL, L"settings");
        bool isExtensions = engine::isInternalPage(frameURL, L"extensions");
        bool isHistory = engine::isInternalPage(frameURL, L"history");
        bool isNewTab = engine::isInternalPage(frameURL, L"newtab");
        bool isBookmarks = engine::isInternalPage(frameURL, L"bookmarks");
        // prism://passwords: only that page, in its main frame.
        if (engine::isInternalPage(frameURL, L"passwords")) {
            std::wstring body = message.substr(6);
            size_t newline = body.find(L'\n');
            std::string answer = passwords::handleCommand(body.substr(0, newline), newline == std::wstring::npos ? L"" : body.substr(newline + 1), tab->window->m_hwnd);
            SecureZeroMemory(body.data(), body.size() * sizeof(wchar_t));
            SecureZeroMemory(message.data(), message.size() * sizeof(wchar_t));
            WKPageRunJavaScriptPromptResultListenerCall(listener, stringUTF8(answer).get());
            return;
        }
        // The "blocked" page can allow the site it's showing, and nothing else.
        if (engine::isInternalPage(frameURL, L"error") && message.rfind(L"prism:allowHttp\n", 0) == 0) {
            std::wstring site = message.substr(16);
            privacy::allowHttpThisSession(site);
            WKPageRunJavaScriptPromptResultListenerCall(listener, stringUTF8("true").get());
            return;
        }
        if (engine::isInternalPage(frameURL, L"error") && message.rfind(L"prism:allowSite\n", 0) == 0) {
            std::wstring site = message.substr(16);
            std::string blockedBy;
            bool allowed = blocker::isBlocked(util::toUTF8(site), &blockedBy) && blockedBy == "Prism Shields";
            if (allowed)
                shields::handleCommand(L"allowDomain", site);
            WKPageRunJavaScriptPromptResultListenerCall(listener, stringUTF8(allowed ? "true" : "false").get());
            return;
        }
        if (isSettings || isExtensions || isHistory || isBookmarks || isNewTab) {
            std::wstring body = message.substr(6);
            size_t newline = body.find(L'\n');
            std::wstring command = body.substr(0, newline);
            std::wstring argument = newline == std::wstring::npos ? L"" : body.substr(newline + 1);
            std::string answer;
            if (isSettings && command.rfind(L"shields.", 0) == 0) {
                answer = shields::handleCommand(command.substr(8), argument);
                refreshToolbars();
            } else if (isSettings && command.rfind(L"privacy.", 0) == 0)
                answer = privacy::handleCommand(command.substr(8), argument);
            else if (isSettings && command.rfind(L"usercss.", 0) == 0)
                answer = usercss::handleCommand(command.substr(8), argument);
            else if (isNewTab && command == L"data")
                answer = newTabData();
            else if (isNewTab && command == L"hide") {
                // A site removed from the shortcuts.
                std::wstring hidden = util::readSetting(L"NewTabHidden", L"");
                if (argument.find(L',') == std::wstring::npos && !argument.empty())
                    util::writeSetting(L"NewTabHidden", hidden.empty() ? argument : hidden + L"," + argument);
                answer = newTabData();
            } else if (isNewTab && command == L"open") {
                size_t split = argument.find(L'\n');
                std::wstring url = argument.substr(0, split);
                if (schemeOf(url) == L"http" || schemeOf(url) == L"https")
                    tab->window->openBookmarkURL(url, split == std::wstring::npos ? L"current" : argument.substr(split + 1));
                answer = "true";
            } else if (isNewTab)
                answer = "null";
            else if (isSettings && command.rfind(L"permissions.", 0) == 0)
                answer = permissions::handleCommand(command.substr(12), argument);
            else if ((isHistory || isBookmarks) && command == L"open") {
                // "<url>\n<current|foreground|background>"
                size_t split = argument.find(L'\n');
                tab->window->openBookmarkURL(argument.substr(0, split), split == std::wstring::npos ? L"current" : argument.substr(split + 1));
                answer = "true";
            } else if (isHistory)
                answer = history::handleCommand(command, argument);
            else if (isBookmarks)
                answer = bookmarks::handleCommand(command, argument, tab->window->m_hwnd);
            else if (isSettings)
                answer = tab->window->handleSettingsCommand(command, argument);
            else
                answer = extensions::handleCommand(command, argument, tab->window->m_hwnd);
            WKPageRunJavaScriptPromptResultListenerCall(listener, stringUTF8(answer).get());
            return;
        }
    }

    auto answer = dialogs::prompt(tab->window->m_hwnd, dialogTitle(tab), toWide(text), toWide(defaultValue));
    if (!answer) {
        WKPageRunJavaScriptPromptResultListenerCall(listener, nullptr);
        return;
    }
    WKPageRunJavaScriptPromptResultListenerCall(listener, string(*answer).get());
}

void BrowserWindow::decidePolicyForNavigationAction(WKPageRef, WKNavigationActionRef action, WKFramePolicyListenerRef listener, WKTypeRef, const void* clientInfo)
{
    Tab* tab = tabFromClientInfo(clientInfo);
    auto request = Ref<WKURLRequestRef>::adopt(WKNavigationActionCopyRequest(action));
    auto requestURL = Ref<WKURLRef>::adopt(WKURLRequestCopyURL(request.get()));
    std::wstring target = toWide(requestURL.get());
    std::wstring scheme = schemeOf(target);

    static const wchar_t* webSchemes[] = { L"http", L"https", L"file", L"about", L"data", L"blob", L"javascript", L"ws", L"wss" };
    // Extension pages only in extension tabs, and nothing else there. A link
    // from an extension page to a website opens in a normal tab; websites
    // can't open extension pages (frames are fine: the extension's server
    // only gives web pages the files the extension shares).
    bool extensionPage = !extensions::resolveExtensionURL(target).empty();
    bool webLike = scheme == L"file" || scheme == L"http" || scheme == L"https";
    if (webLike && !engine::isInternalURL(target) && extensionPage != tab->extensionPages) {
        auto frame = Ref<WKFrameInfoRef>::adopt(WKNavigationActionCopyTargetFrameInfo(action));
        bool mainFrame = !frame || WKFrameInfoGetIsMainFrame(frame.get());
        if (mainFrame) {
            WKFramePolicyListenerIgnore(listener);
            if (tab->extensionPages) {
                BrowserWindow* window = tab->window;
                int index = window->indexOf(tab) + 1;
                util::postToMainThread([hwnd = window->m_hwnd, target, index] {
                    if (BrowserWindow* again = BrowserWindow::fromHWND(hwnd)) {
                        Tab* fresh = again->newTab(target, true);
                        again->moveTab(fresh->id, index);
                    }
                });
            }
            return;
        }
    }

    // Main-frame web pages: remove tracking parameters and use HTTPS.
    if ((scheme == L"http" || scheme == L"https") && !extensionPage) {
        auto frame = Ref<WKFrameInfoRef>::adopt(WKNavigationActionCopyTargetFrameInfo(action));
        if (!frame || WKFrameInfoGetIsMainFrame(frame.get())) {
            // An https page we upgraded to sent us straight back to http:
            // the site doesn't really do HTTPS, so don't loop.
            if (scheme == L"http" && !tab->upgradedTo.empty() && privacy::siteOf(target) == privacy::siteOf(tab->upgradedTo)
                && GetTickCount64() - tab->upgradedAt < 10000) {
                privacy::allowHttpThisSession(target);
                tab->upgradedTo.clear();
                tab->upgradedFrom.clear();
            }
            std::wstring cleaned = privacy::removeTrackingParameters(target);
            std::wstring upgraded = privacy::upgradeURL(cleaned);
            std::wstring next = upgraded.empty() ? cleaned : upgraded;
            if (next != target) {
                WKFramePolicyListenerIgnore(listener);
                if (!upgraded.empty()) {
                    tab->upgradedFrom = cleaned;
                    tab->upgradedTo = upgraded;
                    tab->upgradedAt = GetTickCount64();
                }
                int tabId = tab->id;
                util::postToMainThread([hwnd = tab->window->m_hwnd, tabId, next] {
                    if (BrowserWindow* window = BrowserWindow::fromHWND(hwnd)) {
                        if (Tab* again = window->findTab(tabId))
                            window->navigate(again, next);
                    }
                });
                return;
            }
        }
    }

    for (auto* web : webSchemes) {
        if (scheme == web) {
            // Prism Shields puts the site's filters in place before the page
            // (or frame) starts loading. WebKit delivers them to the page's
            // process ahead of the go-ahead below.
            if ((scheme == L"http" || scheme == L"https") && !extensionPage) {
                auto frame = Ref<WKFrameInfoRef>::adopt(WKNavigationActionCopyTargetFrameInfo(action));
                // No target frame means a new window: that's a top-level page too.
                if (!frame || WKFrameInfoGetIsMainFrame(frame.get())) {
                    blocker::noteTopLevelSite(util::toUTF8(hostOf(target)));
                    extensions::webRequestMainFrame(tab->id, util::toUTF8(target));
                }
                // Right after Prism starts (tabs from last time), wait for
                // Shields' blocker (5 s at most), so pages like YouTube get
                // their fixes from the start instead of showing an ad.
                if (shields::isStarting()) {
                    auto held = Ref<WKFramePolicyListenerRef>::retain(listener);
                    shields::whenReady([held, target] {
                        shields::prepareNavigation(target);
                        WKFramePolicyListenerUse(held.get());
                    });
                    return;
                }
                shields::prepareNavigation(target);
            }
            WKFramePolicyListenerUse(listener);
            return;
        }
    }

    WKFramePolicyListenerIgnore(listener);

    if (scheme == L"prism") {
        tab->window->navigate(tab, target);
        return;
    }
    if (scheme == L"chrome-extension" || scheme == L"moz-extension") {
        // Only an extension's own pages may link to its pages (websites
        // can't open them).
        std::wstring resolved = extensions::resolveExtensionURL(target);
        if (!resolved.empty() && extensions::displayURL(tab->url) != tab->url && hostOf(extensions::displayURL(tab->url)) == hostOf(target))
            tab->window->navigate(tab, resolved);
        return;
    }
    if (scheme.empty())
        return;

    // mailto:, tel:, steam:, ... belong to other apps. Always ask first, so a
    // website can't launch programs on your PC behind your back.
    std::wstring question = L"This page wants to open a link in another app:\n\n" + target + L"\n\nOpen it?";
    if (MessageBoxW(tab->window->m_hwnd, question.c_str(), L"Open in another app?", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
        ShellExecuteW(tab->window->m_hwnd, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void BrowserWindow::decidePolicyForNavigationResponse(WKPageRef, WKNavigationResponseRef response, WKFramePolicyListenerRef listener, WKTypeRef, const void*)
{
    auto frame = Ref<WKFrameInfoRef>::adopt(WKNavigationResponseCopyFrameInfo(response));
    bool isMainFrame = frame && WKFrameInfoGetIsMainFrame(frame.get());
    // Anything WebKit can't display in the main frame (zip files, installers,
    // ...) becomes a download.
    if (WKNavigationResponseCanShowMIMEType(response) || !isMainFrame)
        WKFramePolicyListenerUse(listener);
    else
        WKFramePolicyListenerDownload(listener);
}

void BrowserWindow::didFailProvisionalNavigation(WKPageRef, WKNavigationRef, WKErrorRef error, WKTypeRef, const void* clientInfo)
{
    Tab* tab = tabFromClientInfo(clientInfo);
    int code = WKErrorGetErrorCode(error);
    // -999: cancelled (e.g. you clicked another link), 102: became a download,
    // 204: handled by a plug-in. None of these are real failures.
    if (code == -999 || code == 102 || code == 204)
        return;

    std::wstring failingURL = toWide(Ref<WKURLRef>::adopt(WKErrorCopyFailingURL(error)).get());
    std::wstring description = toWide(Ref<WKStringRef>::adopt(WKErrorCopyLocalizedDescription(error)).get());

    // 60 = curl couldn't verify the site's certificate. WebKit's curl only
    // trusts the root certificates already in Windows' store, and Windows
    // adds missing (but trusted) roots only when its own networking meets
    // them. So: connect once with Windows' networking (WinHTTP), which adds
    // the root if Microsoft trusts it, then restart WebKit's network process
    // so it reads the store again, and load the page again. Once per site.
    static std::set<std::wstring> certificateChecked;
    std::wstring certificateSite = lower(hostOf(failingURL));
    if (code == 60 && lower(failingURL.substr(0, 8)) == L"https://" && certificateChecked.insert(certificateSite).second) {
        HWND hwnd = tab->window->m_hwnd;
        int tabId = tab->id;
        std::thread([hwnd, tabId, failingURL, description, code] {
            http::Request request;
            request.method = L"HEAD";
            request.url = failingURL;
            http::Response response;
            std::wstring problem;
            bool trusted = http::request(request, response, problem, 4096);
            util::postToMainThread([hwnd, tabId, failingURL, description, code, trusted] {
                BrowserWindow* window = BrowserWindow::fromHWND(hwnd);
                Tab* tab = window ? window->findTab(tabId) : nullptr;
                if (!tab)
                    return;
                if (!trusted) {
                    showLoadError(tab, failingURL, description, code);
                    return;
                }
                engine::restartNetworkProcess();
                window->navigate(tab, failingURL);
            });
        }).detach();
        return;
    }

    // An address Prism switched to https didn't load: the site has no
    // (working) HTTPS. "HTTPS first" opens the http address; "HTTPS only"
    // asks first.
    if (!tab->upgradedFrom.empty() && privacy::siteOf(failingURL) == privacy::siteOf(tab->upgradedTo)) {
        std::wstring original = tab->upgradedFrom;
        tab->upgradedFrom.clear();
        if (privacy::httpsMode() == L"only") {
            tab->url = original;
            engine::loadInternalPage(tab->page, L"error", L"kind=https&url=" + util::urlEncode(original));
            tab->window->tabChanged(tab);
        } else {
            privacy::allowHttpThisSession(original);
            tab->window->navigate(tab, original);
            if (tab->id == tab->window->m_activeTabId)
                tab->window->showToast(L"This site has no secure (HTTPS) version, so it's open without one.");
        }
        return;
    }
    showLoadError(tab, failingURL, description, code);
}

void BrowserWindow::showLoadError(Tab* tab, const std::wstring& failingURL, const std::wstring& description, int code)
{
    std::wstring query = L"kind=network&url=" + util::urlEncode(failingURL)
        + L"&message=" + util::urlEncode(description) + L"&code=" + std::to_wstring(code);

    // A page on an extension's blocklist (e.g. clicking an ad): say so.
    std::string owner;
    std::wstring failingHost = hostOf(failingURL);
    if (!failingHost.empty() && failingHost[0] != L'[')
        failingHost = failingHost.substr(0, failingHost.find(L':'));
    if (blocker::isBlocked(util::toUTF8(failingHost), &owner))
        query = L"kind=blocked&url=" + util::urlEncode(failingURL) + L"&owner=" + util::urlEncode(util::fromUTF8(owner));
    tab->url = failingURL;
    engine::loadInternalPage(tab->page, L"error", query);
    tab->window->tabChanged(tab);
}

void BrowserWindow::webProcessDidTerminate(WKPageRef, uint32_t reason, const void* clientInfo)
{
    Tab* tab = tabFromClientInfo(clientInfo);
    // 2 = Prism closed it on purpose (closing the tab); anything else is a crash
    // or the page using too much memory/CPU.
    if (reason == 2)
        return;
    std::wstring lastURL = engine::isInternalURL(tab->url) ? L"" : tab->url;
    std::wstring query = L"kind=crash&url=" + util::urlEncode(lastURL) + L"&code=" + std::to_wstring(reason);
    tab->crashed = true;
    tab->loading = false;
    engine::loadInternalPage(tab->page, L"error", query);
    tab->window->tabChanged(tab);
}

void BrowserWindow::navigationActionDidBecomeDownload(WKPageRef, WKNavigationActionRef, WKDownloadRef download, const void* clientInfo)
{
    tabFromClientInfo(clientInfo)->window->startDownload(download);
}

void BrowserWindow::navigationResponseDidBecomeDownload(WKPageRef, WKNavigationResponseRef, WKDownloadRef download, const void* clientInfo)
{
    tabFromClientInfo(clientInfo)->window->startDownload(download);
}

void BrowserWindow::contextMenuDidCreateDownload(WKPageRef, WKDownloadRef download, const void* clientInfo)
{
    tabFromClientInfo(clientInfo)->window->startDownload(download);
}

void BrowserWindow::startDownload(WKDownloadRef download)
{
    auto* info = new DownloadInfo { m_hwnd, { } };
    WKDownloadClientV0 client = { };
    client.base.version = 0;
    client.base.clientInfo = info;
    client.decideDestinationWithResponse = downloadDecideDestination;
    client.didFinish = downloadDidFinish;
    client.didFailWithError = downloadDidFail;
    WKDownloadSetClient(download, &client.base);
}

// ---------------------------------------------------------------------------
// Menu, zoom, developer tools

void BrowserWindow::showMenu(int x, int y)
{
    enum { NewTab = 1, NewWindow, ReopenTab, ZoomIn, ZoomOut, ZoomReset, DevTools, Downloads, Extensions, Settings, WhatsNew, About,
        History, Bookmarks, BookmarksBar, Find, Print, Passwords };

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, NewTab, L"New tab\tCtrl+T");
    AppendMenuW(menu, MF_STRING, NewWindow, L"New window\tCtrl+N");
    AppendMenuW(menu, MF_STRING | (m_closedURLs.empty() ? MF_GRAYED : 0), ReopenTab, L"Reopen closed tab\tCtrl+Shift+T");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, History, L"History\tCtrl+H");
    AppendMenuW(menu, MF_STRING, Bookmarks, L"Bookmarks\tCtrl+Shift+O");
    AppendMenuW(menu, MF_STRING, Passwords, L"Passwords");
    AppendMenuW(menu, MF_STRING | (settings::showBookmarksBar() ? MF_CHECKED : 0), BookmarksBar, L"Show bookmarks bar\tCtrl+Shift+B");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Find, L"Find in page\tCtrl+F");
    AppendMenuW(menu, MF_STRING, Print, L"Print\tCtrl+P");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ZoomIn, L"Zoom in\tCtrl++");
    AppendMenuW(menu, MF_STRING, ZoomOut, L"Zoom out\tCtrl+-");
    AppendMenuW(menu, MF_STRING, ZoomReset, L"Reset zoom\tCtrl+0");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Downloads, L"Open Downloads folder");
    AppendMenuW(menu, MF_STRING | (WKPageGetInspector ? 0 : MF_GRAYED), DevTools, L"Developer tools\tF12");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Extensions, L"Extensions\tCtrl+Shift+E");
    AppendMenuW(menu, MF_STRING, Settings, L"Settings\tCtrl+,");
    AppendMenuW(menu, MF_STRING, WhatsNew, L"What's new");
    AppendMenuW(menu, MF_STRING, About, L"About Prism");

    double scale = scaleFactor();
    POINT point = { static_cast<LONG>(x * scale), static_cast<LONG>(y * scale) };
    ClientToScreen(m_toolbarHwnd, &point);
    int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_NONOTIFY,
        point.x, point.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);

    switch (choice) {
    case NewTab: newTab(L"prism://newtab", true); break;
    case NewWindow: BrowserWindow::open({ }); break;
    case ReopenTab: reopenClosedTab(); break;
    case ZoomIn: zoom(1.1); break;
    case ZoomOut: zoom(1 / 1.1); break;
    case ZoomReset: zoom(0); break;
    case DevTools: showDevTools(); break;
    case Downloads: handleToolbarCommand(L"openDownloads", L""); break;
    case Extensions: openInternalPage(L"extensions"); break;
    case Settings: openInternalPage(L"settings"); break;
    case WhatsNew: openInternalPage(L"whatsnew"); break;
    case History: openInternalPage(L"history"); break;
    case Passwords: openInternalPage(L"passwords"); break;
    case Bookmarks: openInternalPage(L"bookmarks"); break;
    case BookmarksBar: toggleBookmarksBar(); break;
    case Find: openFind(); break;
    case Print: printFrame(nullptr, nullptr, activeTab()); break;
    case About: {
        std::wstring text = L"Prism " PRISM_VERSION_WSTRING L"\n\n"
            L"A private, independent web browser built on WebKit.\n"
            L"No telemetry. No tracking. No accounts.\n\n"
            L"Licensed under the Mozilla Public License 2.0.";
        MessageBoxW(m_hwnd, text.c_str(), L"About Prism", MB_OK | MB_ICONINFORMATION);
        break;
    }
    }
}

void BrowserWindow::zoom(double factor)
{
    Tab* tab = activeTab();
    if (!tab)
        return;
    double current = WKPageGetPageZoomFactor(tab->page);
    double next = factor == 0 ? 1.0 : std::clamp(current * factor, 0.3, 5.0);
    // Snap to 100% when we pass it, so zooming back and forth lands exactly there.
    if (std::abs(next - 1.0) < 0.04)
        next = 1.0;
    WKPageSetPageZoomFactor(tab->page, next);
    showToast(L"Zoom " + std::to_wstring(static_cast<int>(std::lround(next * 100))) + L"%");
    pushState();
}

void BrowserWindow::showDevTools()
{
    Tab* tab = activeTab();
    if (!tab || !WKPageGetInspector || !WKInspectorShow)
        return;
    if (auto inspector = WKPageGetInspector(tab->page))
        WKInspectorShow(inspector);
}

// ---------------------------------------------------------------------------
// Keyboard shortcuts

bool BrowserWindow::handleShortcut(const MSG& msg)
{
    // Prism's own shortcuts first; keys it doesn't use can be extensions'.
    if (handleBrowserShortcut(msg))
        return true;
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN)
        return false;
    UINT key = static_cast<UINT>(msg.wParam);
    if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU)
        return false;
    Tab* tab = activeTab();
    return extensions::runShortcut(m_hwnd, tab ? tab->id : -1, key, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_MENU) < 0, GetKeyState(VK_SHIFT) < 0);
}

bool BrowserWindow::handleBrowserShortcut(const MSG& msg)
{
    if (msg.message == WM_XBUTTONUP) {
        Tab* tab = activeTab();
        if (!tab)
            return false;
        if (GET_XBUTTON_WPARAM(msg.wParam) == XBUTTON1)
            WKPageGoBack(tab->page);
        else
            WKPageGoForward(tab->page);
        return true;
    }
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN)
        return false;

    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    bool shift = GetKeyState(VK_SHIFT) < 0;
    bool alt = GetKeyState(VK_MENU) < 0;
    UINT key = static_cast<UINT>(msg.wParam);
    Tab* tab = activeTab();

    if (ctrl && !alt) {
        switch (key) {
        case 'T':
            if (shift) reopenClosedTab();
            else newTab(L"prism://newtab", true);
            return true;
        case 'N':
            if (shift) return false;
            BrowserWindow::open({ });
            return true;
        case 'W':
        case VK_F4:
            if (shift && key == 'W') {
                // Ctrl+Shift+W closes the whole window, like Chrome.
                PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
                return true;
            }
            if (tab) closeTab(tab->id);
            return true;
        case VK_TAB:
            selectAdjacentTab(shift ? -1 : 1);
            return true;
        case VK_NEXT:
            selectAdjacentTab(1);
            return true;
        case VK_PRIOR:
            selectAdjacentTab(-1);
            return true;
        case 'L':
            focusAddressBar();
            return true;
        case 'F':
            if (shift) return false;
            openFind();
            return true;
        case 'G':
            if (tab && tab->findOpen && !tab->findQuery.empty())
                findInPage(tab, shift ? -1 : 1);
            else
                openFind();
            return true;
        case 'P':
            if (shift) return false;
            printFrame(nullptr, nullptr, tab);
            return true;
        case 'H':
            if (shift) return false;
            openInternalPage(L"history");
            return true;
        case 'D':
            if (shift) return false;
            runToolbarScript("window.prism && prism.clickBookmarkButton();");
            return true;
        case 'B':
            if (!shift) return false;
            toggleBookmarksBar();
            return true;
        case 'O':
            if (!shift) return false;
            openInternalPage(L"bookmarks");
            return true;
        case 'R':
            if (tab) WKPageReload(tab->page);
            return true;
        case VK_OEM_PLUS:
        case VK_ADD:
            zoom(1.1);
            return true;
        case 'E':
            if (!shift) return false;
            openInternalPage(L"extensions");
            return true;
        case VK_OEM_COMMA:
            openInternalPage(L"settings");
            return true;
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            zoom(1 / 1.1);
            return true;
        case '0':
        case VK_NUMPAD0:
            zoom(0);
            return true;
        case 'I':
            if (!shift) return false;
            showDevTools();
            return true;
        }
        if (key >= '1' && key <= '9' && !m_tabs.empty() && settings::groupStyle() == L"vivaldi" && !settings::verticalTabs()) {
            // The Nth group as shown (the selected one is always first): pick
            // it, or cycle through its tabs if it's already selected.
            auto shown = displayedUnits();
            size_t index = key == '9' ? shown.size() - 1 : std::min<size_t>(key - '1', shown.size() - 1);
            selectUnit(shown[index], true);
            return true;
        }
        if (key >= '1' && key <= '9' && !m_tabs.empty()) {
            size_t index = key == '9' ? m_tabs.size() - 1 : std::min<size_t>(key - '1', m_tabs.size() - 1);
            selectTab(m_tabs[index]->id);
            return true;
        }
        return false;
    }

    if (alt && !ctrl) {
        switch (key) {
        case VK_F4:
            // Web pages take key presses without handing Alt+F4 back to
            // Windows, so close the window here.
            PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
            return true;
        case VK_LEFT:
            if (tab) WKPageGoBack(tab->page);
            return true;
        case VK_RIGHT:
            if (tab) WKPageGoForward(tab->page);
            return true;
        case 'D':
            focusAddressBar();
            return true;
        case VK_HOME:
            if (tab) navigate(tab, L"prism://newtab");
            return true;
        }
        return false;
    }

    switch (key) {
    case VK_F3:
        if (tab && tab->findOpen && !tab->findQuery.empty())
            findInPage(tab, shift ? -1 : 1);
        else
            openFind();
        return true;
    case VK_F5:
        if (tab) WKPageReload(tab->page);
        return true;
    case VK_F6:
        focusAddressBar();
        return true;
    case VK_F12:
        showDevTools();
        return true;
    case VK_BROWSER_BACK:
        if (tab) WKPageGoBack(tab->page);
        return true;
    case VK_BROWSER_FORWARD:
        if (tab) WKPageGoForward(tab->page);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// History

void BrowserWindow::didCommitNavigation(WKPageRef page, WKNavigationRef, WKTypeRef, const void* clientInfo)
{
    Tab* tab = tabFromClientInfo(clientInfo);
    tab->upgradedFrom.clear(); // the upgrade worked (or was replaced)
    requestfilter::pageCommitted(page);
    // A new page: the old page's questions go away, and find starts over.
    tab->window->denyPrompts(tab);
    if (tab->findOpen) {
        tab->findMatches = -1;
        tab->findIndex = 0;
    }
    std::wstring committed = toWide(Ref<WKURLRef>::adopt(WKPageCopyActiveURL(page)).get());
    passwords::tabNavigated(tab->id, committed);
    if (!tab->extensionPages)
        history::recordVisit(committed, L"");
}

void BrowserWindow::didSameDocumentNavigation(WKPageRef page, WKNavigationRef, WKSameDocumentNavigationType type, WKTypeRef, const void* clientInfo)
{
    // Sites that change pages without loading a new one (history.pushState)
    // get a history entry for each page, like a normal link.
    Tab* tab = tabFromClientInfo(clientInfo);
    if (type == kWKSameDocumentNavigationSessionStatePush && !tab->extensionPages)
        history::recordVisit(toWide(Ref<WKURLRef>::adopt(WKPageCopyActiveURL(page)).get()), L"");
}

// ---------------------------------------------------------------------------
// Printing: not possible with the stock Windows WebKit, which has no way to
// draw pages for a printer (its print code is macOS and Linux only).

void BrowserWindow::printFrame(WKPageRef, WKFrameRef, const void* clientInfo)
{
    if (Tab* tab = tabFromClientInfo(clientInfo))
        tab->window->showToast(L"Printing isn't available yet. It comes with Prism's own WebKit build (v0.9).");
}

// ---------------------------------------------------------------------------
// Find in page

void BrowserWindow::openFind()
{
    Tab* tab = activeTab();
    if (!tab || !WKPageFindString)
        return;
    if (!tab->findOpen) {
        tab->findOpen = true;
        tab->findMatches = -1;
        tab->findIndex = 0;
        pushState();
        layout();
    }
    SetFocus(m_toolbarHwnd);
    runToolbarScript("window.prism && prism.focusFind();");
    // Search again for what's in the box (the page may have changed).
    if (!tab->findQuery.empty())
        findInPage(tab, 0);
}

void BrowserWindow::closeFind(Tab* tab)
{
    if (!tab->findOpen)
        return;
    tab->findOpen = false;
    tab->findMatches = -1;
    tab->findIndex = 0;
    if (WKPageHideFindUI)
        WKPageHideFindUI(tab->page);
    if (tab->id == m_activeTabId) {
        pushState();
        layout();
    }
}

void BrowserWindow::findInPage(Tab* tab, int step)
{
    if (!WKPageFindString)
        return;
    if (tab->findQuery.empty()) {
        if (WKPageHideFindUI)
            WKPageHideFindUI(tab->page);
        tab->findMatches = -1;
        tab->findIndex = 0;
        pushState();
        return;
    }
    tab->findStep = step;
    WKFindOptions options = kWKFindOptionsCaseInsensitive | kWKFindOptionsWrapAround | kWKFindOptionsShowHighlight | kWKFindOptionsShowFindIndicator;
    if (step < 0)
        options |= kWKFindOptionsBackwards;
    WKPageFindString(tab->page, string(tab->findQuery).get(), options, 1000);
}

void BrowserWindow::didFindString(WKPageRef, WKStringRef text, unsigned matchCount, const void* clientInfo)
{
    Tab* tab = tabFromClientInfo(clientInfo);
    if (toWide(text) != tab->findQuery)
        return; // an older search
    // More than the maximum (1000) comes back as -1.
    long long count = matchCount == static_cast<unsigned>(-1) ? 1001 : static_cast<long long>(matchCount);
    tab->findMatches = count;
    if (tab->findStep == 0 || tab->findIndex <= 0 || tab->findIndex > count)
        tab->findIndex = count ? 1 : 0;
    else
        tab->findIndex = ((tab->findIndex - 1 + tab->findStep) % count + count) % count + 1;
    tab->findStep = 0;
    if (tab->id == tab->window->m_activeTabId)
        tab->window->pushState();
}

void BrowserWindow::didFailToFindString(WKPageRef, WKStringRef text, const void* clientInfo)
{
    Tab* tab = tabFromClientInfo(clientInfo);
    if (toWide(text) != tab->findQuery)
        return;
    tab->findMatches = 0;
    tab->findIndex = 0;
    tab->findStep = 0;
    if (tab->id == tab->window->m_activeTabId)
        tab->window->pushState();
}

// ---------------------------------------------------------------------------
// Permission prompts (location, notifications)

void BrowserWindow::decidePolicyForGeolocation(WKPageRef, WKFrameRef, WKSecurityOriginRef origin, WKGeolocationPermissionRequestRef request, const void* clientInfo)
{
    Tab* tab = tabFromClientInfo(clientInfo);
    std::wstring site = permissions::originString(origin);
    std::wstring decision = permissions::decision(site, L"location");
    if (decision == L"allow")
        WKGeolocationPermissionRequestAllow(request);
    else if (decision == L"block" || tab->extensionPages || site.empty() || site == L"null")
        WKGeolocationPermissionRequestDeny(request);
    else
        tab->window->queuePrompt(tab, L"location", site, request);
}

void BrowserWindow::decidePolicyForNotifications(WKPageRef, WKSecurityOriginRef origin, WKNotificationPermissionRequestRef request, const void* clientInfo)
{
    Tab* tab = tabFromClientInfo(clientInfo);
    std::wstring site = permissions::originString(origin);
    std::wstring decision = permissions::decision(site, L"notifications");
    if (decision == L"allow")
        WKNotificationPermissionRequestAllow(request);
    else if (decision == L"block" || tab->extensionPages || site.empty() || site == L"null")
        WKNotificationPermissionRequestDeny(request);
    else
        tab->window->queuePrompt(tab, L"notifications", site, request);
}

static void answerRequest(const PermissionPrompt& prompt, bool allow)
{
    if (prompt.kind == L"location") {
        auto request = static_cast<WKGeolocationPermissionRequestRef>(prompt.request.get());
        allow ? WKGeolocationPermissionRequestAllow(request) : WKGeolocationPermissionRequestDeny(request);
    } else {
        auto request = static_cast<WKNotificationPermissionRequestRef>(prompt.request.get());
        allow ? WKNotificationPermissionRequestAllow(request) : WKNotificationPermissionRequestDeny(request);
    }
}

void BrowserWindow::queuePrompt(Tab* tab, const std::wstring& kind, const std::wstring& origin, WKTypeRef request)
{
    // A site asking over and over only gets one question at a time.
    if (tab->prompts.size() >= 8) {
        answerRequest({ kind, origin, Ref<WKTypeRef>::retain(request) }, false);
        return;
    }
    tab->prompts.push_back({ kind, origin, Ref<WKTypeRef>::retain(request) });
    if (tab->id == m_activeTabId && tab->prompts.size() == 1) {
        pushState();
        layout();
    }
}

void BrowserWindow::answerPrompt(Tab* tab, const std::wstring& answer)
{
    if (tab->prompts.empty()) {
        passwords::answerOffer(tab->id, answer, m_hwnd);
        if (tab->id == m_activeTabId)
            focusContent();
        return;
    }
    // "allow" and "block" are remembered for the site; "dismiss" only says
    // no this time.
    PermissionPrompt first = tab->prompts.front();
    bool allow = answer == L"allow";
    if (answer == L"allow" || answer == L"block")
        permissions::set(first.origin, first.kind, allow);
    std::vector<PermissionPrompt> remaining;
    for (auto& prompt : tab->prompts) {
        if (prompt.kind == first.kind && prompt.origin == first.origin)
            answerRequest(prompt, allow);
        else
            remaining.push_back(prompt);
    }
    tab->prompts = std::move(remaining);
    if (tab->id == m_activeTabId) {
        pushState();
        layout();
        if (tab->prompts.empty())
            focusContent();
    }
}

void BrowserWindow::denyPrompts(Tab* tab)
{
    if (tab->prompts.empty())
        return;
    for (auto& prompt : tab->prompts)
        answerRequest(prompt, false);
    tab->prompts.clear();
    if (tab->id == m_activeTabId && m_toolbarReady) {
        pushState();
        layout();
    }
}

// ---------------------------------------------------------------------------
// Address-bar suggestions

static std::wstring searchEngineName()
{
    std::wstring host = lower(hostOf(engine::searchURLTemplate()));
    if (host.rfind(L"www.", 0) == 0)
        host = host.substr(4);
    static const std::pair<const wchar_t*, const wchar_t*> names[] = {
        { L"duckduckgo.com", L"DuckDuckGo" }, { L"startpage.com", L"Startpage" }, { L"search.brave.com", L"Brave Search" },
        { L"ecosia.org", L"Ecosia" }, { L"google.com", L"Google" }, { L"bing.com", L"Bing" },
    };
    for (auto& [domain, name] : names) {
        if (host == domain)
            return name;
    }
    return host;
}

std::string BrowserWindow::suggest(const std::wstring& argument)
{
    // {"text": "...", "go": "<where Enter goes>", "search": bool,
    //  "left": x, "bottom": y, "width": w}  (CSS pixels in the toolbar)
    json::Value request;
    json::parse(util::toUTF8(argument), request);
    std::wstring text = util::fromUTF8(request["text"].asString());
    std::wstring go = util::fromUTF8(request["go"].asString());
    auto items = suggestions::build(text, go, request["search"].asBool(), searchEngineName());

    double scale = scaleFactor();
    POINT corner = { static_cast<LONG>(request["left"].asNumber() * scale), static_cast<LONG>(request["bottom"].asNumber() * scale) };
    ClientToScreen(m_toolbarHwnd, &corner);
    RECT anchor = { corner.x, corner.y + static_cast<LONG>(4 * scale), corner.x + static_cast<LONG>(request["width"].asNumber() * scale), corner.y };
    suggestions::Style style;
    style.dark = m_darkTheme;
    style.scale = scale;
    std::wstring accent = settings::accent();
    unsigned long rgb = wcstoul(accent.c_str() + 1, nullptr, 16);
    style.accent = RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);

    HWND hwnd = m_hwnd;
    suggestions::show(m_hwnd, anchor, style, items, [hwnd](const suggestions::Item& item) {
        auto* window = BrowserWindow::fromHWND(hwnd);
        if (!window)
            return;
        window->runToolbarScript("window.prism && prism.suggestionsClosed();");
        if (Tab* tab = window->activeTab()) {
            window->navigate(tab, item.url);
            window->focusContent();
        }
    });
    return suggestions::isVisible() ? suggestions::itemsJSON(items) : "[]";
}

// ---------------------------------------------------------------------------
// Bookmarks

POINT BrowserWindow::toolbarPoint(const std::wstring& coordinates) const
{
    int x = 0, y = 0;
    swscanf_s(coordinates.c_str(), L"%d,%d", &x, &y);
    double scale = scaleFactor();
    POINT point = { static_cast<LONG>(x * scale), static_cast<LONG>(y * scale) };
    ClientToScreen(m_toolbarHwnd, &point);
    return point;
}

void BrowserWindow::toggleBookmarksBar()
{
    settings::set(L"bookmarksBar", settings::showBookmarksBar() ? L"0" : L"1");
    settingsChanged();
    evaluateInInternalPages(L"settings", "window.prismSettingsChanged && prismSettingsChanged();");
}

void BrowserWindow::openBookmarkURL(const std::wstring& url, const std::wstring& mode)
{
    if (mode == L"background")
        newTab(url, false);
    else if (mode == L"foreground")
        newTab(url, true);
    else if (Tab* tab = activeTab()) {
        navigate(tab, url);
        focusContent();
    }
}

void BrowserWindow::bookmarkButton(const std::wstring& coordinates)
{
    // The star in the address bar: bookmark this page, or (when it already
    // is) rename or remove it.
    Tab* tab = activeTab();
    if (!tab || engine::isInternalURL(tab->url) || tab->extensionPages || tab->url.empty())
        return;
    const bookmarks::Node* existing = bookmarks::findURL(tab->url);
    if (!existing) {
        if (bookmarks::add(tab->url, tab->title))
            showToast(settings::showBookmarksBar() ? L"Added to the bookmarks bar" : L"Bookmarked. Ctrl+Shift+B shows the bookmarks bar.");
        return;
    }
    long long id = existing->id;
    std::wstring title = existing->title;
    POINT point = toolbarPoint(coordinates);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Rename…");
    AppendMenuW(menu, MF_STRING, 2, L"Remove bookmark");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, L"Manage bookmarks");
    int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN, point.x, point.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    if (choice == 1) {
        if (auto name = dialogs::prompt(m_hwnd, L"Rename bookmark", L"Name:", title); name && !name->empty())
            bookmarks::rename(id, *name);
    } else if (choice == 2) {
        bookmarks::remove(id);
        showToast(L"Bookmark removed");
    } else if (choice == 3)
        openInternalPage(L"bookmarks");
}

namespace {

// What a bookmark menu item does when chosen.
struct BookmarkCommand {
    std::wstring url;            // open this
    std::vector<std::wstring> all; // or open all of these in new tabs
};

std::wstring menuText(std::wstring text)
{
    if (text.size() > 60)
        text = text.substr(0, 58) + L"…";
    std::wstring escaped;
    for (wchar_t c : text) {
        escaped += c;
        if (c == L'&')
            escaped += L'&'; // a single & would underline the next letter
    }
    return escaped.empty() ? L"(no name)" : escaped;
}

void appendBookmarkItems(HMENU menu, const bookmarks::Node& folder, std::vector<BookmarkCommand>& commands, int depth)
{
    std::vector<std::wstring> urls;
    for (auto& child : folder.children) {
        if (child->folder) {
            HMENU submenu = CreatePopupMenu();
            if (depth < 20)
                appendBookmarkItems(submenu, *child, commands, depth + 1);
            if (!GetMenuItemCount(submenu))
                AppendMenuW(submenu, MF_STRING | MF_GRAYED, 0, L"(empty)");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), menuText(child->title).c_str());
        } else {
            commands.push_back({ child->url, { } });
            urls.push_back(child->url);
            AppendMenuW(menu, MF_STRING, commands.size(), menuText(child->title.empty() ? child->url : child->title).c_str());
            menuicons::apply(menu, static_cast<UINT>(commands.size()), child->url);
        }
    }
    if (urls.size() >= 2) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        commands.push_back({ L"", urls });
        AppendMenuW(menu, MF_STRING, commands.size(), (L"Open all " + std::to_wstring(urls.size()) + L" in new tabs").c_str());
    }
}

} // namespace

static void runBookmarkCommand(BrowserWindow* window, const BookmarkCommand& command)
{
    if (!command.url.empty()) {
        window->navigate(window->activeTab(), command.url);
        return;
    }
    // A lot of tabs at once is easy to do by accident.
    if (command.all.size() > 15) {
        std::wstring question = L"Open " + std::to_wstring(command.all.size()) + L" tabs?";
        if (MessageBoxW(window->hwnd(), question.c_str(), L"Prism", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }
    for (size_t i = 0; i < command.all.size(); ++i)
        window->newTab(command.all[i], i == 0);
}

void BrowserWindow::showBookmarkFolder(long long folderId, POINT screen)
{
    const bookmarks::Node* folder = bookmarks::find(folderId);
    if (!folder || !folder->folder)
        return;
    std::vector<BookmarkCommand> commands;
    HMENU menu = CreatePopupMenu();
    appendBookmarkItems(menu, *folder, commands, 0);
    if (!GetMenuItemCount(menu))
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"(empty)");
    int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    if (choice > 0 && static_cast<size_t>(choice) <= commands.size() && activeTab())
        runBookmarkCommand(this, commands[static_cast<size_t>(choice) - 1]);
}

void BrowserWindow::showBookmarkOverflow(const std::vector<long long>& ids, POINT screen)
{
    // The bookmarks that don't fit on the bar.
    std::vector<BookmarkCommand> commands;
    HMENU menu = CreatePopupMenu();
    for (long long id : ids) {
        const bookmarks::Node* node = bookmarks::find(id);
        if (!node)
            continue;
        if (node->folder) {
            HMENU submenu = CreatePopupMenu();
            appendBookmarkItems(submenu, *node, commands, 0);
            if (!GetMenuItemCount(submenu))
                AppendMenuW(submenu, MF_STRING | MF_GRAYED, 0, L"(empty)");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), menuText(node->title).c_str());
        } else {
            commands.push_back({ node->url, { } });
            AppendMenuW(menu, MF_STRING, commands.size(), menuText(node->title.empty() ? node->url : node->title).c_str());
            menuicons::apply(menu, static_cast<UINT>(commands.size()), node->url);
        }
    }
    int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN, screen.x, screen.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    if (choice > 0 && static_cast<size_t>(choice) <= commands.size() && activeTab())
        runBookmarkCommand(this, commands[static_cast<size_t>(choice) - 1]);
}

void BrowserWindow::showBookmarkMenu(long long id, POINT screen)
{
    // Right-click on the bookmarks bar.
    const bookmarks::Node* node = bookmarks::find(id);
    if (!node)
        return;
    bool folder = node->folder;
    std::wstring url = node->url;
    std::wstring title = node->title;
    enum { Open = 1, OpenNewTab, OpenBackground, Rename, Remove, Manage, HideBar };
    HMENU menu = CreatePopupMenu();
    if (!folder) {
        AppendMenuW(menu, MF_STRING, Open, L"Open");
        AppendMenuW(menu, MF_STRING, OpenNewTab, L"Open in new tab");
        AppendMenuW(menu, MF_STRING, OpenBackground, L"Open in background tab");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, Rename, L"Rename…");
    AppendMenuW(menu, MF_STRING, Remove, folder ? L"Delete folder" : L"Delete");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Manage, L"Manage bookmarks");
    AppendMenuW(menu, MF_STRING, HideBar, L"Hide bookmarks bar");
    int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    switch (choice) {
    case Open: openBookmarkURL(url, L"current"); break;
    case OpenNewTab: openBookmarkURL(url, L"foreground"); break;
    case OpenBackground: openBookmarkURL(url, L"background"); break;
    case Rename:
        if (auto name = dialogs::prompt(m_hwnd, folder ? L"Rename folder" : L"Rename bookmark", L"Name:", title); name && !name->empty())
            bookmarks::rename(id, *name);
        break;
    case Remove:
        if (folder && bookmarks::find(id) && !bookmarks::find(id)->children.empty()) {
            std::wstring question = L"Delete the folder \"" + title + L"\" and everything in it?";
            if (MessageBoxW(m_hwnd, question.c_str(), L"Prism", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
                break;
        }
        bookmarks::remove(id);
        break;
    case Manage: openInternalPage(L"bookmarks"); break;
    case HideBar: toggleBookmarksBar(); break;
    }
}

// ---------------------------------------------------------------------------
// Site icons

namespace {
struct IconRequest {
    std::wstring url;
};
}

void BrowserWindow::requestIcon(Tab* tab)
{
    if (tab->extensionPages || engine::isInternalURL(tab->url))
        return;
    std::wstring scheme = schemeOf(tab->url);
    if (scheme != L"http" && scheme != L"https")
        return;
    auto* request = new IconRequest { tab->url };
    WKPageEvaluateJavaScriptInMainFrame(tab->page, stringUTF8(icons::pageScript()).get(), request, [](WKTypeRef result, WKErrorRef, void* context) {
        std::unique_ptr<IconRequest> request(static_cast<IconRequest*>(context));
        std::wstring icon;
        if (result && WKGetTypeID(result) == WKStringGetTypeID())
            icon = toWide(static_cast<WKStringRef>(result));
        if (!icon.empty())
            icons::pageLoaded(request->url, icon);
    });
}

// ---------------------------------------------------------------------------
// Tab groups

TabGroup* BrowserWindow::findGroup(int id)
{
    if (id <= 0)
        return nullptr;
    for (auto& group : m_groups) {
        if (group.id == id)
            return &group;
    }
    return nullptr;
}

// Keeps each group's tabs next to each other (where the group's first tab
// is) and drops groups without tabs.
void BrowserWindow::normalizeGroups()
{
    for (auto& tab : m_tabs) {
        if (tab->groupId && !findGroup(tab->groupId))
            tab->groupId = 0;
    }
    std::vector<std::unique_ptr<Tab>> ordered;
    std::vector<int> emitted;
    std::vector<std::unique_ptr<Tab>*> slots;
    for (auto& tab : m_tabs)
        slots.push_back(&tab);
    for (size_t i = 0; i < slots.size(); ++i) {
        if (!*slots[i])
            continue;
        int group = (*slots[i])->groupId;
        if (!group) {
            ordered.push_back(std::move(*slots[i]));
            continue;
        }
        if (std::find(emitted.begin(), emitted.end(), group) != emitted.end())
            continue;
        emitted.push_back(group);
        for (size_t j = i; j < slots.size(); ++j) {
            if (*slots[j] && (*slots[j])->groupId == group)
                ordered.push_back(std::move(*slots[j]));
        }
    }
    m_tabs = std::move(ordered);
    std::erase_if(m_groups, [&](const TabGroup& group) { return std::find(emitted.begin(), emitted.end(), group.id) == emitted.end(); });
}

int BrowserWindow::createGroup(Tab* tab)
{
    TabGroup group;
    group.id = ++s_nextGroupId;
    // The first color no other group uses (then round again).
    group.color = kGroupColors[m_groups.size() % std::size(kGroupColors)];
    for (auto* color : kGroupColors) {
        bool used = false;
        for (auto& other : m_groups)
            used = used || other.color == color;
        if (!used) {
            group.color = color;
            break;
        }
    }
    m_groups.push_back(group);
    tab->groupId = group.id;
    normalizeGroups();
    pushState();
    scheduleSessionSave();
    return group.id;
}

void BrowserWindow::addTabToGroup(Tab* tab, int groupId)
{
    if (!findGroup(groupId))
        return;
    int from = indexOf(tab);
    auto owned = std::move(m_tabs[static_cast<size_t>(from)]);
    m_tabs.erase(m_tabs.begin() + from);
    int last = -1;
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i]->groupId == groupId)
            last = static_cast<int>(i);
    }
    owned->groupId = groupId;
    m_tabs.insert(m_tabs.begin() + (last + 1), std::move(owned));
    normalizeGroups();
    pushState();
    scheduleSessionSave();
}

void BrowserWindow::removeTabFromGroup(Tab* tab)
{
    int groupId = tab->groupId;
    if (!groupId)
        return;
    // Out of the group, just after it.
    int from = indexOf(tab);
    auto owned = std::move(m_tabs[static_cast<size_t>(from)]);
    m_tabs.erase(m_tabs.begin() + from);
    int last = from - 1;
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i]->groupId == groupId)
            last = static_cast<int>(i);
    }
    owned->groupId = 0;
    m_tabs.insert(m_tabs.begin() + (last + 1), std::move(owned));
    normalizeGroups();
    pushState();
    scheduleSessionSave();
}

void BrowserWindow::closeTabs(const std::vector<int>& ids)
{
    for (int id : ids) {
        if (m_tabs.empty())
            break;
        closeTab(id);
    }
}

void BrowserWindow::closeGroup(int id)
{
    std::vector<int> ids;
    for (auto& tab : m_tabs) {
        if (tab->groupId == id)
            ids.push_back(tab->id);
    }
    closeTabs(ids);
}

void BrowserWindow::ungroup(int id)
{
    for (auto& tab : m_tabs) {
        if (tab->groupId == id)
            tab->groupId = 0;
    }
    normalizeGroups();
    pushState();
    scheduleSessionSave();
}

void BrowserWindow::setGroupCollapsed(int id, bool collapsed)
{
    TabGroup* group = findGroup(id);
    if (!group)
        return;
    group->collapsed = collapsed;
    // Collapsing the selected tab's group: select the nearest tab outside it.
    Tab* active = activeTab();
    if (collapsed && active && active->groupId == id) {
        int index = indexOf(active);
        Tab* next = nullptr;
        for (int distance = 1; distance < static_cast<int>(m_tabs.size()) && !next; ++distance) {
            for (int candidate : { index + distance, index - distance }) {
                if (candidate < 0 || candidate >= static_cast<int>(m_tabs.size()))
                    continue;
                Tab* tab = m_tabs[static_cast<size_t>(candidate)].get();
                TabGroup* other = findGroup(tab->groupId);
                if (!other || !other->collapsed) {
                    next = tab;
                    break;
                }
            }
        }
        if (next)
            selectTab(next->id);
        else
            newTab(L"prism://newtab", true);
        group = findGroup(id);
        if (group)
            group->collapsed = true; // selectTab may have opened it again
    }
    pushState();
    scheduleSessionSave();
}

void BrowserWindow::moveTabTo(int tabId, int index, int groupId)
{
    Tab* tab = findTab(tabId);
    if (!tab)
        return;
    tab->groupId = findGroup(groupId) ? groupId : 0;
    moveTab(tabId, index);
    normalizeGroups();
    pushState();
    scheduleSessionSave();
}

std::vector<int> BrowserWindow::units() const
{
    std::vector<int> result;
    for (auto& tab : m_tabs) {
        int unit = unitOf(tab.get());
        if (result.empty() || result.back() != unit)
            result.push_back(unit);
    }
    return result;
}

int BrowserWindow::unitOf(const Tab* tab) const
{
    return tab->groupId > 0 ? tab->groupId : -tab->id;
}

// Vivaldi style: the selected group (or single tab) first, then the rest
// in their own order.
std::vector<int> BrowserWindow::displayedUnits() const
{
    std::vector<int> all = units();
    Tab* active = activeTab();
    if (!active)
        return all;
    int current = unitOf(active);
    std::vector<int> shown { current };
    for (int unit : all) {
        if (unit != current)
            shown.push_back(unit);
    }
    return shown;
}

void BrowserWindow::selectUnit(int unit, bool cycle)
{
    std::vector<Tab*> members;
    for (auto& tab : m_tabs) {
        if (unitOf(tab.get()) == unit)
            members.push_back(tab.get());
    }
    if (members.empty())
        return;
    Tab* active = activeTab();
    Tab* target = nullptr;
    if (cycle && active && unitOf(active) == unit) {
        // Already selected: the next tab of the group, round again at the end.
        auto it = std::find(members.begin(), members.end(), active);
        target = (it == members.end() || it + 1 == members.end()) ? members.front() : *(it + 1);
    } else {
        // The group's tab you used last.
        target = members.front();
        for (Tab* tab : members) {
            if (tab->lastActive > target->lastActive)
                target = tab;
        }
    }
    if (target != active)
        selectTab(target->id);
}

void BrowserWindow::moveUnit(int unit, int index)
{
    // Moves a group (or single tab) to another place among the groups. This
    // is its new home place in the Vivaldi-style strip.
    std::vector<std::unique_ptr<Tab>> moving;
    std::vector<std::unique_ptr<Tab>> rest;
    for (auto& tab : m_tabs)
        (unitOf(tab.get()) == unit ? moving : rest).push_back(std::move(tab));
    if (moving.empty()) {
        m_tabs = std::move(rest);
        return;
    }
    // Where unit number `index` starts in the remaining tabs.
    size_t position = rest.size();
    int seen = -1;
    int last = 0;
    for (size_t i = 0; i < rest.size(); ++i) {
        int current = unitOf(rest[i].get());
        if (i == 0 || current != last) {
            ++seen;
            last = current;
            if (seen == index) {
                position = i;
                break;
            }
        }
    }
    m_tabs.clear();
    for (size_t i = 0; i < rest.size(); ++i) {
        if (i == position) {
            for (auto& tab : moving)
                m_tabs.push_back(std::move(tab));
        }
        m_tabs.push_back(std::move(rest[i]));
    }
    if (position >= rest.size()) {
        for (auto& tab : moving) {
            if (tab)
                m_tabs.push_back(std::move(tab));
        }
    }
    pushState();
    scheduleSessionSave();
}

static std::wstring groupLabel(const TabGroup& group)
{
    if (!group.name.empty())
        return group.name;
    for (size_t i = 0; i < std::size(kGroupColors); ++i) {
        if (group.color == kGroupColors[i])
            return std::wstring(kGroupColorNames[i]) + L" group";
    }
    return L"Unnamed group";
}

void BrowserWindow::showTabMenu(int tabId, POINT screen)
{
    Tab* tab = findTab(tabId);
    if (!tab)
        return;
    enum { NewTabRight = 1, Reload, Duplicate, NewGroup, RemoveFromGroup, Close, CloseOthers, CloseRight, FirstGroup = 100 };
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, NewTabRight, L"New tab to the right");
    AppendMenuW(menu, MF_STRING, Reload, L"Reload");
    AppendMenuW(menu, MF_STRING, Duplicate, L"Duplicate");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, NewGroup, L"Add tab to new group");
    std::vector<int> groupChoices;
    if (!m_groups.empty()) {
        HMENU groups = CreatePopupMenu();
        for (auto& group : m_groups) {
            if (group.id == tab->groupId)
                continue;
            groupChoices.push_back(group.id);
            AppendMenuW(groups, MF_STRING, FirstGroup + groupChoices.size() - 1, menuText(groupLabel(group)).c_str());
        }
        if (groupChoices.empty())
            DestroyMenu(groups);
        else
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(groups), L"Add tab to group");
    }
    if (tab->groupId)
        AppendMenuW(menu, MF_STRING, RemoveFromGroup, L"Remove from group");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Close, L"Close tab\tCtrl+W");
    AppendMenuW(menu, MF_STRING | (m_tabs.size() > 1 ? 0 : MF_GRAYED), CloseOthers, L"Close other tabs");
    AppendMenuW(menu, MF_STRING | (indexOf(tab) + 1 < static_cast<int>(m_tabs.size()) ? 0 : MF_GRAYED), CloseRight, L"Close tabs to the right");
    int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    tab = findTab(tabId);
    if (!tab)
        return;
    switch (choice) {
    case NewTabRight: {
        Tab* fresh = newTab(L"prism://newtab", true);
        fresh->groupId = tab->groupId;
        moveTab(fresh->id, indexOf(tab) + 1);
        normalizeGroups();
        pushState();
        break;
    }
    case Reload: WKPageReload(tab->page); break;
    case Duplicate: {
        std::wstring url = sessionURL(tab);
        if (url.empty())
            break;
        Tab* copy = newTab(url, true);
        copy->groupId = tab->groupId;
        moveTab(copy->id, indexOf(tab) + 1);
        normalizeGroups();
        pushState();
        break;
    }
    case NewGroup: createGroup(tab); break;
    case RemoveFromGroup: removeTabFromGroup(tab); break;
    case Close: closeTab(tabId); break;
    case CloseOthers: {
        std::vector<int> ids;
        for (auto& other : m_tabs) {
            if (other->id != tabId)
                ids.push_back(other->id);
        }
        closeTabs(ids);
        break;
    }
    case CloseRight: {
        std::vector<int> ids;
        for (size_t i = static_cast<size_t>(indexOf(tab)) + 1; i < m_tabs.size(); ++i)
            ids.push_back(m_tabs[i]->id);
        closeTabs(ids);
        break;
    }
    default:
        if (choice >= FirstGroup && static_cast<size_t>(choice - FirstGroup) < groupChoices.size())
            addTabToGroup(tab, groupChoices[static_cast<size_t>(choice - FirstGroup)]);
    }
}

void BrowserWindow::showGroupMenu(int groupId, POINT screen)
{
    TabGroup* group = findGroup(groupId);
    if (!group)
        return;
    enum { Rename = 1, Toggle, NewTabInGroup, Ungroup, CloseGroup, FirstColor = 100 };
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, menuText(groupLabel(*group)).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Rename, L"Name group…");
    HMENU colors = CreatePopupMenu();
    for (size_t i = 0; i < std::size(kGroupColors); ++i)
        AppendMenuW(colors, MF_STRING | (group->color == kGroupColors[i] ? MF_CHECKED : 0), FirstColor + i, kGroupColorNames[i]);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(colors), L"Color");
    if (settings::groupStyle() != L"vivaldi" || settings::verticalTabs())
        AppendMenuW(menu, MF_STRING, Toggle, group->collapsed ? L"Expand group" : L"Collapse group");
    AppendMenuW(menu, MF_STRING, NewTabInGroup, L"New tab in group");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Ungroup, L"Ungroup");
    AppendMenuW(menu, MF_STRING, CloseGroup, L"Close group");
    std::wstring name = group->name;
    bool collapsed = group->collapsed;
    int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    if (!findGroup(groupId))
        return;
    switch (choice) {
    case Rename:
        if (auto answer = dialogs::prompt(m_hwnd, L"Name group", L"Name (leave empty for none):", name); answer) {
            if (TabGroup* again = findGroup(groupId)) {
                again->name = answer->size() > 60 ? answer->substr(0, 60) : *answer;
                pushState();
                scheduleSessionSave();
            }
        }
        break;
    case Toggle: setGroupCollapsed(groupId, !collapsed); break;
    case NewTabInGroup: {
        Tab* fresh = newTab(L"prism://newtab", true);
        addTabToGroup(fresh, groupId);
        break;
    }
    case Ungroup: ungroup(groupId); break;
    case CloseGroup: closeGroup(groupId); break;
    default:
        if (choice >= FirstColor && static_cast<size_t>(choice - FirstColor) < std::size(kGroupColors)) {
            findGroup(groupId)->color = kGroupColors[choice - FirstColor];
            pushState();
            scheduleSessionSave();
        }
    }
}

// ---------------------------------------------------------------------------
// Extensions menu (the puzzle button) and showing or hiding their buttons

void BrowserWindow::showExtensionsMenu(POINT screen)
{
    Tab* tab = activeTab();
    json::Value actions;
    json::parse(extensions::actionsJSON(tab ? tab->id : -1), actions);
    enum { Manage = 1, Base = 100 };
    // Per extension: Base + 4*i + {0 open, 1 show/hide, 2 options}
    HMENU menu = CreatePopupMenu();
    std::vector<std::string> ids;
    for (auto& action : actions.items()) {
        std::string id = action["id"].asString();
        size_t i = ids.size();
        ids.push_back(id);
        bool hidden = settings::isActionHidden(id);
        HMENU submenu = CreatePopupMenu();
        AppendMenuW(submenu, MF_STRING, Base + i * 4, L"Open");
        AppendMenuW(submenu, MF_STRING | (hidden ? 0 : MF_CHECKED), Base + i * 4 + 1, L"Show in toolbar");
        AppendMenuW(submenu, MF_STRING | (action["options"].asBool() ? 0 : MF_GRAYED), Base + i * 4 + 2, L"Options");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), menuText(util::fromUTF8(action["name"].asString())).c_str());
    }
    if (ids.empty())
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No extensions with buttons");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Manage, L"Manage extensions\tCtrl+Shift+E");
    int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN, screen.x, screen.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    if (choice == Manage) {
        openInternalPage(L"extensions");
        return;
    }
    if (choice < Base || static_cast<size_t>((choice - Base) / 4) >= ids.size())
        return;
    const std::string& id = ids[static_cast<size_t>((choice - Base) / 4)];
    switch ((choice - Base) % 4) {
    case 0: {
        // Popups open under the Extensions button.
        POINT client = screen;
        ScreenToClient(m_toolbarHwnd, &client);
        double scale = scaleFactor();
        Tab* current = activeTab();
        extensions::actionClicked(m_hwnd, current ? current->id : -1, id, static_cast<int>(client.x / scale), static_cast<int>(client.y / scale));
        break;
    }
    case 1:
        settings::setActionHidden(id, !settings::isActionHidden(id));
        refreshToolbars();
        break;
    case 2:
        extensions::openOptionsPage(id, m_id);
        break;
    }
}

void BrowserWindow::showHiddenActionMenu(const std::string& extensionId, int x, int y)
{
    // Right-click on an extension's button.
    Tab* tab = activeTab();
    json::Value actions;
    json::parse(extensions::actionsJSON(tab ? tab->id : -1), actions);
    std::wstring name;
    bool options = false;
    for (auto& action : actions.items()) {
        if (action["id"].asString() == extensionId) {
            name = util::fromUTF8(action["name"].asString());
            options = action["options"].asBool();
        }
    }
    POINT point;
    anchorForExtension(x, y, point);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, menuText(name).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // The extension's own items for its button (contextMenus "action").
    std::vector<std::string> itemIds; // menu id - 100 -> item
    std::function<void(HMENU, const std::vector<extensions::MenuNode>&)> addItems = [&](HMENU target, const std::vector<extensions::MenuNode>& nodes) {
        for (auto& node : nodes) {
            std::wstring title = menuText(util::fromUTF8(node.title));
            UINT disabled = node.enabled ? 0 : MF_GRAYED;
            if (node.type == "separator") {
                AppendMenuW(target, MF_SEPARATOR, 0, nullptr);
            } else if (!node.children.empty()) {
                HMENU submenu = CreatePopupMenu();
                addItems(submenu, node.children);
                AppendMenuW(target, MF_POPUP | MF_STRING | disabled, reinterpret_cast<UINT_PTR>(submenu), title.c_str());
            } else {
                UINT id = 100 + static_cast<UINT>(itemIds.size());
                itemIds.push_back(node.itemId);
                UINT checked = (node.type == "checkbox" || node.type == "radio") && node.checked ? MF_CHECKED : 0;
                AppendMenuW(target, MF_STRING | disabled | checked, id, title.c_str());
            }
        }
    };
    std::vector<extensions::MenuNode> extensionItems = extensions::actionMenu(extensionId);
    if (!extensionItems.empty()) {
        addItems(menu, extensionItems);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING | (options ? 0 : MF_GRAYED), 1, L"Options");
    AppendMenuW(menu, MF_STRING, 2, L"Hide from toolbar");
    AppendMenuW(menu, MF_STRING, 3, L"Manage extension");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 4, L"Inspect popup");
    AppendMenuW(menu, MF_STRING, 5, L"Inspect background page");
    int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_RIGHTALIGN, point.x, point.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    if (choice >= 100 && static_cast<size_t>(choice - 100) < itemIds.size()) {
        extensions::MenuContext context;
        if (tab)
            context.pageURL = util::toUTF8(tab->url);
        extensions::menuItemClicked(extensionId, itemIds[choice - 100], context, tab ? tab->id : -1);
        return;
    }
    if (choice == 4)
        extensions::inspectPopup(m_hwnd, tab ? tab->id : -1, extensionId, x, y);
    else if (choice == 5) {
        if (!extensions::inspectBackground(extensionId))
            showToast(L"This extension has no background page running.");
    } else if (choice == 1)
        extensions::openOptionsPage(extensionId, m_id);
    else if (choice == 2) {
        settings::setActionHidden(extensionId, true);
        refreshToolbars();
        showToast(L"Hidden. It's still in the Extensions menu (the puzzle button).");
    } else if (choice == 3)
        openInternalPage(L"extensions");
}

// ---------------------------------------------------------------------------
// Windows and tabs for the extension APIs (Tabs.h)

const std::vector<BrowserWindow*>& BrowserWindow::all()
{
    return s_windows;
}

void BrowserWindow::moveTab(int id, int index)
{
    Tab* tab = findTab(id);
    if (!tab)
        return;
    int from = indexOf(tab);
    int count = static_cast<int>(m_tabs.size());
    if (index < 0 || index >= count)
        index = count - 1;
    if (from == index)
        return;
    auto owned = std::move(m_tabs[from]);
    m_tabs.erase(m_tabs.begin() + from);
    m_tabs.insert(m_tabs.begin() + index, std::move(owned));
    pushState();
    scheduleSessionSave();
}

// Extension toolbar buttons report their position in CSS pixels; popups open
// under them, right-aligned.
void BrowserWindow::anchorForExtension(int buttonRight, int buttonBottom, POINT& screen) const
{
    double scale = scaleFactor();
    screen = { static_cast<LONG>(buttonRight * scale), static_cast<LONG>(buttonBottom * scale) };
    ClientToScreen(m_toolbarHwnd, &screen);
}

namespace tabs {

static Info infoFor(const BrowserWindow* window, const Tab* tab, int index)
{
    Info info;
    info.id = tab->id;
    info.windowId = window->id();
    info.index = index;
    info.active = tab->id == window->activeTabId();
    info.loading = tab->loading;
    info.url = tab->url;
    info.title = tab->title;
    info.page = tab->page;
    return info;
}

static BrowserWindow* windowById(int windowId)
{
    if (windowId < 0)
        windowId = lastFocusedWindow();
    for (auto* window : BrowserWindow::all()) {
        if (window->id() == windowId)
            return window;
    }
    return nullptr;
}

static Tab* tabById(int tabId)
{
    for (auto* window : BrowserWindow::all()) {
        for (auto& tab : window->tabList()) {
            if (tab->id == tabId)
                return tab.get();
        }
    }
    return nullptr;
}

std::vector<WindowInfo> windows()
{
    std::vector<WindowInfo> out;
    for (auto* window : BrowserWindow::all()) {
        WindowInfo info;
        info.id = window->id();
        info.focused = window->id() == s_lastFocusedWindowId;
        GetWindowRect(window->hwnd(), &info.bounds);
        info.state = IsIconic(window->hwnd()) ? 1 : IsZoomed(window->hwnd()) ? 2 : 0;
        int index = 0;
        for (auto& tab : window->tabList())
            info.tabs.push_back(infoFor(window, tab.get(), index++));
        out.push_back(std::move(info));
    }
    return out;
}

bool find(int tabId, Info& out)
{
    for (auto* window : BrowserWindow::all()) {
        int index = 0;
        for (auto& tab : window->tabList()) {
            if (tab->id == tabId) {
                out = infoFor(window, tab.get(), index);
                return true;
            }
            ++index;
        }
    }
    return false;
}

bool findByPage(WKPageRef page, Info& out)
{
    for (auto* window : BrowserWindow::all()) {
        int index = 0;
        for (auto& tab : window->tabList()) {
            if (tab->page == page) {
                out = infoFor(window, tab.get(), index);
                return true;
            }
            ++index;
        }
    }
    return false;
}

int lastFocusedWindow()
{
    for (auto* window : BrowserWindow::all()) {
        if (window->id() == s_lastFocusedWindowId)
            return window->id();
    }
    return BrowserWindow::all().empty() ? -1 : BrowserWindow::all().back()->id();
}

int create(int windowId, const std::wstring& url, bool active, int index)
{
    BrowserWindow* window = windowById(windowId);
    if (!window)
        return -1;
    Tab* tab = window->newTab(url.empty() ? L"prism://newtab" : url, active);
    if (index >= 0)
        window->moveTab(tab->id, index);
    return tab->id;
}

int createWindow(const std::vector<std::wstring>& urls, bool focused)
{
    HWND previous = GetForegroundWindow();
    BrowserWindow* window = BrowserWindow::open(urls);
    if (!window)
        return -1;
    if (!focused && previous)
        SetForegroundWindow(previous);
    return window->id();
}

bool navigate(int tabId, const std::wstring& url)
{
    Tab* tab = tabById(tabId);
    if (!tab)
        return false;
    tab->window->navigate(tab, url);
    return true;
}

bool activate(int tabId)
{
    Tab* tab = tabById(tabId);
    if (!tab)
        return false;
    tab->window->selectTab(tabId);
    return true;
}

bool close(int tabId)
{
    Tab* tab = tabById(tabId);
    if (!tab)
        return false;
    // Not from inside a WebKit callback: post it.
    PostMessageW(tab->window->hwnd(), WM_PRISM_CLOSE_TAB, static_cast<WPARAM>(tabId), 0);
    return true;
}

bool reload(int tabId, bool bypassCache)
{
    Tab* tab = tabById(tabId);
    if (!tab)
        return false;
    if (bypassCache)
        WKPageReloadFromOrigin(tab->page);
    else
        WKPageReload(tab->page);
    return true;
}

bool goBack(int tabId)
{
    Tab* tab = tabById(tabId);
    if (!tab || !WKPageCanGoBack(tab->page))
        return false;
    WKPageGoBack(tab->page);
    return true;
}

bool goForward(int tabId)
{
    Tab* tab = tabById(tabId);
    if (!tab || !WKPageCanGoForward(tab->page))
        return false;
    WKPageGoForward(tab->page);
    return true;
}

bool focusWindow(int windowId)
{
    BrowserWindow* window = windowById(windowId);
    if (!window)
        return false;
    if (IsIconic(window->hwnd()))
        ShowWindow(window->hwnd(), SW_RESTORE);
    SetForegroundWindow(window->hwnd());
    return true;
}

bool closeWindow(int windowId)
{
    BrowserWindow* window = windowById(windowId);
    if (!window)
        return false;
    PostMessageW(window->hwnd(), WM_CLOSE, 0, 0);
    return true;
}

HWND windowHandle(int windowId)
{
    BrowserWindow* window = windowById(windowId);
    return window ? window->hwnd() : nullptr;
}

} // namespace tabs
