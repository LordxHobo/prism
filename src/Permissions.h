// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "WebKitApi.h"

#include <functional>
#include <string>

// What sites may do: know your location, show notifications. Decisions are
// saved per site (origin) in %LOCALAPPDATA%\Prism\permissions.json; the
// prompts themselves are shown by BrowserWindow.
//
// Camera and microphone aren't here: the Windows WebKit build has no media
// capture (ENABLE_MEDIA_STREAM is off), so sites can't ask for them.
namespace permissions {

// "location" or "notifications".
// Returns "allow", "block", or "" (ask).
std::wstring decision(const std::wstring& origin, const std::wstring& kind);
void set(const std::wstring& origin, const std::wstring& kind, bool allow);

std::wstring originString(WKSecurityOriginRef);
// "https://maps.example.com" -> "maps.example.com" (what prompts show).
std::wstring displayName(const std::wstring& origin);

// Sets up location and notifications for the browsing context.
// `showTab` brings a tab to the front (when a notification is clicked).
void initialize(WKContextRef, std::function<void(WKPageRef)> showTab);
void shutdown();

// For prism://settings: "list", "remove".
std::string handleCommand(const std::wstring& command, const std::wstring& argument);

} // namespace permissions
