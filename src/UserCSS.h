// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <functional>
#include <string>

// Your own CSS: one block for Prism's interface (tab strip, toolbar,
// sidebar) and rules for websites, each for a list of sites.
//   %LOCALAPPDATA%\Prism\interface.css
//   %LOCALAPPDATA%\Prism\site-styles.json  [{"sites": "...", "css": "...", "enabled": true}]
// Site rules are added to pages as a user script (as they start loading),
// so they apply to pages loaded afterwards.
namespace usercss {

void initialize(std::function<void()> interfaceChanged);

const std::string& interfaceCSS();

// For prism://settings: "get", "setInterface", "setSites".
std::string handleCommand(const std::wstring& command, const std::wstring& argument);

} // namespace usercss
