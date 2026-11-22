// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <functional>
#include <string>

// Site icons (favicons) for tabs, the bookmarks bar, History and Bookmarks.
//
// When a page has loaded, Prism asks it which icon it lists (or uses the
// site's /favicon.ico) and downloads it itself, without cookies, into
// %LOCALAPPDATA%\Prism\Icons\, one per site. Icons are fetched again after
// a week.
namespace icons {

// Changed icons are announced through this (to redraw toolbars and pages).
void setChangeListener(std::function<void()>);

// The script that asks a page for its icon; it evaluates to an address.
const std::string& pageScript();

// `iconURL` is what pageScript() returned on the page at `pageURL`.
void pageLoaded(const std::wstring& pageURL, const std::wstring& iconURL);

// For pages Prism hasn't visited yet (bookmarks): looks up the site's icon
// in its front page (or /favicon.ico) in the background, one site at a time.
// Does nothing when the site already has a fresh icon.
void prefetch(const std::wstring& pageURL);

// file:/// URL of the saved icon for this page's site, or "" if there's none.
std::wstring fileURLFor(const std::wstring& pageURL);

// The saved icon file itself (for native menus), or "".
std::wstring filePathFor(const std::wstring& pageURL);

// Deletes every saved icon (with browsing history).
void clear();

} // namespace icons
