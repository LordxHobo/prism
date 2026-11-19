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

// Windows' Open and Save dialogs for one file. `filterName` is shown in the
// file type list ("Bookmark files (*.html)"), `pattern` filters ("*.html;*.htm").
std::optional<std::wstring> openFile(HWND owner, const std::wstring& title, const std::wstring& filterName, const std::wstring& pattern);
std::optional<std::wstring> saveFile(HWND owner, const std::wstring& title, const std::wstring& fileName, const std::wstring& filterName, const std::wstring& pattern);

} // namespace dialogs
