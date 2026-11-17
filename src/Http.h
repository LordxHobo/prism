// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <string>

// Minimal HTTPS downloads (WinHTTP), used only to fetch extensions from the
// Chrome Web Store and Firefox Add-ons when you install one. Blocking: call
// from a worker thread.
namespace http {

// Downloads `url` (following redirects) into `body`. Fails on anything but
// HTTP 200 or if the response is larger than `maxBytes`.
bool get(const std::wstring& url, std::string& body, std::wstring& error, size_t maxBytes = 256 * 1024 * 1024);

} // namespace http
