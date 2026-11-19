// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <functional>
#include <string>
#include <vector>
#include <windows.h>

// The list under the address bar while typing. It's a small native window
// that never takes the keyboard focus, so typing carries on in the address
// bar; the toolbar moves the selection with the arrow keys.
//
// Suggestions only come from Prism's own history and bookmarks: what's typed
// isn't sent anywhere until Enter is pressed.
namespace suggestions {

struct Item {
    std::wstring kind;  // "search", "url", "bookmark" or "history"
    std::wstring title;
    std::wstring url;   // where choosing it goes
    std::wstring detail; // second, dimmer text
};

// Items for what was typed. `go` is where Enter would go without choosing
// anything (an address or a search) and `isSearch` says which.
std::vector<Item> build(const std::wstring& text, const std::wstring& go, bool isSearch, const std::wstring& searchName);

struct Style {
    bool dark { true };
    COLORREF accent { RGB(0x8b, 0x7c, 0xf6) };
    double scale { 1 };
};

// Shows the list below `anchor` (screen pixels: the address bar's left,
// bottom and width). `pick` runs when an item is clicked.
void show(HWND owner, const RECT& anchor, const Style&, std::vector<Item> items, std::function<void(const Item&)> pick);
void select(int index); // -1 = none
void hide();
bool isVisible();

std::string itemsJSON(const std::vector<Item>&);

} // namespace suggestions
