// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <string>

// v0.8 privacy protections, each switchable per site
// (%LOCALAPPDATA%\Prism\site-privacy.json):
// - HTTPS: http:// addresses are opened with https:// ("first": fall back to
//   http when a site has no HTTPS; "only": ask first).
// - Tracking parameters (utm_*, fbclid, gclid, ...) are removed from
//   addresses before pages open.
// - Fingerprinting protection: a page script that makes canvas, WebGL and
//   audio read-outs slightly different per site and per session, and
//   reports common values for the graphics card and processor count.
namespace privacy {

void initialize();

// "off", "first" or "only".
std::wstring httpsMode();

// The site key settings are stored under: the host without "www.".
std::wstring siteOf(const std::wstring& url);

bool httpsFor(const std::wstring& url);
bool trackingParamsFor(const std::wstring& url);
bool fingerprintingFor(const std::wstring& url);

// `url` without tracking parameters (the same string if there are none or
// removing them is off for its site).
std::wstring removeTrackingParameters(const std::wstring& url);

// The https:// address to try instead of `url`, or "" to keep it.
std::wstring upgradeURL(const std::wstring& url);

// This session, open the site over http (it has no working HTTPS).
void allowHttpThisSession(const std::wstring& url);

// For the Shields panel: {"https":bool,"params":bool,"fingerprinting":bool,
// "httpsMode":"..."} for the site, and changing one ("https", "params",
// "fingerprinting").
std::string siteJSON(const std::wstring& url);
void setForSite(const std::wstring& url, const std::wstring& key, bool on);

// For prism://settings: "list" (sites with exceptions), "remove" <site>.
std::string handleCommand(const std::wstring& command, const std::wstring& argument);

// Rebuilds the fingerprinting script after settings changed.
void settingsChanged();

} // namespace privacy
