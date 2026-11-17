// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "Json.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Network-level blocking for content-blocking extensions (uBlock Origin Lite
// and other declarativeNetRequest extensions).
//
// WebKit's own content blocker is switched off in its Windows build, so Prism
// runs a tiny filtering proxy on 127.0.0.1 instead. WebKit sends its traffic
// through it; requests to blocked domains are refused before any connection
// to them is made. HTTPS stays end-to-end encrypted: the proxy only sees which
// host a connection is for (never the page or its content), so blocking works
// per domain, like DNS-level blockers.
namespace blocker {

struct Lists {
    // Domain -> index into `owners` (the extension that blocks it).
    std::unordered_map<std::string, uint16_t> blocked;
    std::unordered_set<std::string> allowed;
    std::vector<std::string> owners;
    bool empty() const { return blocked.empty(); }
};

// Collects the domains a declarativeNetRequest ruleset blocks or allows.
// Only rules that apply to a whole domain everywhere are used; rules that
// depend on the page, the path or the resource type can't be applied by a
// domain-level blocker and are skipped. Returns the number of rules used.
size_t collectDomains(const json::Value& rules, std::vector<std::string>& blocked, std::vector<std::string>& allowed);

// Swaps in new lists (thread-safe). Empty lists turn blocking off.
void setLists(std::shared_ptr<const Lists>);

// True if `host` (or a parent domain) is blocked and not allowed.
// `owner` receives the name of the extension that blocks it.
bool isBlocked(const std::string& host, std::string* owner = nullptr);

// Starts the proxy (once) and returns its port, or 0 if it couldn't start.
uint16_t startProxy();

// How many connections were blocked since Prism started.
uint64_t blockedCount();
void recordBlocked(); // used by the proxy

} // namespace blocker
