// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <windows.h>

#include <functional>
#include <string>

// The panel that opens from the Extensions (puzzle) button: the extensions
// with buttons, grouped by what they can do on the current site, with a pin
// (show on the toolbar), a menu per extension, and "Manage extensions".
// A small popup window showing ui/extensions-panel.html, like Shields' panel.
namespace extensionspanel {

struct Callbacks {
    // {"site", "extensions": [{id, name, icon, access, pinned, options, badge}]}
    std::function<std::string()> list;
    std::function<void(const std::string& id)> openAction;
    std::function<void(const std::string& id, bool pinned)> setPinned;
    std::function<bool(const std::string& id)> hasOptions;
    // "options", "manage", "inspectPopup", "inspectBackground"
    std::function<void(const std::string& id, const std::string& choice)> menuChoice;
    std::function<void()> manage;
};

// Opens the panel with its top-right corner at `anchor` (screen pixels).
// Clicking the button while the panel is open closes it instead.
void toggle(HWND owner, POINT anchor, double scale, Callbacks);
void close();

} // namespace extensionspanel
