// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "WebKitApi.h"

#include <functional>
#include <string>
#include <vector>

// Shared WebKit state for the whole app: one browsing context, one set of
// preferences and one on-disk profile, used by every window and tab.
namespace engine {

// Call before initialize(): wipes the profile if "clear on exit" is on.
void deleteProfileIfRequested();

bool initialize(std::wstring& error);

// Where extension content scripts are registered (shared by all pages).
WKUserContentControllerRef userContentController();

// Sends WebKit's traffic through Prism's blocking proxy while anything
// (Prism Shields or an extension) has domains to block; call after changing
// the blocker's lists.
void updateBlockingProxy();
bool blockingProxyActive();

// User scripts, in named groups ("extensions", "shields"). setUserScripts
// replaces a group (and re-registers everything); addUserScript appends one
// script to a group without touching the others. Scripts apply to pages
// loaded afterwards.
void setUserScripts(const std::string& group, std::vector<std::string> sources);
void addUserScript(const std::string& group, const std::string& source);

// Applies the third-party cookie setting (call again after it changes).
void applyCookiePolicy();

// Deletes browsing data; `done` runs on the main thread when finished.
void clearBrowsingData(bool cookiesAndSiteData, bool cache, std::function<void()> done);

WKPageConfigurationRef pageConfiguration();

// Applies per-page privacy settings (user agent, scale). Call on every new page.
void configurePage(WKPageRef, double scaleFactor);

// Internal pages live in the "ui" folder next to Prism.exe and are shown in
// the address bar as prism://<name>.
std::wstring uiDirectoryURL();                   // file:///.../ui/
std::wstring internalPageURL(const std::wstring& name, const std::wstring& query = L"");
void loadInternalPage(WKPageRef, const std::wstring& name, const std::wstring& query = L"");
bool isInternalURL(const std::wstring& url);
// True if `url` is the internal page `name` (e.g. "newtab"), ignoring query.
bool isInternalPage(const std::wstring& url, const std::wstring& name);

// Internal pages are told which theme to use (?theme=dark|light).
void setDarkTheme(bool);
bool darkTheme();

// The search engine used for address-bar searches. %s is replaced by the query.
std::wstring searchURLTemplate();

} // namespace engine
