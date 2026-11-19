// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "History.h"

#include "Engine.h"
#include "Icons.h"
#include "Json.h"
#include "Settings.h"
#include "Util.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <unordered_map>
#include <windows.h>

namespace history {

namespace {

// history.tsv, UTF-8, one entry per line, fields separated by tabs:
//   V <time> <url> <title>    a visit (time in ms since 1970, UTC)
//   T <url> <title>           the page's title changed
// New entries are appended; the file is rewritten when something is deleted
// and when old entries are dropped at startup.
constexpr long long kKeepMilliseconds = 90LL * 24 * 60 * 60 * 1000;
constexpr size_t kMaxVisits = 100000;

struct Visit {
    long long time { 0 };
    std::wstring url;
};

struct Page {
    std::wstring title;
    int visits { 0 };
    long long lastVisit { 0 };
};

std::vector<Visit> g_visits; // oldest first
std::unordered_map<std::wstring, Page> g_pages;
bool g_loaded = false;

std::wstring historyFile()
{
    return util::dataDirectory() + L"\\history.tsv";
}

long long nowMilliseconds()
{
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    // FILETIME counts 100 ns steps since 1601.
    return static_cast<long long>(value.QuadPart / 10000ULL) - 11644473600000LL;
}

std::wstring lower(std::wstring text)
{
    for (auto& c : text)
        c = static_cast<wchar_t>(towlower(c));
    return text;
}

// Tabs and line breaks would break the file format.
std::string field(const std::wstring& text)
{
    std::wstring clean = text.size() > 2048 ? text.substr(0, 2048) : text;
    for (auto& c : clean) {
        if (c == L'\t' || c == L'\r' || c == L'\n')
            c = L' ';
    }
    return util::toUTF8(clean);
}

std::vector<std::string> splitTabs(const std::string& line)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t tab = line.find('\t', start);
        parts.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos)
            break;
        start = tab + 1;
    }
    return parts;
}

bool shouldRecord(const std::wstring& url)
{
    std::wstring scheme = lower(url.substr(0, url.find(L':')));
    if (scheme == L"http" || scheme == L"https")
        return url.size() < 8192;
    if (scheme == L"file") // but not Prism's own pages, which are files too
        return !engine::isInternalURL(url);
    return false;
}

void append(const std::string& line)
{
    HANDLE file = CreateFileW(historyFile().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

void rewrite()
{
    std::string contents;
    contents.reserve(g_visits.size() * 80);
    for (auto& visit : g_visits) {
        auto page = g_pages.find(visit.url);
        contents += "V\t" + std::to_string(visit.time) + "\t" + field(visit.url) + "\t"
            + field(page != g_pages.end() ? page->second.title : L"") + "\n";
    }
    if (g_visits.empty())
        DeleteFileW(historyFile().c_str());
    else
        util::writeFileAtomically(historyFile(), contents);
}

// Rebuilds the per-page summary after visits were removed.
void rebuildPages()
{
    std::unordered_map<std::wstring, Page> pages;
    for (auto& visit : g_visits) {
        auto& page = pages[visit.url];
        auto old = g_pages.find(visit.url);
        if (old != g_pages.end())
            page.title = old->second.title;
        page.visits++;
        page.lastVisit = std::max(page.lastVisit, visit.time);
    }
    g_pages = std::move(pages);
}

std::vector<std::wstring> words(const std::wstring& text)
{
    std::vector<std::wstring> result;
    std::wstring current;
    for (wchar_t c : lower(text)) {
        if (iswspace(c)) {
            if (!current.empty())
                result.push_back(std::move(current));
            current.clear();
        } else
            current += c;
    }
    if (!current.empty())
        result.push_back(std::move(current));
    return result;
}

// The address without "https://" and "www.", for matching what people type.
std::wstring bareAddress(const std::wstring& url)
{
    std::wstring text = lower(url);
    size_t scheme = text.find(L"://");
    if (scheme != std::wstring::npos)
        text = text.substr(scheme + 3);
    if (text.rfind(L"www.", 0) == 0)
        text = text.substr(4);
    return text;
}

bool matchesAll(const std::vector<std::wstring>& terms, const std::wstring& url, const std::wstring& title)
{
    std::wstring haystack = bareAddress(url) + L" " + lower(title);
    for (auto& term : terms) {
        if (haystack.find(term) == std::wstring::npos)
            return false;
    }
    return true;
}

std::string visitJSON(const Visit& visit)
{
    auto page = g_pages.find(visit.url);
    return "{\"time\":" + std::to_string(visit.time) + ",\"url\":" + util::jsonString(visit.url)
        + ",\"title\":" + util::jsonString(page != g_pages.end() ? page->second.title : L"")
        + ",\"icon\":" + util::jsonString(icons::fileURLFor(visit.url)) + "}";
}

} // namespace

void load()
{
    if (g_loaded)
        return;
    g_loaded = true;
    std::string contents = util::readFile(historyFile());
    long long oldest = nowMilliseconds() - kKeepMilliseconds;
    bool dropped = false;
    size_t titleLines = 0;
    size_t start = 0;
    while (start < contents.size()) {
        size_t end = contents.find('\n', start);
        if (end == std::string::npos)
            end = contents.size();
        std::string line = contents.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto parts = splitTabs(line);
        if (parts.size() >= 3 && parts[0] == "V") {
            long long time = _atoi64(parts[1].c_str());
            std::wstring url = util::fromUTF8(parts[2]);
            if (time < oldest || url.empty()) {
                dropped = true;
                continue;
            }
            g_visits.push_back({ time, url });
            auto& page = g_pages[url];
            page.visits++;
            page.lastVisit = std::max(page.lastVisit, time);
            if (parts.size() >= 4 && !parts[3].empty())
                page.title = util::fromUTF8(parts[3]);
        } else if (parts.size() >= 3 && parts[0] == "T") {
            ++titleLines;
            auto page = g_pages.find(util::fromUTF8(parts[1]));
            if (page != g_pages.end())
                page->second.title = util::fromUTF8(parts[2]);
        }
    }
    std::stable_sort(g_visits.begin(), g_visits.end(), [](const Visit& a, const Visit& b) { return a.time < b.time; });
    if (g_visits.size() > kMaxVisits) {
        g_visits.erase(g_visits.begin(), g_visits.end() - kMaxVisits);
        rebuildPages();
        dropped = true;
    }
    // Title lines pile up; fold them into the visits now and then.
    if (dropped || titleLines > 1000)
        rewrite();
}

void recordVisit(const std::wstring& url, const std::wstring& title)
{
    if (!settings::saveHistory() || !shouldRecord(url))
        return;
    load();
    long long now = nowMilliseconds();
    // The same address again within a second is the same visit (a page that
    // commits and then updates its URL, a double click).
    if (!g_visits.empty() && g_visits.back().url == url && now - g_visits.back().time < 1000)
        return;
    g_visits.push_back({ now, url });
    auto& page = g_pages[url];
    page.visits++;
    page.lastVisit = now;
    if (!title.empty())
        page.title = title;
    append("V\t" + std::to_string(now) + "\t" + field(url) + "\t" + field(page.title) + "\n");
}

void updateTitle(const std::wstring& url, const std::wstring& title)
{
    if (title.empty() || !settings::saveHistory())
        return;
    auto page = g_pages.find(url);
    if (page == g_pages.end() || page->second.title == title)
        return;
    page->second.title = title;
    append("T\t" + field(url) + "\t" + field(title) + "\n");
}

std::vector<Match> search(const std::wstring& text, size_t limit)
{
    load();
    std::vector<Match> matches;
    auto terms = words(text);
    if (terms.empty())
        return matches;
    long long now = nowMilliseconds();
    for (auto& [url, page] : g_pages) {
        if (!matchesAll(terms, url, page.title))
            continue;
        // Visits count less the older they are (half every 2 weeks).
        double ageDays = static_cast<double>(now - page.lastVisit) / 86400000.0;
        double score = (1 + std::log2(1.0 + page.visits)) * std::pow(0.5, ageDays / 14.0);
        std::wstring bare = bareAddress(url);
        // "git" should suggest github.com before a page that mentions git.
        if (bare.rfind(terms[0], 0) == 0)
            score *= 4;
        else if (bare.find(L"." + terms[0]) != std::wstring::npos && bare.find(L"." + terms[0]) < bare.find(L'/'))
            score *= 2;
        // Prefer a site's front page to its deeper pages.
        size_t slash = bare.find(L'/');
        if (slash == std::wstring::npos || slash + 1 >= bare.size())
            score *= 1.5;
        matches.push_back({ url, page.title, score });
    }
    std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) { return a.score > b.score; });
    if (matches.size() > limit)
        matches.resize(limit);
    return matches;
}

std::vector<Match> topSites(size_t count, const std::vector<std::wstring>& hidden)
{
    load();
    long long now = nowMilliseconds();
    // Group pages by site; a site's score is the sum of its pages'.
    struct Site {
        double score { 0 };
        Match best;
        double bestScore { -1 };
    };
    std::unordered_map<std::wstring, Site> sites;
    for (auto& [url, page] : g_pages) {
        if (url.rfind(L"http", 0) != 0)
            continue;
        std::wstring bare = bareAddress(url);
        std::wstring host = bare.substr(0, bare.find_first_of(L"/?#"));
        bool isHidden = false;
        for (auto& item : hidden)
            isHidden = isHidden || item == host;
        if (host.empty() || isHidden)
            continue;
        double ageDays = static_cast<double>(now - page.lastVisit) / 86400000.0;
        double score = page.visits * std::pow(0.5, ageDays / 30.0);
        auto& site = sites[host];
        site.score += score;
        size_t slash = bare.find(L'/');
        double pageScore = score * (slash == std::wstring::npos || slash + 1 >= bare.size() ? 3 : 1);
        if (pageScore > site.bestScore) {
            site.bestScore = pageScore;
            site.best = { url, page.title, 0 };
        }
    }
    std::vector<Match> result;
    for (auto& [host, site] : sites) {
        Match match = site.best;
        match.score = site.score;
        // A deep page as the site's best: open the site's front page instead.
        result.push_back(match);
    }
    std::sort(result.begin(), result.end(), [](const Match& a, const Match& b) { return a.score > b.score; });
    if (result.size() > count)
        result.resize(count);
    return result;
}

void clear()
{
    g_visits.clear();
    g_pages.clear();
    DeleteFileW(historyFile().c_str());
}

std::string handleCommand(const std::wstring& command, const std::wstring& argument)
{
    load();
    if (command == L"query") {
        // {"text": "...", "before": <time or 0>, "limit": n}
        json::Value request;
        json::parse(util::toUTF8(argument), request);
        auto terms = words(util::fromUTF8(request["text"].asString()));
        long long before = static_cast<long long>(request["before"].asNumber(0));
        size_t limit = static_cast<size_t>(std::clamp(request["limit"].asNumber(200), 1.0, 1000.0));
        std::string json = "{\"saving\":" + std::string(settings::saveHistory() ? "true" : "false") + ",\"visits\":[";
        size_t count = 0;
        bool more = false;
        for (auto it = g_visits.rbegin(); it != g_visits.rend(); ++it) {
            if (before && it->time >= before)
                continue;
            if (!terms.empty()) {
                auto page = g_pages.find(it->url);
                if (!matchesAll(terms, it->url, page != g_pages.end() ? page->second.title : L""))
                    continue;
            }
            if (count == limit) {
                more = true;
                break;
            }
            json += (count++ ? "," : "") + visitJSON(*it);
        }
        json += "],\"more\":" + std::string(more ? "true" : "false") + "}";
        return json;
    }
    if (command == L"delete") {
        // [{"time": t, "url": "..."}, ...]
        json::Value request;
        json::parse(util::toUTF8(argument), request);
        size_t before = g_visits.size();
        for (auto& item : request.items()) {
            long long time = static_cast<long long>(item["time"].asNumber(0));
            std::wstring url = util::fromUTF8(item["url"].asString());
            auto found = std::find_if(g_visits.begin(), g_visits.end(), [&](const Visit& v) { return v.time == time && v.url == url; });
            if (found != g_visits.end())
                g_visits.erase(found);
        }
        if (g_visits.size() != before) {
            rebuildPages();
            rewrite();
        }
        return "true";
    }
    if (command == L"deleteURL") {
        // Every visit to this address.
        std::erase_if(g_visits, [&](const Visit& v) { return v.url == argument; });
        rebuildPages();
        rewrite();
        return "true";
    }
    if (command == L"clear") {
        // "hour", "day", "week", "month" or "all"
        long long span = 0;
        if (argument == L"hour") span = 3600000LL;
        else if (argument == L"day") span = 86400000LL;
        else if (argument == L"week") span = 7 * 86400000LL;
        else if (argument == L"month") span = 30 * 86400000LL;
        if (span == 0) {
            clear();
            return "true";
        }
        long long from = nowMilliseconds() - span;
        std::erase_if(g_visits, [&](const Visit& v) { return v.time >= from; });
        rebuildPages();
        rewrite();
        return "true";
    }
    return "null";
}

} // namespace history
