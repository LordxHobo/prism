// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "WebKitApi.h"

#include <windows.h>
#include <string>

// ACI (Adaptive Cognitive Interface): reading views for any page (Focus,
// Dyslexia-friendly, Dense). The page side is ui/aci.js, run in a world of
// Prism's own (PrismBundle) or, without it, in the page. This side keeps the
// settings (prism.ini Aci* keys, aci-sites.json for sites with their own
// view), the fonts, and answers the view's questions.
namespace aci {

void initialize();
bool canRun(const std::wstring& url); // http(s) pages
// The toolbar button and F9: opens the last used view, or closes it.
void toggle(WKPageRef page, const std::wstring& url, HWND owner);
// After a page loads: opens the site's view (or the default one) by itself.
void pageLoaded(WKPageRef page, const std::wstring& url, HWND owner);

// From the view: {"op": ...}; answers JSON.
std::string handleMessage(WKPageRef page, const std::string& text);
// The same from a view in the page's world: checks the per-start key first.
std::string handleFallbackMessage(WKPageRef page, const std::string& text);
// prism://settings: aci.get, aci.set "<key>\n<value>", aci.removeSite "<site>", aci.fontFile, aci.clearFontFile.
std::string handleCommand(const std::wstring& command, const std::wstring& argument, HWND owner);

} // namespace aci
