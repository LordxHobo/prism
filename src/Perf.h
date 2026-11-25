// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <string>

// Finding what makes pages slow. Prism's main thread answers every page's
// request checks, extension messages and Shields questions, so when it's
// busy, pages wait. A watchdog thread notices when it hasn't been free for a
// while and notes what it was doing (the innermost Scope); slow extension
// webRequest answers are noted too. Shown in Settings › About › Performance.
namespace perf {

void start(); // on the main thread, once

// Marks what the main thread is doing, for the watchdog. Cheap.
class Scope {
public:
    explicit Scope(const char* what, const std::string& detail = { });
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    std::string m_previous;
};

// An extension's webRequest listener made a page's request wait.
void noteWebRequest(const std::string& extensionName, unsigned long long waitedMs, bool timedOut, const std::string& url);

// A problem worth seeing in the log (e.g. the toolbar's script failed).
// The same text is noted once a minute at most.
void note(const std::string& text);

// {"lines": [...], "stalls": n, "longest": ms}
std::string logJSON();
void clear();

} // namespace perf
