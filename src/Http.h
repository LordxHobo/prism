// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Minimal HTTPS downloads (WinHTTP), used to fetch extensions from the Chrome
// Web Store and Firefox Add-ons, and Prism Shields' filter lists. Blocking:
// call from a worker thread.
namespace http {

// Downloads `url` (following redirects) into `body`. Fails on anything but
// HTTP 200 or if the response is larger than `maxBytes`. `progress`, if
// given, is called as data arrives with (bytes so far, total or 0 if unknown).
using Progress = std::function<void(uint64_t received, uint64_t total)>;
bool get(const std::wstring& url, std::string& body, std::wstring& error, size_t maxBytes = 256 * 1024 * 1024, const Progress& progress = nullptr);

// Any HTTP(S) request, for extensions' fetch() to sites they have
// permission for. No cookies are sent or kept. Returns false only if no
// answer arrived (any HTTP status is an answer).
struct Request {
    std::wstring method { L"GET" };
    std::wstring url;
    std::vector<std::pair<std::wstring, std::wstring>> headers;
    std::string body;
    std::wstring userAgent; // "" = Prism's own ("Prism/<version>")
};
struct Response {
    int status { 0 };
    std::wstring statusText;
    std::vector<std::pair<std::wstring, std::wstring>> headers;
    std::string body;
    std::wstring url; // after redirects
};
bool request(const Request&, Response&, std::wstring& error, size_t maxBytes = 64 * 1024 * 1024);

} // namespace http
