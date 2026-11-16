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

// Accent color as #rrggbb.
std::wstring accent();

// Smaller tab strip and toolbar.
bool compactTabs();

// Block all third-party cookies (strict) or allow them for sites you've
// visited directly (standard).
bool strictCookies();

// Delete cookies, site data and caches when Prism closes.
bool clearOnExit();

// All settings as a JSON object (UTF-8), for the settings page.
std::string toJSON();

// Changes one setting. Returns false (and changes nothing) if the key is
// unknown or the value isn't allowed.
bool set(const std::wstring& key, const std::wstring& value);

} // namespace settings
