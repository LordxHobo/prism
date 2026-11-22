// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <windows.h>
#include <string>

// Site icons in native menus (bookmark folders): the saved icon for a page,
// as a small 32-bit bitmap with alpha, or nullptr (no icon yet, or SVG,
// which Windows can't decode). Bitmaps are kept for the session.
namespace menuicons {

HBITMAP forPage(const std::wstring& pageURL);
// Sets a menu item's bitmap (by command id) if the page has an icon.
void apply(HMENU menu, UINT commandId, const std::wstring& pageURL);

} // namespace menuicons
