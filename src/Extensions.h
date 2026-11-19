// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "WebKitApi.h"

#include <functional>
#include <string>

// Browser extensions (Chrome Web Store and Firefox Add-ons).
//
// What works:
// - Installing from a store link, a .crx/.xpi/.zip file, or an unpacked
//   folder, with a permissions check before anything is installed.
// - Content scripts and styles (manifest "content_scripts").
// - Background pages and service workers, toolbar buttons with popups and
//   badges, options pages, and messaging between all parts of an
//   extension (ExtensionHost.cpp).
// - The chrome.*/browser.* APIs those use most: runtime, storage, tabs,
//   windows, action, scripting, alarms, i18n, permissions.
// - Ad and tracker blocking from declarativeNetRequest rulesets (uBlock
//   Origin Lite and similar), applied per domain by Blocker.h.
// The Extensions page shows per extension what doesn't work yet.
namespace extensions {

// Loads installed extensions and applies them. Call once after
// engine::initialize(). `notify` receives events for prism://extensions
// (as a JSON string) and must forward them to any open extensions page.
void initialize(std::function<void(const std::string& eventJSON)> notify);

// The script message handler that content scripts talk to.
void handleScriptMessage(WKScriptMessageRef, WKCompletionListenerRef reply);

// Commands from prism://extensions (see ui/extensions.js). `owner` is the
// window used for file pickers and dialogs. Returns JSON.
std::string handleCommand(const std::wstring& command, const std::wstring& argument, HWND owner);

// The script message handler extension pages (background, popup, options)
// talk to; see ui/extension-page.js.
void handlePageMessage(WKScriptMessageRef, WKCompletionListenerRef reply);

// Toolbar buttons for the tab `tabId` (JSON array), and what happens when
// one is clicked or right-clicked (position in the toolbar's CSS pixels;
// -1 for "under the toolbar's right end").
std::string actionsJSON(int tabId);
void actionClicked(HWND window, int tabId, const std::string& extensionId, int buttonRight, int buttonBottom);
void showActionMenu(HWND window, const std::string& extensionId, int x, int y);
void openOptionsPage(const std::string& extensionId, int windowId);

// Extension pages live in files; the address bar shows them as
// chrome-extension://<id>/page.html. resolveExtensionURL() goes the other
// way (empty if it isn't an installed extension's page).
std::wstring displayURL(const std::wstring& url);
std::wstring resolveExtensionURL(const std::wstring& url);

// From BrowserWindow, for chrome.tabs / chrome.windows events.
void tabCreated(int tabId);
void tabUpdated(int tabId);
void tabActivated(int tabId, int windowId);
void tabRemoved(int tabId, int windowId, bool windowClosing);
void windowCreated(int windowId);
void windowRemoved(int windowId);
void windowFocused(int windowId);

// True for links to an extension in the Chrome Web Store or Firefox Add-ons
// (used to offer "Add to Prism" in the toolbar).
bool isStoreURL(const std::wstring& url);

// Remembers a store link to offer on the Extensions page when it opens.
void queueInstall(const std::wstring& url);

// Writes extension storage that's still waiting to be saved. Call at exit.
void flushStorage();

} // namespace extensions
