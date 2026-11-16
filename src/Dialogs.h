// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <windows.h>
#include <optional>
#include <string>

namespace dialogs {

// A small modal text-input dialog (for window.prompt()).
// Returns nothing if the user cancelled.
std::optional<std::wstring> prompt(HWND owner, const std::wstring& title, const std::wstring& message, const std::wstring& defaultValue);

} // namespace dialogs
