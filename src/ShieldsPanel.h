// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <windows.h>

#include <functional>
#include <string>

// The panel that opens from the shield in the address bar: Prism Shields for
// the current site (on/off, what was blocked). A small popup window showing
// ui/shields-panel.html; it closes when you click anywhere else.
namespace shieldspanel {

struct Callbacks {
    std::function<void()> reloadTab;
    std::function<void()> openSettings;
    // JSON of what was blocked on the tab's page ({blocked, hosts, filtering}).
    std::function<std::string()> pageStats;
};

// Opens the panel with its top-right corner at `anchor` (screen pixels),
// for the page at `url`. Clicking the shield while the panel is open closes
// it instead.
void toggle(HWND owner, POINT anchor, double scale, const std::wstring& url, Callbacks);

// Closes the panel if it's open (e.g. the tab changed).
void close();

} // namespace shieldspanel
