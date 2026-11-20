// Prism — filtering every request pages make (with PrismBundle.dll).
//
// PrismBundle.dll runs in WebKit's content processes and asks Prism about
// each request a page makes before it's sent. Prism answers here, using
// Prism Shields' filter lists and the rules of content-blocking extensions,
// with the full address of the request, the site in the address bar, and
// (roughly) what kind of file it is. Blocked requests are counted per tab.
//
// The filtering proxy (Blocker.h) stays in place for whole domains, and
// catches anything the bundle doesn't see (such as service workers).
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "Shields.h"
#include "WebKitApi.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace requestfilter {

// The bundle's file next to Prism.exe ("" if it isn't there).
std::wstring bundlePath();

// Hooks Prism up to the bundle in `context`'s content processes.
void initialize(WKContextRef context);

// True once the bundle has asked about a request (it's running).
bool bundleActive();

// Lets Prism tell which page a request comes from. Call for every page.
void attachPage(WKPageRef page);
void detachPage(WKPageRef page);

// A page started showing a new document: its counts start over.
void pageCommitted(WKPageRef page);

// Requests blocked on a page since it started showing its document.
uint32_t blockedOnPage(WKPageRef page);
// The most-blocked hosts on a page (most first).
std::vector<std::pair<std::string, uint32_t>> blockedHostsOnPage(WKPageRef page, size_t max);

// Path-level rules of one content-blocking extension.
struct ExtensionRules {
    std::string owner;   // extension name, shown in the Shields panel
    void* engine = nullptr; // shields::buildEngine
    ~ExtensionRules();
};
void setExtensionRules(std::vector<std::shared_ptr<ExtensionRules>> rules);

// Rules changed (lists rebuilt, Shields switched, a site allowed): pages
// forget the answers they remembered.
void rulesChanged();

// The decision itself (exposed for tests). `kind` is the bundle's guess:
// an adblock request type, or "" when it couldn't tell. Fills `owner` with
// whatever blocks it.
bool decide(const std::string& kind, const std::string& pageURL, const std::string& url, std::string* owner);
// The same, with what to load instead of the request, if anything
// (a stand-in for a blocked file, or the address without tracking
// parameters).
shields::RequestVerdict decideRequest(const std::string& kind, const std::string& pageURL, const std::string& url, std::string* owner);

} // namespace requestfilter
