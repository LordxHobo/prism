// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Icons.h"

#include "Http.h"
#include "Util.h"

#include <algorithm>
#include <cwctype>
#include <vector>
#include <set>
#include <thread>
#include <unordered_map>
#include <windows.h>

namespace icons {

namespace {

constexpr size_t kMaxIconBytes = 512 * 1024;
constexpr ULONGLONG kRefreshAfterMs = 7ULL * 24 * 60 * 60 * 1000;

struct Saved {
    std::wstring file;     // full path
    ULONGLONG savedAt { 0 }; // GetTickCount64-independent: unix ms
};

std::unordered_map<std::wstring, Saved> g_saved; // key: site (host[:port])
std::set<std::wstring> g_inFlight;
std::set<std::wstring> g_failed; // this session: don't keep trying
std::function<void()> g_listener;
bool g_scanned = false;

const std::wstring& iconDirectory()
{
    static std::wstring dir;
    if (dir.empty()) {
        dir = util::dataDirectory() + L"\\Icons";
        util::ensureDirectory(dir);
    }
    return dir;
}

ULONGLONG nowMilliseconds()
{
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart / 10000ULL - 11644473600000ULL;
}

std::wstring lower(std::wstring text)
{
    for (auto& c : text)
        c = static_cast<wchar_t>(towlower(c));
    return text;
}

// "https://www.example.com:8443/path" -> "www.example.com:8443"
std::wstring siteOf(const std::wstring& url)
{
    std::wstring text = lower(url);
    if (text.rfind(L"http://", 0) != 0 && text.rfind(L"https://", 0) != 0)
        return { };
    size_t start = text.find(L"://") + 3;
    size_t end = text.find_first_of(L"/?#", start);
    std::wstring host = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    if (size_t at = host.rfind(L'@'); at != std::wstring::npos)
        host = host.substr(at + 1);
    return host;
}

// The file name for a site: a hash, so nothing about the site is in it.
std::wstring fileStem(const std::wstring& site)
{
    unsigned long long hash = 1469598103934665603ULL;
    for (wchar_t c : site) {
        hash ^= static_cast<unsigned long long>(c);
        hash *= 1099511628211ULL;
    }
    wchar_t text[17];
    swprintf_s(text, L"%016llx", hash);
    return text;
}

void scan()
{
    if (g_scanned)
        return;
    g_scanned = true;
    // Index files are "<hash>.<ext>"; a small sidecar maps hashes to sites.
    std::string index = util::readFile(iconDirectory() + L"\\index.txt");
    size_t position = 0;
    while (position < index.size()) {
        size_t end = index.find('\n', position);
        std::string line = index.substr(position, end == std::string::npos ? std::string::npos : end - position);
        position = end == std::string::npos ? index.size() : end + 1;
        // site \t file name \t saved-at
        size_t tab1 = line.find('\t');
        size_t tab2 = tab1 == std::string::npos ? std::string::npos : line.find('\t', tab1 + 1);
        if (tab2 == std::string::npos)
            continue;
        std::wstring site = util::fromUTF8(line.substr(0, tab1));
        std::wstring file = iconDirectory() + L"\\" + util::fromUTF8(line.substr(tab1 + 1, tab2 - tab1 - 1));
        if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        g_saved[site] = { file, _strtoui64(line.c_str() + tab2 + 1, nullptr, 10) };
    }
}

void saveIndex()
{
    std::string index;
    for (auto& [site, saved] : g_saved) {
        size_t slash = saved.file.rfind(L'\\');
        index += util::toUTF8(site) + "\t" + util::toUTF8(saved.file.substr(slash + 1)) + "\t" + std::to_string(saved.savedAt) + "\n";
    }
    util::writeFileAtomically(iconDirectory() + L"\\index.txt", index);
}

// Only fetch from public web addresses (not the PC itself or IP addresses,
// which a page could use to make Prism poke at local devices).
bool isFetchable(const std::wstring& url)
{
    std::wstring site = siteOf(url);
    if (site.empty())
        return false;
    std::wstring host = site.substr(0, site[0] == L'[' ? site.find(L']') + 1 : site.find(L':'));
    if (host.empty() || host[0] == L'[' || host == L"localhost" || (host.size() > 10 && host.substr(host.size() - 10) == L".localhost"))
        return false;
    bool digitsAndDots = true;
    for (wchar_t c : host)
        digitsAndDots = digitsAndDots && (iswdigit(c) || c == L'.');
    return !digitsAndDots && host.find(L'.') != std::wstring::npos;
}

// Picks a file extension from the data itself.
std::wstring extensionFor(const std::string& data)
{
    auto starts = [&](const char* magic, size_t length) { return data.size() >= length && memcmp(data.data(), magic, length) == 0; };
    if (starts("\x89PNG", 4)) return L"png";
    if (starts("\0\0\1\0", 4)) return L"ico";
    if (starts("GIF8", 4)) return L"gif";
    if (starts("\xFF\xD8\xFF", 3)) return L"jpg";
    if (starts("RIFF", 4) && data.size() > 12 && memcmp(data.data() + 8, "WEBP", 4) == 0) return L"webp";
    if (starts("BM", 2)) return L"bmp";
    // SVG: text that has an <svg tag near the start.
    std::string head = data.substr(0, 1024);
    for (auto& c : head)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (head.find("<svg") != std::string::npos)
        return L"svg";
    return { };
}

int base64Value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

// data:image/png;base64,.... and data:image/svg+xml,<svg...>
bool decodeDataURL(const std::string& url, std::string& out)
{
    size_t comma = url.find(',');
    if (comma == std::string::npos || url.size() > kMaxIconBytes * 2)
        return false;
    std::string meta = url.substr(5, comma - 5);
    std::string payload = url.substr(comma + 1);
    if (meta.find(";base64") != std::string::npos) {
        int bits = 0, value = 0;
        for (char c : payload) {
            int v = base64Value(c);
            if (v < 0)
                continue;
            value = (value << 6) | v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out += static_cast<char>((value >> bits) & 0xFF);
            }
        }
    } else
        out = util::toUTF8(util::urlDecode(util::fromUTF8(payload)));
    return !out.empty() && out.size() <= kMaxIconBytes;
}

void store(const std::wstring& site, const std::string& data)
{
    std::wstring extension = extensionFor(data);
    if (extension.empty()) {
        g_failed.insert(site);
        return;
    }
    std::wstring file = iconDirectory() + L"\\" + fileStem(site) + L"." + extension;
    // An older icon with another extension goes away.
    auto old = g_saved.find(site);
    if (old != g_saved.end() && old->second.file != file)
        DeleteFileW(old->second.file.c_str());
    if (!util::writeFileAtomically(file, data))
        return;
    g_saved[site] = { file, nowMilliseconds() };
    saveIndex();
    if (g_listener)
        g_listener();
}

} // namespace

void setChangeListener(std::function<void()> listener)
{
    g_listener = std::move(listener);
}

const std::string& pageScript()
{
    // The largest listed icon up to 64 px (SVG counts as best), otherwise
    // /favicon.ico. Only the address comes back; Prism fetches it.
    static const std::string script = R"JS((function () {
  try {
    var best = null, bestScore = -1;
    var links = document.querySelectorAll('link[rel]');
    for (var i = 0; i < links.length; i++) {
      var link = links[i];
      var rel = (link.getAttribute('rel') || '').toLowerCase().split(/\s+/);
      if (rel.indexOf('icon') < 0 && rel.indexOf('apple-touch-icon') < 0) continue;
      var href = link.getAttribute('href');
      if (!href) continue;
      var score = rel.indexOf('icon') >= 0 ? 10 : 5;
      var type = (link.getAttribute('type') || '').toLowerCase();
      if (type.indexOf('svg') >= 0 || /\.svg(\?|$)/i.test(href)) score += 50;
      var sizes = (link.getAttribute('sizes') || '').match(/(\d+)x\d+/);
      if (sizes) { var size = +sizes[1]; score += size <= 64 ? size / 2 : Math.max(1, 40 - size / 16); }
      if (score > bestScore) { bestScore = score; best = href; }
    }
    return best ? new URL(best, document.baseURI).href : location.origin + '/favicon.ico';
  } catch (e) {
    return '';
  }
})())JS";
    return script;
}

void pageLoaded(const std::wstring& pageURL, const std::wstring& iconURL)
{
    scan();
    std::wstring site = siteOf(pageURL);
    if (site.empty() || g_inFlight.count(site) || g_failed.count(site))
        return;
    auto saved = g_saved.find(site);
    if (saved != g_saved.end() && nowMilliseconds() - saved->second.savedAt < kRefreshAfterMs)
        return;
    if (iconURL.size() > 64 * 1024)
        return;

    if (iconURL.rfind(L"data:", 0) == 0) {
        std::string data;
        if (decodeDataURL(util::toUTF8(iconURL), data))
            store(site, data);
        else
            g_failed.insert(site);
        return;
    }
    if (!isFetchable(iconURL)) {
        g_failed.insert(site);
        return;
    }
    g_inFlight.insert(site);
    std::wstring fallback = L"https://" + site + L"/favicon.ico";
    std::thread([site, iconURL, fallback] {
        auto fetch = [](const std::wstring& url, std::string& data) {
            http::Request request;
            request.url = url;
            request.headers.push_back({ L"Accept", L"image/avif,image/webp,image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5" });
            http::Response response;
            std::wstring error;
            if (!http::request(request, response, error, kMaxIconBytes) || response.status < 200 || response.status >= 300)
                return false;
            data = std::move(response.body);
            return !data.empty() && !extensionFor(data).empty();
        };
        std::string data;
        bool ok = fetch(iconURL, data) || (iconURL != fallback && fetch(fallback, data));
        util::postToMainThread([site, ok, data = std::move(data)] {
            g_inFlight.erase(site);
            if (ok)
                store(site, data);
            else
                g_failed.insert(site);
        });
    }).detach();
}

namespace {

// The icon a page's HTML lists: <link rel="icon" ...>, preferring SVG or
// a size near 32 px, then "shortcut icon", then apple-touch-icon. "" if none.
std::wstring iconFromHTML(const std::string& html, const std::wstring& pageURL)
{
    std::string lowerHTML = html;
    for (auto& c : lowerHTML)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    auto attribute = [&](const std::string& tag, const std::string& lowerTag, const char* name) {
        std::string key = std::string(name) + "=";
        for (size_t at = lowerTag.find(key); at != std::string::npos; at = lowerTag.find(key, at + 1)) {
            if (at > 0 && !isspace(static_cast<unsigned char>(lowerTag[at - 1])))
                continue;
            size_t start = at + key.size();
            if (start >= tag.size())
                return std::string();
            char quote = tag[start];
            if (quote == '"' || quote == '\'') {
                size_t end = tag.find(quote, start + 1);
                return end == std::string::npos ? std::string() : tag.substr(start + 1, end - start - 1);
            }
            size_t end = tag.find_first_of(" \t\r\n>", start);
            return tag.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }
        return std::string();
    };
    std::string best;
    int bestScore = -1;
    size_t limit = std::min(lowerHTML.size(), static_cast<size_t>(256 * 1024));
    for (size_t at = lowerHTML.find("<link"); at != std::string::npos && at < limit; at = lowerHTML.find("<link", at + 5)) {
        size_t end = lowerHTML.find('>', at);
        if (end == std::string::npos)
            break;
        std::string tag = html.substr(at, end - at);
        std::string lowerTag = lowerHTML.substr(at, end - at);
        std::string rel = attribute(tag, lowerTag, "rel");
        for (auto& c : rel)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        std::string href = attribute(tag, lowerTag, "href");
        if (href.empty() || rel.find("icon") == std::string::npos || rel.find("mask-icon") != std::string::npos)
            continue;
        std::string type = attribute(tag, lowerTag, "type");
        std::string sizes = attribute(tag, lowerTag, "sizes");
        int score = rel.find("apple-touch-icon") != std::string::npos ? 10 : 30;
        if (type.find("svg") != std::string::npos || href.find(".svg") != std::string::npos)
            score += 50;
        int size = atoi(sizes.c_str());
        if (size >= 16 && size <= 64)
            score += 20 - std::abs(size - 32) / 4;
        if (score > bestScore) {
            bestScore = score;
            best = href;
        }
    }
    if (best.empty())
        return { };
    // Entities in attribute values (&amp; is common).
    for (size_t amp = best.find("&amp;"); amp != std::string::npos; amp = best.find("&amp;", amp + 1))
        best.replace(amp, 5, "&");
    std::wstring href = util::fromUTF8(best);
    if (href.rfind(L"http://", 0) == 0 || href.rfind(L"https://", 0) == 0 || href.rfind(L"data:", 0) == 0)
        return href;
    std::wstring scheme = pageURL.substr(0, pageURL.find(L"://"));
    size_t hostStart = pageURL.find(L"://") + 3;
    size_t pathStart = pageURL.find(L'/', hostStart);
    std::wstring origin = pageURL.substr(0, pathStart);
    if (href.rfind(L"//", 0) == 0)
        return scheme + L":" + href;
    if (!href.empty() && href[0] == L'/')
        return origin + href;
    std::wstring base = pathStart == std::wstring::npos ? origin + L"/" : pageURL.substr(0, pageURL.find_last_of(L'/') + 1);
    size_t query = base.find_first_of(L"?#");
    if (query != std::wstring::npos)
        base = base.substr(0, base.rfind(L'/', query) + 1);
    return base + href;
}

std::vector<std::wstring> g_prefetchQueue;
bool g_prefetching = false;

void prefetchNext()
{
    if (g_prefetching || g_prefetchQueue.empty())
        return;
    std::wstring site = g_prefetchQueue.front();
    g_prefetchQueue.erase(g_prefetchQueue.begin());
    g_prefetching = true;
    g_inFlight.insert(site);
    std::thread([site] {
        std::wstring front = L"https://" + site + L"/";
        std::wstring iconURL;
        {
            http::Request request;
            request.url = front;
            request.headers.push_back({ L"Accept", L"text/html,application/xhtml+xml;q=0.9,*/*;q=0.5" });
            http::Response response;
            std::wstring error;
            if (http::request(request, response, error, 1024 * 1024) && response.status >= 200 && response.status < 300)
                iconURL = iconFromHTML(response.body, response.url.empty() ? front : response.url);
        }
        std::wstring fallback = L"https://" + site + L"/favicon.ico";
        auto fetch = [](const std::wstring& url, std::string& data) {
            if (!isFetchable(url))
                return false;
            http::Request request;
            request.url = url;
            request.headers.push_back({ L"Accept", L"image/avif,image/webp,image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5" });
            http::Response response;
            std::wstring error;
            if (!http::request(request, response, error, kMaxIconBytes) || response.status < 200 || response.status >= 300)
                return false;
            data = std::move(response.body);
            return !data.empty() && !extensionFor(data).empty();
        };
        std::string data;
        bool ok = false;
        if (iconURL.rfind(L"data:", 0) == 0)
            ok = decodeDataURL(util::toUTF8(iconURL), data) && !extensionFor(data).empty();
        else if (!iconURL.empty())
            ok = fetch(iconURL, data);
        if (!ok)
            ok = fetch(fallback, data);
        util::postToMainThread([site, ok, data = std::move(data)] {
            g_inFlight.erase(site);
            g_prefetching = false;
            if (ok)
                store(site, data);
            else
                g_failed.insert(site);
            prefetchNext();
        });
    }).detach();
}

} // namespace

void prefetch(const std::wstring& pageURL)
{
    scan();
    if (!isFetchable(pageURL))
        return;
    std::wstring site = siteOf(pageURL);
    if (site.empty() || g_inFlight.count(site) || g_failed.count(site)
        || std::find(g_prefetchQueue.begin(), g_prefetchQueue.end(), site) != g_prefetchQueue.end())
        return;
    auto saved = g_saved.find(site);
    if (saved != g_saved.end() && nowMilliseconds() - saved->second.savedAt < kRefreshAfterMs)
        return;
    g_prefetchQueue.push_back(site);
    prefetchNext();
}

std::wstring fileURLFor(const std::wstring& pageURL)
{
    scan();
    auto saved = g_saved.find(siteOf(pageURL));
    if (saved == g_saved.end())
        return { };
    return util::fileURLFromPath(saved->second.file) + L"?" + std::to_wstring(saved->second.savedAt);
}

std::wstring filePathFor(const std::wstring& pageURL)
{
    scan();
    auto saved = g_saved.find(siteOf(pageURL));
    return saved == g_saved.end() ? std::wstring() : saved->second.file;
}

void clear()
{
    scan();
    for (auto& [site, saved] : g_saved)
        DeleteFileW(saved.file.c_str());
    g_saved.clear();
    g_failed.clear();
    DeleteFileW((iconDirectory() + L"\\index.txt").c_str());
    if (g_listener)
        g_listener();
}

} // namespace icons
