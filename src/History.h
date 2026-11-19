// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <vector>

// Browsing history: %LOCALAPPDATA%\Prism\history.tsv, kept for 90 days.
// Only websites and the user's own files are recorded (not Prism's pages,
// extension pages or error pages), and nothing at all while the "Save
// history" setting is off.
namespace history {

void load();

// A page was opened (a new entry in the tab's back/forward list).
void recordVisit(const std::wstring& url, const std::wstring& title);
// The page's title arrived or changed.
void updateTitle(const std::wstring& url, const std::wstring& title);

struct Match {
    std::wstring url;
    std::wstring title;
    double score { 0 };
};
// Pages whose address or title contains every word of `text`, best first
// (visited often and recently, address starting with what was typed).
std::vector<Match> search(const std::wstring& text, size_t limit);

// Deletes everything.
void clear();

// For prism://history ("query", "delete", "deleteURL", "clear").
std::string handleCommand(const std::wstring& command, const std::wstring& argument);

} // namespace history
