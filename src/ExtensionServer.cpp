// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Serves each extension's files from its own address,
// http://<extension>.localhost:<port>/, on this computer only (127.0.0.1).
//
// Extensions expect "/" to be their own folder, and their own origin
// (storage that's theirs alone). WebKit's C API can't add a
// chrome-extension:// scheme, but it treats *.localhost as a secure origin
// and connects it to this computer, so this works the same way.
//
// Only the extension's own pages may load its files. Other pages (websites,
// content scripts running in them) only get the files the extension lists
// as "web_accessible_resources", like in Chrome.

#include "ExtensionServer.h"

#include "Util.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace extensions::server {

namespace {

std::mutex g_lock;
std::map<std::string, Site> g_sites; // by host label
std::map<std::string, Site> g_prismSites; // Prism's own, by host label
std::atomic<uint16_t> g_port { 0 };

std::string lowerASCII(std::string text)
{
    for (auto& c : text)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
}

bool sendAll(SOCKET socket, const char* data, size_t size)
{
    while (size) {
        int sent = send(socket, data, static_cast<int>(std::min<size_t>(size, 1 << 20)), 0);
        if (sent <= 0)
            return false;
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

void respond(SOCKET socket, int status, const char* reason, const std::string& extraHeaders = { }, const std::string& body = { }, bool head = false, const std::string& type = "text/plain; charset=utf-8")
{
    std::string response = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
        "Content-Type: " + type + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Cache-Control: no-cache\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n" + extraHeaders + "\r\n";
    sendAll(socket, response.data(), response.size());
    if (!head && !body.empty())
        sendAll(socket, body.data(), body.size());
}

const char* mimeType(const std::string& path)
{
    static const std::map<std::string, const char*> types = {
        { "html", "text/html; charset=utf-8" }, { "htm", "text/html; charset=utf-8" },
        { "js", "text/javascript; charset=utf-8" }, { "mjs", "text/javascript; charset=utf-8" },
        { "css", "text/css; charset=utf-8" }, { "json", "application/json; charset=utf-8" },
        { "map", "application/json; charset=utf-8" }, { "txt", "text/plain; charset=utf-8" },
        { "xml", "text/xml; charset=utf-8" }, { "svg", "image/svg+xml" }, { "png", "image/png" },
        { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "gif", "image/gif" }, { "webp", "image/webp" },
        { "ico", "image/x-icon" }, { "bmp", "image/bmp" }, { "avif", "image/avif" },
        { "woff", "font/woff" }, { "woff2", "font/woff2" }, { "ttf", "font/ttf" }, { "otf", "font/otf" },
        { "wasm", "application/wasm" }, { "mp3", "audio/mpeg" }, { "ogg", "audio/ogg" }, { "wav", "audio/wav" },
        { "mp4", "video/mp4" }, { "webm", "video/webm" }, { "pdf", "application/pdf" },
    };
    size_t dot = path.rfind('.');
    if (dot == std::string::npos)
        return "application/octet-stream";
    auto found = types.find(lowerASCII(path.substr(dot + 1)));
    return found == types.end() ? "application/octet-stream" : found->second;
}

// "*" matches anything, including "/".
bool globMatch(const std::string& pattern, const std::string& text)
{
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == text[t] || pattern[p] == '?')) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else
            return false;
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

std::string percentDecode(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && isxdigit(static_cast<unsigned char>(text[i + 1])) && isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out += static_cast<char>(strtol(text.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else
            out += text[i];
    }
    return out;
}

std::string header(const std::string& request, const char* name)
{
    std::string lower = lowerASCII(request);
    std::string key = std::string("\r\n") + name + ":";
    size_t at = lower.find(key);
    if (at == std::string::npos)
        return { };
    size_t start = at + key.size();
    size_t end = request.find("\r\n", start);
    std::string value = request.substr(start, end == std::string::npos ? std::string::npos : end - start);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.erase(0, 1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.pop_back();
    return value;
}

void handle(SOCKET socket)
{
    DWORD timeout = 10000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 32 * 1024) {
        int received = recv(socket, buffer, sizeof(buffer), 0);
        if (received <= 0)
            return;
        request.append(buffer, static_cast<size_t>(received));
    }

    size_t lineEnd = request.find("\r\n");
    std::string line = request.substr(0, lineEnd);
    size_t space1 = line.find(' '), space2 = line.rfind(' ');
    if (space1 == std::string::npos || space2 == space1) {
        respond(socket, 400, "Bad Request");
        return;
    }
    std::string method = line.substr(0, space1);
    std::string target = line.substr(space1 + 1, space2 - space1 - 1);
    bool head = method == "HEAD";
    if (method != "GET" && !head) {
        respond(socket, 405, "Method Not Allowed", "Allow: GET, HEAD\r\n");
        return;
    }

    // Host: <label>.localhost:<port>, nothing else (no DNS rebinding).
    std::string host = lowerASCII(header(request, "host"));
    std::string suffix = ".localhost:" + std::to_string(g_port.load());
    if (host.size() <= suffix.size() || host.compare(host.size() - suffix.size(), std::string::npos, suffix) != 0) {
        respond(socket, 404, "Not Found");
        return;
    }
    std::string label = host.substr(0, host.size() - suffix.size());
    Site site;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto found = g_sites.find(label);
        if (found == g_sites.end()) {
            found = g_prismSites.find(label);
            if (found == g_prismSites.end()) {
                respond(socket, 404, "Not Found");
                return;
            }
        }
        site = found->second;
    }

    std::string path = percentDecode(target.substr(0, target.find_first_of("?#")));
    while (!path.empty() && path[0] == '/')
        path.erase(0, 1);
    if (path.empty() || path.find('\\') != std::string::npos || path.find(':') != std::string::npos || path.find('\0') != std::string::npos) {
        respond(socket, 404, "Not Found");
        return;
    }
    for (size_t start = 0; start <= path.size();) {
        size_t slash = path.find('/', start);
        std::string segment = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (segment == ".." || segment == ".") {
            respond(socket, 404, "Not Found");
            return;
        }
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }

    // Who's asking? The extension's own pages are "same-origin" (or "none"
    // for typing the address / Prism opening it).
    std::string origin = "http://" + host;
    std::string fetchSite = lowerASCII(header(request, "sec-fetch-site"));
    std::string requestOrigin = lowerASCII(header(request, "origin"));
    std::string referer = lowerASCII(header(request, "referer"));
    bool own;
    if (!fetchSite.empty())
        own = fetchSite == "same-origin" || fetchSite == "none";
    else
        own = (requestOrigin.empty() || requestOrigin == origin) && (referer.empty() || referer.rfind(origin + "/", 0) == 0);
    bool accessible = false;
    for (auto& pattern : site.webAccessible) {
        std::string clean = pattern;
        while (!clean.empty() && clean[0] == '/')
            clean.erase(0, 1);
        if (globMatch(lowerASCII(clean), lowerASCII(path))) {
            accessible = true;
            break;
        }
    }
    if (!own && !accessible) {
        respond(socket, 403, "Forbidden", { }, "This extension file isn't available to web pages.\n", head);
        return;
    }

    std::error_code error;
    fs::path base = site.dir;
    std::string inside = path;
    for (auto& [prefix, dir] : site.mounts) {
        if (path.size() > prefix.size() + 1 && path.compare(0, prefix.size(), prefix) == 0 && path[prefix.size()] == '/') {
            base = dir;
            inside = path.substr(prefix.size() + 1);
            break;
        }
    }
    fs::path file = base / fs::path(util::fromUTF8(inside)).make_preferred();
    fs::path canonical = fs::weakly_canonical(file, error);
    fs::path root = fs::weakly_canonical(base, error);
    auto relative = canonical.lexically_relative(root);
    if (error || relative.empty() || relative.native().rfind(L"..", 0) == 0 || !fs::is_regular_file(canonical, error)) {
        respond(socket, 404, "Not Found", { }, "Not found\n", head);
        return;
    }
    std::ifstream stream(canonical, std::ios::binary);
    std::ostringstream contents;
    contents << stream.rdbuf();
    std::string extra;
    if (accessible && !own)
        extra = "Access-Control-Allow-Origin: *\r\n";
    respond(socket, 200, "OK", extra, contents.str(), head, mimeType(path));
}

} // namespace

std::string hostLabel(const std::string& id)
{
    std::string lower = lowerASCII(id);
    bool usable = !lower.empty() && lower.size() <= 63 && lower.front() != '-' && lower.back() != '-';
    for (char c : lower)
        usable = usable && (isalnum(static_cast<unsigned char>(c)) || c == '-');
    if (usable)
        return lower;
    // Firefox IDs ("addon@example.com", "{uuid}") aren't valid host names:
    // use a stable hash of the ID instead.
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : id) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char text[32];
    snprintf(text, sizeof(text), "x%016llx", static_cast<unsigned long long>(hash));
    return text;
}

void setSites(std::vector<Site> sites)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_sites.clear();
    for (auto& site : sites)
        g_sites[hostLabel(site.id)] = std::move(site);
}

void setPrismSite(Site site)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_prismSites[hostLabel(site.id)] = std::move(site);
}

uint16_t port()
{
    return g_port.load();
}

uint16_t start()
{
    if (g_port.load())
        return g_port.load();
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data))
        return 0;

    // The same port every time if possible: each extension's address (and
    // so its saved data in WebKit) includes it.
    std::wstring saved = util::readSetting(L"ExtensionPort");
    int preferred = _wtoi(saved.c_str());
    if (preferred < 1024 || preferred > 65535) {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        preferred = 49152 + static_cast<int>(counter.QuadPart % 15000);
    }

    SOCKET listener = INVALID_SOCKET;
    int chosen = 0;
    for (int attempt = 0; attempt < 20 && listener == INVALID_SOCKET; ++attempt) {
        int candidate = attempt == 0 ? preferred : 49152 + static_cast<int>((preferred + attempt * 7919) % 15000);
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            break;
        BOOL exclusive = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in address = { };
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<u_short>(candidate));
        if (bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(s, SOMAXCONN) == 0) {
            listener = s;
            chosen = candidate;
        } else
            closesocket(s);
    }
    if (listener == INVALID_SOCKET)
        return 0;
    if (chosen != _wtoi(saved.c_str()))
        util::writeSetting(L"ExtensionPort", std::to_wstring(chosen));
    g_port = static_cast<uint16_t>(chosen);

    std::thread([listener] {
        for (;;) {
            SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET)
                continue;
            std::thread([client] {
                handle(client);
                shutdown(client, SD_SEND);
                closesocket(client);
            }).detach();
        }
    }).detach();
    return g_port.load();
}

} // namespace extensions::server
