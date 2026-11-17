// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Blocker.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <iterator>
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
static std::shared_ptr<const Lists> s_lists;
static std::atomic<uint64_t> s_blocked { 0 };

void setLists(std::shared_ptr<const Lists> lists)
{
    std::lock_guard<std::mutex> lock(s_listsLock);
    s_lists = std::move(lists);
}

static std::shared_ptr<const Lists> currentLists()
{
    std::lock_guard<std::mutex> lock(s_listsLock);
    return s_lists;
}

bool isBlocked(const std::string& rawHost, std::string* owner)
{
    auto lists = currentLists();
    if (!lists || lists->empty())
        return false;

    std::string host = rawHost;
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    while (!host.empty() && host.back() == '.')
        host.pop_back();

    // Check "a.b.example.com", then "b.example.com", "example.com", "com".
    // An allow entry for the host or any parent wins over a block.
    const std::string* blockedBy = nullptr;
    size_t pos = 0;
    for (;;) {
        std::string suffix = host.substr(pos);
        if (lists->allowed.count(suffix))
            return false;
        if (!blockedBy) {
            auto found = lists->blocked.find(suffix);
            if (found != lists->blocked.end() && found->second < lists->owners.size())
                blockedBy = &lists->owners[found->second];
        }
        size_t dot = host.find('.', pos);
        if (dot == std::string::npos)
            break;
        pos = dot + 1;
    }
    if (blockedBy && owner)
        *owner = *blockedBy;
    return blockedBy != nullptr;
}

uint64_t blockedCount()
{
    return s_blocked.load();
}

void recordBlocked()
{
    ++s_blocked;
}

} // namespace blocker
