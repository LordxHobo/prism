// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "WebKitApi.h"

#include <string>
#include <vector>

// A read/write view of Prism's windows and tabs, for the extension APIs
// (chrome.tabs, chrome.windows). Private windows aren't in it (except where
// noted). Implemented in BrowserWindow.cpp.
namespace tabs {

struct Info {
    int id { -1 };
    int windowId { -1 };
    int index { 0 };
    bool active { false };
    bool loading { false };
    std::wstring url;
    std::wstring title;
    WKPageRef page { };
    bool isPrivate { false };
};

struct WindowInfo {
    int id { -1 };
    bool focused { false };
    RECT bounds { };
    int state { 0 }; // 0 normal, 1 minimized, 2 maximized
    std::vector<Info> tabs;
};

std::vector<WindowInfo> windows();
bool find(int tabId, Info&);
bool findByPage(WKPageRef, Info&);
// The same, private windows included (for Prism's own features).
bool findByPageAnyWindow(WKPageRef, Info&);
int lastFocusedWindow(); // -1 if there's no window

// Creates a tab (in `windowId`, or the last focused window if -1) and
// returns its id, or -1.
int create(int windowId, const std::wstring& url, bool active, int index = -1);
int createWindow(const std::vector<std::wstring>& urls, bool focused);
bool navigate(int tabId, const std::wstring& url);
bool activate(int tabId);
bool close(int tabId);
bool reload(int tabId, bool bypassCache);
bool goBack(int tabId);
bool goForward(int tabId);
// activate, focusWindow and windowHandle also take private windows.
bool focusWindow(int windowId);
bool closeWindow(int windowId);
// The browser window (HWND) a tab lives in, for dialogs and popups.
HWND windowHandle(int windowId);

} // namespace tabs
