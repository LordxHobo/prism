// Prism — guessing a request's type from its address.
//
// WebKit tells the bundle the address of every request a page makes, but not
// whether it's for a script, a picture or a stylesheet. Filter lists often
// care ("$script", "$image"), so Prism guesses from the file extension. When
// there's no telling extension, the request is checked as "other" (rules
// without a type) and as a script or fetch, which is what most untyped
// tracker requests are.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace requestkind {

// Lower-case host of an http(s) URL, without port, user info or brackets.
// Empty for anything else.
inline std::string hostOf(std::string_view url)
{
    size_t start;
    if (url.size() > 7 && (url.compare(0, 7, "http://") == 0 || url.compare(0, 7, "HTTP://") == 0))
        start = 7;
    else if (url.size() > 8 && (url.compare(0, 8, "https://") == 0 || url.compare(0, 8, "HTTPS://") == 0))
        start = 8;
    else
        return { };
    size_t end = url.find_first_of("/?#", start);
    std::string_view authority = url.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    size_t at = authority.rfind('@');
    if (at != std::string_view::npos)
        authority = authority.substr(at + 1);
    std::string host;
    if (!authority.empty() && authority[0] == '[') {
        size_t close = authority.find(']');
        host = std::string(authority.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1));
    } else {
        size_t colon = authority.find(':');
        host = std::string(authority.substr(0, colon));
    }
    for (auto& c : host)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!host.empty() && host.back() == '.')
        host.pop_back();
    return host;
}

// Hosts on this PC: never filtered (Prism's own extension pages live on
// *.localhost).
inline bool isLocalHost(const std::string& host)
{
    if (host.empty() || host == "localhost" || host == "::1" || host.rfind("127.", 0) == 0)
        return true;
    return host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0;
}

// The lower-case extension of the URL's last path segment ("" if none).
inline std::string extensionOf(std::string_view url)
{
    size_t scheme = url.find("://");
    size_t pathStart = url.find('/', scheme == std::string_view::npos ? 0 : scheme + 3);
    if (pathStart == std::string_view::npos)
        return { };
    size_t pathEnd = url.find_first_of("?#", pathStart);
    std::string_view path = url.substr(pathStart, pathEnd == std::string_view::npos ? std::string_view::npos : pathEnd - pathStart);
    size_t slash = path.rfind('/');
    std::string_view last = path.substr(slash + 1);
    size_t dot = last.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= last.size() || last.size() - dot > 6)
        return { };
    std::string ext(last.substr(dot + 1));
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

// adblock-rust request type for a subresource, or "" when unknown.
inline const char* guess(std::string_view url)
{
    std::string ext = extensionOf(url);
    if (ext.empty())
        return "";
    static const char* const scripts[] = { "js", "mjs", "cjs", "jsx" };
    static const char* const styles[] = { "css" };
    static const char* const images[] = { "png", "jpg", "jpeg", "gif", "webp", "avif", "svg", "ico", "bmp", "apng", "jxl", "cur" };
    static const char* const fonts[] = { "woff", "woff2", "ttf", "otf", "eot" };
    static const char* const media[] = { "mp4", "webm", "mp3", "m4a", "m4v", "ogg", "oga", "ogv", "opus", "wav", "flac", "aac", "m3u8", "mpd", "ts", "m4s", "mov" };
    for (auto* e : scripts) if (ext == e) return "script";
    for (auto* e : styles) if (ext == e) return "stylesheet";
    for (auto* e : images) if (ext == e) return "image";
    for (auto* e : fonts) if (ext == e) return "font";
    for (auto* e : media) if (ext == e) return "media";
    if (ext == "json" || ext == "xml")
        return "xmlhttprequest";
    if (ext == "html" || ext == "htm" || ext == "php" || ext == "aspx" || ext == "asp" || ext == "jsp")
        return ""; // could be anything (frames, scripts, pixels)
    return "";
}

} // namespace requestkind
