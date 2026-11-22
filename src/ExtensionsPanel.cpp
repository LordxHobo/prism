// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ExtensionsPanel.h"

#include "Engine.h"
#include "Util.h"
#include "WebKitApi.h"

#include <algorithm>

namespace extensionspanel {

using namespace wk;

namespace {

constexpr int kWidth = 330;         // CSS pixels
constexpr int kInitialHeight = 260; // until the page reports its height
constexpr int kMaxHeight = 640;
constexpr UINT WM_PANEL_CLOSE = WM_APP + 1;

struct Panel {
    HWND hwnd { };
    HWND owner { };
    Ref<WKViewRef> view;
    HWND viewHwnd { };
    WKPageRef page { };
    double scale { 1 };
    POINT anchor { };
    Callbacks callbacks;
};

Panel* g_panel = nullptr;
ULONGLONG g_closedAt = 0;
HWND g_closedOwner = nullptr;

void place(Panel* panel, int cssHeight)
{
    int width = static_cast<int>(kWidth * panel->scale);
    int height = static_cast<int>(std::clamp(cssHeight, 120, kMaxHeight) * panel->scale);
    int x = panel->anchor.x - width;
    int y = panel->anchor.y;
    // Stay on the monitor.
    MONITORINFO monitor = { sizeof(monitor) };
    if (GetMonitorInfoW(MonitorFromPoint(panel->anchor, MONITOR_DEFAULTTONEAREST), &monitor)) {
        x = std::max<int>(monitor.rcWork.left, std::min<int>(x, monitor.rcWork.right - width));
        y = std::min<int>(y, monitor.rcWork.bottom - height);
    }
    SetWindowPos(panel->hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

std::string handleCommand(Panel* panel, const std::wstring& command, const std::wstring& argument)
{
    std::string id = util::toUTF8(argument);
    if (command == L"list")
        return panel->callbacks.list ? panel->callbacks.list() : "null";
    if (command == L"open") {
        // The popup opens under the Extensions button; the panel goes away.
        auto open = panel->callbacks.openAction;
        PostMessageW(panel->hwnd, WM_PANEL_CLOSE, 0, 0);
        if (open)
            open(id);
        return "true";
    }
    if (command == L"pin" || command == L"unpin") {
        if (panel->callbacks.setPinned)
            panel->callbacks.setPinned(id, command == L"pin");
        return panel->callbacks.list ? panel->callbacks.list() : "null";
    }
    if (command == L"menu") {
        // "<id>\n<x>,<y>" (CSS pixels in the panel)
        size_t newline = argument.find(L'\n');
        std::string extId = util::toUTF8(argument.substr(0, newline));
        int x = 0, y = 0;
        if (newline != std::wstring::npos)
            swscanf_s(argument.c_str() + newline + 1, L"%d,%d", &x, &y);
        POINT point = { static_cast<LONG>(x * panel->scale), static_cast<LONG>(y * panel->scale) };
        ClientToScreen(panel->viewHwnd, &point);
        bool options = false;
        if (panel->callbacks.hasOptions)
            options = panel->callbacks.hasOptions(extId);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (options ? 0 : MF_GRAYED), 1, L"Options");
        AppendMenuW(menu, MF_STRING, 2, L"Manage extension");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 3, L"Inspect popup");
        AppendMenuW(menu, MF_STRING, 4, L"Inspect background page");
        int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN, point.x, point.y, 0, panel->hwnd, nullptr);
        DestroyMenu(menu);
        if (choice > 0) {
            auto run = panel->callbacks.menuChoice;
            PostMessageW(panel->hwnd, WM_PANEL_CLOSE, 0, 0);
            if (run)
                run(extId, choice == 1 ? "options" : choice == 2 ? "manage" : choice == 3 ? "inspectPopup" : "inspectBackground");
        }
        return "true";
    }
    if (command == L"manage") {
        auto manage = panel->callbacks.manage;
        PostMessageW(panel->hwnd, WM_PANEL_CLOSE, 0, 0);
        if (manage)
            manage();
        return "true";
    }
    if (command == L"resize") {
        place(panel, _wtoi(argument.c_str()));
        return "true";
    }
    if (command == L"close") {
        PostMessageW(panel->hwnd, WM_PANEL_CLOSE, 0, 0);
        return "true";
    }
    return "null";
}

void prompt(WKPageRef, WKStringRef message, WKStringRef, WKFrameRef frame, WKSecurityOriginRef, WKPageRunJavaScriptPromptResultListenerRef listener, const void* clientInfo)
{
    auto* panel = static_cast<Panel*>(const_cast<void*>(clientInfo));
    std::wstring text = toWide(message);
    std::string answer = "null";
    bool fromPanel = frame && WKFrameIsMainFrame(frame)
        && engine::isInternalPage(toWide(Ref<WKURLRef>::adopt(WKFrameCopyURL(frame)).get()), L"extensions-panel");
    if (fromPanel && text.rfind(L"prism:", 0) == 0) {
        text = text.substr(6);
        size_t newline = text.find(L'\n');
        std::wstring command = text.substr(0, newline);
        std::wstring argument = newline == std::wstring::npos ? L"" : text.substr(newline + 1);
        answer = handleCommand(panel, command, argument);
    }
    WKPageRunJavaScriptPromptResultListenerCall(listener, stringUTF8(answer).get());
}

void decidePolicy(WKPageRef, WKNavigationActionRef action, WKFramePolicyListenerRef listener, WKTypeRef, const void*)
{
    // The panel only ever shows its own page.
    auto request = Ref<WKURLRequestRef>::adopt(WKNavigationActionCopyRequest(action));
    auto requestURL = Ref<WKURLRef>::adopt(WKURLRequestCopyURL(request.get()));
    if (engine::isInternalPage(toWide(requestURL.get()), L"extensions-panel"))
        WKFramePolicyListenerUse(listener);
    else
        WKFramePolicyListenerIgnore(listener);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* panel = reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_ACTIVATE:
        // Clicking anywhere else closes the panel.
        if (LOWORD(wParam) == WA_INACTIVE)
            PostMessageW(hwnd, WM_PANEL_CLOSE, 0, 0);
        else if (panel && panel->viewHwnd)
            SetFocus(panel->viewHwnd);
        return 0;
    case WM_PANEL_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_SIZE:
        if (panel && panel->viewHwnd)
            MoveWindow(panel->viewHwnd, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        return 0;
    case WM_DESTROY:
        if (panel) {
            g_closedAt = GetTickCount64();
            g_closedOwner = panel->owner;
            if (g_panel == panel)
                g_panel = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            // The view (a child window) is destroyed with us; release it after.
            Panel* doomed = panel;
            util::postToMainThread([doomed] { delete doomed; });
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void registerClass()
{
    static bool registered = false;
    if (registered)
        return;
    WNDCLASSEXW windowClass = { sizeof(windowClass) };
    windowClass.style = CS_DROPSHADOW;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = L"PrismExtensionsPanel";
    RegisterClassExW(&windowClass);
    registered = true;
}

} // namespace

void close()
{
    if (g_panel && g_panel->hwnd)
        DestroyWindow(g_panel->hwnd);
}

void toggle(HWND owner, POINT anchor, double scale, Callbacks callbacks)
{
    // A click on the button first takes focus from the open panel (which
    // closes it), then arrives here: treat that as "close", not "reopen".
    if (g_panel) {
        close();
        return;
    }
    if (g_closedOwner == owner && GetTickCount64() - g_closedAt < 300)
        return;

    registerClass();
    auto* panel = new Panel;
    panel->owner = owner;
    panel->anchor = anchor;
    panel->scale = scale;
    panel->callbacks = std::move(callbacks);

    panel->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"PrismExtensionsPanel", L"Extensions", WS_POPUP | WS_BORDER,
        0, 0, 10, 10, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!panel->hwnd) {
        delete panel;
        return;
    }
    SetWindowLongPtrW(panel->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(panel));
    g_panel = panel;

    RECT rect = { };
    panel->view = Ref<WKViewRef>::adopt(WKViewCreate(rect, engine::pageConfiguration(), panel->hwnd));
    WKViewSetIsInWindow(panel->view.get(), true);
    panel->viewHwnd = WKViewGetWindow(panel->view.get());
    panel->page = WKViewGetPage(panel->view.get());
    engine::configurePage(panel->page, scale);

    WKPageUIClientV6 uiClient = { };
    uiClient.base.version = 6;
    uiClient.base.clientInfo = panel;
    uiClient.runJavaScriptPrompt = prompt;
    WKPageSetPageUIClient(panel->page, &uiClient.base);

    WKPageNavigationClientV3 navigationClient = { };
    navigationClient.base.version = 3;
    navigationClient.base.clientInfo = panel;
    navigationClient.decidePolicyForNavigationAction = decidePolicy;
    WKPageSetPageNavigationClient(panel->page, &navigationClient.base);

    engine::loadInternalPage(panel->page, L"extensions-panel");
    place(panel, kInitialHeight);
    ShowWindow(panel->hwnd, SW_SHOW);
    SetForegroundWindow(panel->hwnd);
}

} // namespace extensionspanel
