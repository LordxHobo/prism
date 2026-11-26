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

// The one browsing context every page belongs to.
WKContextRef context();

// Restarts WebKit's network process (it starts again with the next load),
// e.g. so it reads certificates Windows has just added.
void restartNetworkProcess();

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

// For extension pages (background, popups, options tabs): like
// pageConfiguration(), but WebKit doesn't slow down their timers while
// they're hidden. Tabs made with it only ever show extension pages.
WKPageConfigurationRef extensionPageConfiguration();
WKPageConfigurationRef pageConfiguration();

// Private windows: pages whose cookies, storage and cache live only in
// memory, in a browsing context of their own (its web processes never get
// extension content scripts). Every private window shares one such store
// until the last one closes (releasePrivateData()). Returns null if this
// WebKit can't make one.
WKPageConfigurationRef privatePageConfiguration();
bool isPrivateContext(WKContextRef);
// The last private window closed: forget everything its pages stored.
void releasePrivateData();
// The private browsing context, if a private window was ever opened.
WKContextRef privateContextIfCreated();
// "Extensions in private windows" may have changed: gives private pages
// opened from now on extension content scripts, or takes them away.
void privateExtensionsChanged();
// What PrismBundle runs in new documents (extensions' content scripts and
// Prism's translator), for a web process of the normal or private context.
std::string contentScriptsPayload(bool privateContext);
// Sends it again to both contexts (for documents loaded from now on).
void sendContentScripts();

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
