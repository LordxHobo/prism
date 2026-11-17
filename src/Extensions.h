// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "WebKitApi.h"

#include <functional>
#include <string>

// Browser extensions (Chrome Web Store and Firefox Add-ons).
//
// What works in this version:
// - Installing from a store link, a .crx/.xpi/.zip file, or an unpacked
//   folder, with a permissions check before anything is installed.
// - Content scripts and styles (manifest "content_scripts"), with the
//   chrome.*/browser.* APIs they use most: storage, i18n, runtime basics.
// - Ad and tracker blocking from declarativeNetRequest rulesets (uBlock
//   Origin Lite and similar), applied per domain by Blocker.h.
// Background scripts, popups and options pages come in a later version; the
// Extensions page shows per extension what does and doesn't work yet.
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

// True for links to an extension in the Chrome Web Store or Firefox Add-ons
// (used to offer "Add to Prism" in the toolbar).
bool isStoreURL(const std::wstring& url);

// Remembers a store link to offer on the Extensions page when it opens.
void queueInstall(const std::wstring& url);

} // namespace extensions
