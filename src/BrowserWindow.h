// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "WebKitApi.h"

#include <memory>
#include <string>
#include <vector>

class BrowserWindow;
namespace tabs { struct Info; }

// A site's question waiting for an answer in the bar under the address bar.
struct PermissionPrompt {
    std::wstring kind;   // "location" or "notifications"
    std::wstring origin; // "https://example.com"
    wk::Ref<WKTypeRef> request;
};

// A named, colored group of tabs. A group's tabs are always next to each
// other in the window's tab list.
struct TabGroup {
    int id { 0 };
    std::wstring name;
    std::wstring color; // #rrggbb
    bool collapsed { false };
};

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
    bool extensionPages { false }; // made with engine::extensionPageConfiguration()
    std::wstring savedURL; // what the session file has for this tab

    // Find in page (Ctrl+F)
    bool findOpen { false };
    std::wstring findQuery;
    long long findMatches { -1 }; // -1 = not searched yet
    long long findIndex { 0 };    // which match is selected, from 1
    int findStep { 0 };           // what the search in progress does: 0 new, 1 next, -1 previous

    std::vector<PermissionPrompt> prompts;

    // HTTPS upgrades: the http address we turned into https (until the page
    // commits or fails), and when, to notice sites that send us back.
    std::wstring upgradedFrom;
    std::wstring upgradedTo;
    unsigned long long upgradedAt { 0 };

    int groupId { 0 };          // 0 = not in a group
    unsigned long long lastActive { 0 }; // when it was last the selected tab
};

class BrowserWindow {
public:
    // Opens a new window. `startURLs` are opened as tabs (empty = new tab page);
    // `activeIndex` picks the selected one (-1 = the last).
    static BrowserWindow* open(const std::vector<std::wstring>& startURLs, int activeIndex = -1);
    // The same, with tab groups: `tabGroups[i]` is the index into `groups`
    // (plus one; 0 = no group) of tab i.
    static BrowserWindow* open(const std::vector<std::wstring>& startURLs, int activeIndex,
        const std::vector<TabGroup>& groups, const std::vector<int>& tabGroups);

    // Reopens the windows and tabs from last time. `extraURLs` are added to
    // the first window. Returns false if there was nothing to restore.
    static bool restoreSession(const std::vector<std::wstring>& extraURLs);

    // Re-applies settings (theme, accent, tab size, ...) to every window.
    static void settingsChanged();
    static void refreshToolbars();
    // A tab's "Save password?" offer appeared or went away (Passwords.h).
    static void passwordOfferChanged(int tabId);
    // Remembers maximized or the size and place, for the next window (prism.ini WindowPlacement).
    void savePlacement();
    // Sends your interface CSS (usercss::interfaceCSS) to every toolbar.
    static void applyInterfaceCSS();

    // Runs `script` in every open tab showing prism://<name>.
    static void evaluateInInternalPages(const std::wstring& name, const std::string& script);
    static bool registerWindowClass(HINSTANCE);
    static BrowserWindow* fromHWND(HWND);
    static size_t windowCount();

    HWND hwnd() const { return m_hwnd; }
    int id() const { return m_id; }

    // Keyboard shortcuts work no matter which part of the window has focus.
    // Returns true when the key was used by Prism.
    bool handleShortcut(const MSG&);
    bool handleBrowserShortcut(const MSG&); // Prism's own shortcuts (handleShortcut adds extensions')

    Tab* newTab(const std::wstring& url, bool select, WKPageConfigurationRef = nullptr);
    void closeTab(int id);
    void selectTab(int id);
    void navigate(Tab*, const std::wstring& url);
    Tab* activeTab() const;

    void showToast(const std::wstring& text);

    // Opens prism://<name>, or switches to it if a tab already shows it.
    void openInternalPage(const std::wstring& name, const std::wstring& section = L"");

    // For the extension tab APIs (Tabs.h).
    static const std::vector<BrowserWindow*>& all();
    const std::vector<std::unique_ptr<Tab>>& tabList() const { return m_tabs; }
    int activeTabId() const { return m_activeTabId; }
    void moveTab(int id, int index);
    void anchorForExtension(int buttonRight, int buttonBottom, POINT& screen) const;
    double scale() const { return scaleFactor(); }

private:
    BrowserWindow() = default;
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleMessage(UINT, WPARAM, LPARAM);

    void createToolbar();
    void createSidebar();
    void destroySidebar();
    int sidebarWidth() const; // device pixels, 0 without vertical tabs
    void layout();
    double scaleFactor() const;
    int toolbarHeight() const;
    int toolbarCSSHeight() const;
    void applyScaleFactor();
    void updateTheme();
    void updateIcon(); // the window and taskbar icon

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
    static void didCommitNavigation(WKPageRef, WKNavigationRef, WKTypeRef, const void*);
    static void didSameDocumentNavigation(WKPageRef, WKNavigationRef, WKSameDocumentNavigationType, WKTypeRef, const void*);
    static void decidePolicyForGeolocation(WKPageRef, WKFrameRef, WKSecurityOriginRef, WKGeolocationPermissionRequestRef, const void*);
    static void decidePolicyForNotifications(WKPageRef, WKSecurityOriginRef, WKNotificationPermissionRequestRef, const void*);
    static void printFrame(WKPageRef, WKFrameRef, const void*);
    static void didFindString(WKPageRef, WKStringRef, unsigned, const void*);
    static void didFailToFindString(WKPageRef, WKStringRef, const void*);
    void attachClients(Tab*);
    static void sidebarDecidePolicyForNavigationAction(WKPageRef, WKNavigationActionRef, WKFramePolicyListenerRef, WKTypeRef, const void*);
    void requestIcon(Tab*);
    static void showLoadError(Tab*, const std::wstring& failingURL, const std::wstring& description, int code);

    // Tab groups
    TabGroup* findGroup(int id);
    void normalizeGroups();
    int createGroup(Tab*);
    void addTabToGroup(Tab*, int groupId);
    void removeTabFromGroup(Tab*);
    void closeGroup(int id);
    void ungroup(int id);
    void setGroupCollapsed(int id, bool collapsed);
    void moveTabTo(int tabId, int index, int groupId);
    // Groups and ungrouped tabs, in order ("units" of the Vivaldi-style
    // strip): a group id (> 0) or minus a tab id.
    std::vector<int> units() const;
    int unitOf(const Tab*) const;
    std::vector<int> displayedUnits() const;
    void selectUnit(int unit, bool cycle);
    void moveUnit(int unit, int index);
    void showTabMenu(int tabId, POINT screen);
    void showGroupMenu(int groupId, POINT screen);
    void showExtensionsMenu(POINT screen);
    void showExtensionsPanel(const std::wstring& coordinates); // the puzzle button
    void showHiddenActionMenu(const std::string& extensionId, int x, int y);
    void closeTabs(const std::vector<int>& ids);

    // Find in page
    void openFind();
    void closeFind(Tab*);
    void findInPage(Tab*, int step);

    // Permission prompts
    void queuePrompt(Tab*, const std::wstring& kind, const std::wstring& origin, WKTypeRef request);
    void answerPrompt(Tab*, const std::wstring& answer);
    void denyPrompts(Tab*);

    // Address-bar suggestions
    std::string suggest(const std::wstring& argument);

    // Bookmarks
    POINT toolbarPoint(const std::wstring& coordinates) const;
    void bookmarkButton(const std::wstring& coordinates);
    void openBookmarkURL(const std::wstring& url, const std::wstring& mode);
    void showBookmarkFolder(long long folderId, POINT screen);
    void showBookmarkMenu(long long id, POINT screen);
    void showBookmarkOverflow(const std::vector<long long>& ids, POINT screen);
    void toggleBookmarksBar();
    void startDownload(WKDownloadRef);

    HWND m_hwnd { };
    int m_id { 0 };
    wk::Ref<WKViewRef> m_toolbarView;
    WKPageRef m_toolbarPage { };
    HWND m_toolbarHwnd { };
    bool m_toolbarReady { false };
    wk::Ref<WKViewRef> m_sidebarView;
    WKPageRef m_sidebarPage { };
    HWND m_sidebarHwnd { };
    bool m_sidebarReady { false };
    std::vector<TabGroup> m_groups;
    std::vector<TabGroup> m_pendingGroups;
    std::vector<int> m_pendingTabGroups;
    std::vector<std::unique_ptr<Tab>> m_tabs;
    std::vector<std::wstring> m_closedURLs;
    int m_activeTabId { 0 };
    bool m_darkTheme { true };
    std::vector<std::wstring> m_pendingStartURLs;
    int m_pendingActiveIndex { -1 };
    uint64_t m_lastBlockedCount { 0 };
    uint32_t m_lastBlockedHere { 0 };
};
