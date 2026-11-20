// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "WebKitApi.h"

#include <functional>
#include <string>
#include <vector>

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

// ---- webRequest (from RequestFilter, for requests pages make) ----

struct WebRequestDetails {
    int tabId { -1 };
    int frameId { 0 };
    int parentFrameId { -1 };
    std::string url;
    std::string method;
    std::string type;        // webRequest type: "script", "image", "sub_frame"...
    std::string documentURL; // the frame that made the request
};

struct WebRequestDecision {
    bool cancel { false };
    std::string redirectURL;
};

// True if an extension listens to webRequest events (pages then ask about
// every request).
bool webRequestListening();

// Sends onBeforeRequest to the extensions listening for this request.
// `done` runs with the outcome: right away if no blocking listener cares,
// otherwise once they've all answered (or after about a second).
// `requestId` is set to the id extensions saw ("" if none did); end events
// for it go to webRequestEnded.
void webRequestBefore(const WebRequestDetails& details, std::string& requestId, std::function<void(WebRequestDecision)> done);
void webRequestEnded(const std::string& requestId, bool succeeded, const std::string& error);

// A tab starts loading a page (main_frame; extensions can't block it here).
void webRequestMainFrame(int tabId, const std::string& url);

// ---- Right-click menu items (contextMenus / menus) ----

// What was right-clicked.
struct MenuContext {
    std::string pageURL;
    std::string linkURL;
    std::string linkText;
    std::string srcURL;    // image, video or audio address
    std::string mediaType; // "image", "video", "audio" or ""
    std::string selectionText;
    bool editable { false };
};

struct MenuNode {
    std::string extensionId;
    std::string itemId; // the extension's id for it, as JSON (number or string)
    std::string title;
    std::string type;   // "normal", "checkbox", "radio", "separator"
    bool enabled { true };
    bool checked { false };
    std::vector<MenuNode> children;
};

// True if any extension has added menu items.
bool hasMenuItems();
// Items for a right-click on a page: per extension, its one item, or a
// submenu named after the extension when it has several.
std::vector<MenuNode> pageMenu(const MenuContext& context);
// Items for a right-click on an extension's toolbar button.
std::vector<MenuNode> actionMenu(const std::string& extensionId);
void menuItemClicked(const std::string& extensionId, const std::string& itemId, const MenuContext& context, int tabId);

// ---- Keyboard shortcuts (commands) ----

// Runs the extension shortcut for this key press, if there's one. `tabId`
// is the active tab. Returns true if a shortcut was used.
bool runShortcut(HWND window, int tabId, unsigned key, bool ctrl, bool alt, bool shift);

// Writes extension storage that's still waiting to be saved. Call at exit.
void flushStorage();

} // namespace extensions
