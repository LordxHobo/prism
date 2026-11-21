// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "WebKitApi.h"

#include <windows.h>
#include <string>

// Prism Passwords in the browser: the page script (ui/passwords-runtime.js),
// "Save password?" offers, the login menu on login fields, and
// prism://passwords. The vault itself is PasswordVault.h.
namespace passwords {

// After engine::initialize(): opens the vault with Windows if that's on,
// adds the page script and starts the auto-lock timer.
void initialize();

// Messages from the page script ("prismPasswords").
void handleScriptMessage(WKScriptMessageRef, WKCompletionListenerRef reply);

// A "Save password?" offer for a tab's info bar.
struct Offer {
    std::wstring site;
    std::wstring username;
    bool update { false };
};
bool offerFor(int tabId, Offer& out);
// "allow" (save), "block" (never for this site) or "dismiss".
void answerOffer(int tabId, const std::wstring& answer, HWND window);
void tabNavigated(int tabId, const std::wstring& url);
void tabClosed(int tabId);

// prism://passwords commands; answers JSON.
std::string handleCommand(const std::wstring& command, const std::wstring& argument, HWND window);

} // namespace passwords
