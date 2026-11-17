// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Blocker.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <iterator>
#include <map>
#include <mutex>

namespace blocker {

// ---------------------------------------------------------------------------
// Rules

static bool isPlainDomain(const std::string& text)
{
    if (text.empty() || text.size() > 253)
        return false;
    for (char c : text) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.'))
            return false;
    }
    return text.front() != '.' && text.back() != '.';
}

// "||example.com^" (or "||example.com", "||example.com/") means "this domain
// and its subdomains". Anything with a path or wildcard needs the full URL.
static std::string domainFromURLFilter(const std::string& filter)
{
    if (filter.size() < 3 || filter.compare(0, 2, "||") != 0)
        return { };
    std::string rest = filter.substr(2);
    while (!rest.empty() && (rest.back() == '^' || rest.back() == '/'))
        rest.pop_back();
    std::transform(rest.begin(), rest.end(), rest.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return isPlainDomain(rest) ? rest : std::string();
}

size_t collectDomains(const json::Value& rules, std::vector<std::string>& blocked, std::vector<std::string>& allowed)
{
    // Conditions a domain-level blocker can honour: which domain, and
    // first/third party (approximated as "always") or "not as a page".
    static const char* usable[] = { "urlFilter", "requestDomains", "domainType", "excludedResourceTypes", "isUrlFilterCaseSensitive" };
    size_t used = 0;
    for (auto& rule : rules.items()) {
        const json::Value& action = rule["action"];
        const json::Value& condition = rule["condition"];
        if (!condition.isObject())
            continue;
        bool ok = true;
        for (auto& member : condition.members()) {
            if (std::none_of(std::begin(usable), std::end(usable), [&](const char* key) { return member.first == key; })) {
                ok = false;
                break;
            }
        }
        if (!ok)
            continue;

        std::vector<std::string> domains;
        const json::Value& filter = condition["urlFilter"];
        const json::Value& requestDomains = condition["requestDomains"];
        if (requestDomains.isArray() && filter.isNull()) {
            for (auto& domain : requestDomains.items()) {
                std::string name = domain.asString();
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (isPlainDomain(name))
                    domains.push_back(name);
            }
        } else if (filter.isString() && requestDomains.isNull()) {
            std::string name = domainFromURLFilter(filter.asString());
            if (!name.empty())
                domains.push_back(name);
        }
        if (domains.empty())
            continue;

        const std::string& type = action["type"].asString();
        if (type == "block")
            blocked.insert(blocked.end(), domains.begin(), domains.end());
        else if (type == "allow" || type == "allowAllRequests")
            allowed.insert(allowed.end(), domains.begin(), domains.end());
        else
            continue;
        ++used;
    }
    return used;
}

static std::mutex s_listsLock;
static std::map<std::string, std::shared_ptr<const Lists>> s_sources;
static std::unordered_set<std::string> s_userAllowed;
static std::atomic<uint64_t> s_blocked { 0 };

// Recently blocked hosts, newest at the back.
static std::mutex s_recentLock;
static std::vector<std::pair<std::string, uint32_t>> s_recent;

void setLists(const std::string& source, std::shared_ptr<const Lists> lists)
{
    std::lock_guard<std::mutex> lock(s_listsLock);
    if (lists && !lists->empty())
        s_sources[source] = std::move(lists);
    else
        s_sources.erase(source);
}

bool hasAnyLists()
{
    std::lock_guard<std::mutex> lock(s_listsLock);
    return !s_sources.empty();
}

void setUserAllowed(std::unordered_set<std::string> allowed)
{
    std::lock_guard<std::mutex> lock(s_listsLock);
    s_userAllowed = std::move(allowed);
}

static std::vector<std::string> s_topLevelSites; // most recent last, guarded by s_listsLock

static std::string normalizeHost(const std::string& rawHost)
{
    std::string host = rawHost;
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    while (!host.empty() && host.back() == '.')
        host.pop_back();
    return host;
}

void noteTopLevelSite(const std::string& rawHost)
{
    std::string host = normalizeHost(rawHost);
    if (host.empty())
        return;
    std::lock_guard<std::mutex> lock(s_listsLock);
    s_topLevelSites.erase(std::remove(s_topLevelSites.begin(), s_topLevelSites.end(), host), s_topLevelSites.end());
    s_topLevelSites.push_back(host);
    if (s_topLevelSites.size() > 48)
        s_topLevelSites.erase(s_topLevelSites.begin());
}

// Is a site on `domain` (or one of its subdomains) open in a tab?
static bool isOpenSite(const std::vector<std::string>& sites, const std::string& domain)
{
    for (auto& site : sites) {
        if (site == domain || (site.size() > domain.size() && site.compare(site.size() - domain.size() - 1, std::string::npos, "." + domain) == 0))
            return true;
    }
    return false;
}

// Calls `visit` with the host and each parent domain:
// "a.b.example.com", "b.example.com", "example.com", "com".
template<typename Visit>
static void forEachSuffix(const std::string& host, Visit visit)
{
    size_t pos = 0;
    for (;;) {
        if (!visit(host.substr(pos)))
            return;
        size_t dot = host.find('.', pos);
        if (dot == std::string::npos)
            return;
        pos = dot + 1;
    }
}

bool isBlocked(const std::string& rawHost, std::string* owner)
{
    std::map<std::string, std::shared_ptr<const Lists>> sources;
    std::vector<std::string> openSites;
    bool userAllowed = false;
    std::string host = normalizeHost(rawHost);
    {
        std::lock_guard<std::mutex> lock(s_listsLock);
        if (s_sources.empty())
            return false;
        sources = s_sources;
        openSites = s_topLevelSites;
        forEachSuffix(host, [&](const std::string& suffix) {
            userAllowed = userAllowed || s_userAllowed.count(suffix);
            return !userAllowed;
        });
    }
    if (userAllowed)
        return false;

    // Each source decides on its own (its allow rules only cancel its own
    // blocks); blocked if any source blocks.
    for (auto& [name, lists] : sources) {
        bool allowed = false;
        const std::string* blockedBy = nullptr;
        forEachSuffix(host, [&](const std::string& suffix) {
            if (lists->allowed.count(suffix)) {
                allowed = true;
                return false;
            }
            if (!blockedBy) {
                auto found = lists->blocked.find(suffix);
                if (found != lists->blocked.end() && found->second < lists->owners.size()
                    && !(lists->thirdPartyOnly.count(suffix) && isOpenSite(openSites, suffix)))
                    blockedBy = &lists->owners[found->second];
            }
            return true;
        });
        if (!allowed && blockedBy) {
            if (owner)
                *owner = *blockedBy;
            return true;
        }
    }
    return false;
}

std::vector<std::pair<std::string, uint32_t>> recentBlocked(size_t max)
{
    std::lock_guard<std::mutex> lock(s_recentLock);
    std::vector<std::pair<std::string, uint32_t>> out;
    for (auto it = s_recent.rbegin(); it != s_recent.rend() && out.size() < max; ++it)
        out.push_back(*it);
    return out;
}

uint64_t blockedCount()
{
    return s_blocked.load();
}

void recordBlocked(const std::string& rawHost)
{
    ++s_blocked;
    std::string host = normalizeHost(rawHost);
    std::lock_guard<std::mutex> lock(s_recentLock);
    uint32_t count = 1;
    for (auto it = s_recent.begin(); it != s_recent.end(); ++it) {
        if (it->first == host) {
            count = it->second + 1;
            s_recent.erase(it);
            break;
        }
    }
    s_recent.emplace_back(host, count);
    if (s_recent.size() > 100)
        s_recent.erase(s_recent.begin());
}

} // namespace blocker
