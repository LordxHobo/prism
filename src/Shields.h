// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "WebKitApi.h"

#include <functional>
#include <string>

// Prism Shields: the built-in ad and tracker blocker.
//
// Powered by Brave's adblock-rust engine (prism_shields.dll) running the same
// filter lists as uBlock Origin. Three layers:
// - Network: ad/tracker domains are refused by Prism's local proxy (see
//   Blocker.h; per domain until Prism has its own WebKit build).
// - Cosmetic: leftover ad boxes are hidden with CSS, per site and generic.
// - Scriptlets: small scripts from uBlock Origin that defuse ads served from
//   a site's own servers (like YouTube's). Ad images and frames from a site's
//   own servers that network rules match are hidden too.
namespace shields {

// Loads the engine and filter lists (downloading them on first run) in the
// background. `notify` receives events for prism://settings and the Shields
// panel, as JSON.
void initialize(std::function<void(const std::string& eventJSON)> notify);

// Called before every page and frame load: prepares that site's cosmetic
// filters and scriptlets so they're in place before the page's own scripts.
void prepareNavigation(const std::wstring& url);

// The script message handler pages use for generic hiding and ad checks.
void handleScriptMessage(WKScriptMessageRef, WKCompletionListenerRef reply);

// Commands from prism://settings ("shields.*") and the Shields panel.
// Returns JSON.
std::string handleCommand(const std::wstring& command, const std::wstring& argument);

// For the toolbar: is Shields protecting this site?
bool isActiveFor(const std::wstring& url);

// True while Prism is starting and Shields' blocker isn't built yet.
bool isStarting();
// Runs `task` once the blocker is ready (straight away if it already is, and
// after 5 seconds at the latest), so the first pages get their site fixes.
void whenReady(std::function<void()> task);

} // namespace shields
