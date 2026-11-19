// Prism — declarativeNetRequest rules as adblock filters.
//
// Content-blocking extensions (uBlock Origin Lite and others) describe what
// to block with Chrome's declarativeNetRequest rules. Their urlFilter syntax
// is the adblock filter syntax ("||ads.example^", "|https://", "*"), so Prism
// turns each rule into a filter line and lets Prism Shields' engine
// (adblock-rust) match them against every request a page makes.
//
// Not converted (counted as skipped): redirects, header changes, scheme
// upgrades, rules that need both a request domain and a URL pattern,
// excluded request domains, request methods, tab IDs and response headers.
// Rule priorities aren't kept: an allow rule always wins over a block rule,
// as in adblock lists.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "Json.h"

#include <cstddef>
#include <string>

namespace dnr {

struct Conversion {
    std::string filters; // one filter per line
    size_t converted = 0;
    size_t skipped = 0;
};

// Appends the filters for a ruleset (a JSON array of rules) to `out`.
void convert(const json::Value& rules, Conversion& out);

// One rule as a filter line ("" if it can't be converted). Exposed for tests.
std::string ruleToFilter(const json::Value& rule);

} // namespace dnr
