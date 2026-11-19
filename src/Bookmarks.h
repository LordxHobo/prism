// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

// Bookmarks: %LOCALAPPDATA%\Prism\bookmarks.json. Two top folders, the
// bookmarks bar (shown under the address bar) and "Other bookmarks".
namespace bookmarks {

constexpr long long kBarId = 1;
constexpr long long kOtherId = 2;

struct Node {
    long long id { 0 };
    bool folder { false };
    std::wstring title;
    std::wstring url;
    long long added { 0 };
    Node* parent { nullptr };
    std::vector<std::unique_ptr<Node>> children;
};

void load();

// Called after every change (to redraw the bookmarks bar and pages).
void setChangeListener(std::function<void()>);

const Node* find(long long id);
// The bookmark for this address, if there is one.
const Node* findURL(const std::wstring& url);

// Adds a bookmark at the end of the bookmarks bar. Returns its id.
long long add(const std::wstring& url, const std::wstring& title, long long parentId = kBarId);
void remove(long long id);
void rename(long long id, const std::wstring& title);

// The bookmarks bar for the toolbar: [{id, title, url, folder}].
std::string barJSON();

struct Match {
    std::wstring url;
    std::wstring title;
};
// Bookmarks whose title or address contains every word of `text`.
std::vector<Match> search(const std::wstring& text, size_t limit);

// For prism://bookmarks.
std::string handleCommand(const std::wstring& command, const std::wstring& argument, HWND owner);

} // namespace bookmarks
