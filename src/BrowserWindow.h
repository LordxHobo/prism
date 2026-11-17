// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "WebKitApi.h"

#include <memory>
#include <string>
#include <vector>

class BrowserWindow;

struct Tab {
    BrowserWindow* window { };
    int id { };
    wk::Ref<WKViewRef> view;
    WKPageRef page { };
    HWND hwnd { };
    std::wstring title;
    std::wstring url;
    bool loading { false };
    double progress { 0 };
    bool crashed { false };
    std::wstring savedURL; // what the session file has for this tab
};

class BrowserWindow {
public:
    // Opens a new window. `startURLs` are opened as tabs (empty = new tab page);
    // `activeIndex` picks the selected one (-1 = the last).
    static BrowserWindow* open(const std::vector<std::wstring>& startURLs, int activeIndex = -1);

    // Reopens the windows and tabs from last time. `extraURLs` are added to
    // the first window. Returns false if there was nothing to restore.
    static bool restoreSession(const std::vector<std::wstring>& extraURLs);

    // Re-applies settings (theme, accent, tab size, ...) to every window.
    static void settingsChanged();

    // Runs `script` in every open tab showing prism://<name>.
    static void evaluateInInternalPages(const std::wstring& name, const std::string& script);
    static bool registerWindowClass(HINSTANCE);
    static BrowserWindow* fromHWND(HWND);
    static size_t windowCount();

    HWND hwnd() const { return m_hwnd; }

    // Keyboard shortcuts work no matter which part of the window has focus.
    // Returns true when the key was used by Prism.
    bool handleShortcut(const MSG&);

    Tab* newTab(const std::wstring& url, bool select, WKPageConfigurationRef = nullptr);
    void closeTab(int id);
    void selectTab(int id);
    void navigate(Tab*, const std::wstring& url);
    Tab* activeTab() const;

    void showToast(const std::wstring& text);

    // Opens prism://<name>, or switches to it if a tab already shows it.
    void openInternalPage(const std::wstring& name);

private:
    BrowserWindow() = default;
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleMessage(UINT, WPARAM, LPARAM);

    void createToolbar();
    void layout();
    double scaleFactor() const;
    int toolbarHeight() const;
    int toolbarCSSHeight() const;
    void applyScaleFactor();
    void updateTheme();

    Tab* findTab(int id) const;
    Tab* findTab(WKPageRef) const;
    int indexOf(const Tab*) const;
    void selectAdjacentTab(int delta);
    void reopenClosedTab();
    void focusAddressBar();
    void focusContent();
    void showMenu(int x, int y);
    void zoom(double factor);
    void showDevTools();
    void handleToolbarCommand(const std::wstring& command, const std::wstring& argument);
    void tabChanged(Tab*);
    void pushState();
    void runToolbarScript(const std::string& script);
    void updateTitle();
    void onDestroy();
    std::string handleSettingsCommand(const std::wstring& command, const std::wstring& argument);
    static void scheduleSessionSave();
    static void saveSession();
    static std::wstring sessionURL(const Tab*);

    // WebKit callbacks
    static void tabStateChanged(const void*);
    static WKPageRef createNewPage(WKPageRef, WKPageConfigurationRef, WKNavigationActionRef, WKWindowFeaturesRef, const void*);
    static void closePage(WKPageRef, const void*);
    static void runJavaScriptAlert(WKPageRef, WKStringRef, WKFrameRef, WKSecurityOriginRef, WKPageRunJavaScriptAlertResultListenerRef, const void*);
    static void runJavaScriptConfirm(WKPageRef, WKStringRef, WKFrameRef, WKSecurityOriginRef, WKPageRunJavaScriptConfirmResultListenerRef, const void*);
    static void runJavaScriptPrompt(WKPageRef, WKStringRef, WKStringRef, WKFrameRef, WKSecurityOriginRef, WKPageRunJavaScriptPromptResultListenerRef, const void*);
    static void decidePolicyForNavigationAction(WKPageRef, WKNavigationActionRef, WKFramePolicyListenerRef, WKTypeRef, const void*);
    static void decidePolicyForNavigationResponse(WKPageRef, WKNavigationResponseRef, WKFramePolicyListenerRef, WKTypeRef, const void*);
    static void didFailProvisionalNavigation(WKPageRef, WKNavigationRef, WKErrorRef, WKTypeRef, const void*);
    static void webProcessDidTerminate(WKPageRef, uint32_t, const void*);
    static void navigationActionDidBecomeDownload(WKPageRef, WKNavigationActionRef, WKDownloadRef, const void*);
    static void navigationResponseDidBecomeDownload(WKPageRef, WKNavigationResponseRef, WKDownloadRef, const void*);
    static void contextMenuDidCreateDownload(WKPageRef, WKDownloadRef, const void*);
    static void toolbarDecidePolicyForNavigationAction(WKPageRef, WKNavigationActionRef, WKFramePolicyListenerRef, WKTypeRef, const void*);
    static void toolbarPrompt(WKPageRef, WKStringRef, WKStringRef, WKFrameRef, WKSecurityOriginRef, WKPageRunJavaScriptPromptResultListenerRef, const void*);
    void attachClients(Tab*);
    void startDownload(WKDownloadRef);

    HWND m_hwnd { };
    wk::Ref<WKViewRef> m_toolbarView;
    WKPageRef m_toolbarPage { };
    HWND m_toolbarHwnd { };
    bool m_toolbarReady { false };
    std::vector<std::unique_ptr<Tab>> m_tabs;
    std::vector<std::wstring> m_closedURLs;
    int m_activeTabId { 0 };
    bool m_darkTheme { true };
    std::vector<std::wstring> m_pendingStartURLs;
    int m_pendingActiveIndex { -1 };
    uint64_t m_lastBlockedCount { 0 };
};
