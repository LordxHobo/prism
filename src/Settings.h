// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <string>

// Prism's settings, stored in %LOCALAPPDATA%\Prism\prism.ini.
// The settings page (prism://settings) reads and changes them through
// settings::toJSON() and settings::set().
namespace settings {

// Address-bar search. Returns a URL template where %s is the query.
std::wstring searchURL();

// "newtab" or "restore" (reopen the tabs from last time).
bool restoreSession();

// Where downloads are saved (falls back to the Downloads folder).
std::wstring downloadDirectory();

// "system", "dark" or "light".
std::wstring theme();

// Accent color as #rrggbb: the user's own colour, or the theme's (Ocean on
// dark, Amber on light; follows engine::darkTheme()).
std::wstring accent();
// The user's own accent colour, or "" when it follows the theme.
std::wstring customAccent();

// Smaller tab strip and toolbar.
bool compactTabs();

// Block all third-party cookies (strict) or allow them for sites you've
// visited directly (standard).
bool strictCookies();

// Delete cookies, site data and caches when Prism closes.
bool clearOnExit();

// Keep a list of visited pages (prism://history, address-bar suggestions).
bool saveHistory();

// Show the bookmarks bar under the address bar (Ctrl+Shift+B).
bool showBookmarksBar();

// What happens when a site asks for something ("ask" or "block"):
// "location" and "notifications".
std::wstring permissionDefault(const std::wstring& permission);

// Tabs across the top ("horizontal") or down the left side ("vertical").
bool verticalTabs();
// Vertical tabs narrowed to their icons.
bool verticalTabsCollapsed();
// How tab groups look with tabs on top: "chrome" (one row) or "vivaldi"
// (groups on top, the selected group's tabs below).
std::wstring groupStyle();
// Color palette for Prism's interface: "default", "midnight", "nord",
// "forest", "rose" or "sepia".
std::wstring palette();
bool showHomeButton();
// Extensions whose buttons are hidden from the toolbar (still in the
// Extensions menu).
bool isActionHidden(const std::string& extensionId);
void setActionHidden(const std::string& extensionId, bool hidden);
std::string hiddenActionsJSON();
// New tab page: "top" (most visited), "bookmarks" (the bookmarks bar) or "off".
std::wstring newTabShortcuts();

// All settings as a JSON object (UTF-8), for the settings page.
std::string toJSON();

// Changes one setting. Returns false (and changes nothing) if the key is
// unknown or the value isn't allowed.
bool set(const std::wstring& key, const std::wstring& value);

} // namespace settings
