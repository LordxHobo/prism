// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "WebKitApi.h"

#include <windows.h>
#include <functional>
#include <string>

// Prism's translator: pages in another language are translated on this
// computer with Bergamot, the engine of Firefox Translations. The page side
// is ui/translate-page.js (in a world of Prism's own, see PrismBundle); the
// engine runs in a hidden page (ui/translate/engine.html) served by Prism's
// local server. The engine and the language models are downloaded from
// Mozilla's servers the first time they're needed, checked (SHA-256) and
// kept in <data>\translations.
namespace translate {

void initialize();
void shutdown();

// "prism-translate\t<length>\n<source>" for PrismBundle's content script
// payload, added to `payload` ("<count>\n..."); unchanged when translating
// is off.
std::string withTranslator(const std::string& payload);

// From the page script: answers the JSON for {"ok":true,"result":...}.
void handleMessage(WKPageRef page, bool mainFrame, const std::string& text, std::function<void(const std::string&)> reply);

// The address bar button: null, or {"name", "status", ...}.
std::string stateJSON(WKPageRef page);
void pageCommitted(WKPageRef page);
void pageClosed(WKPageRef page);
// The button's menu at `point` (screen). True if Settings should open.
bool showMenu(WKPageRef page, HWND owner, POINT point);

// prism://settings: translate.<command>.
std::string handleCommand(const std::wstring& command, const std::wstring& argument);

} // namespace translate
