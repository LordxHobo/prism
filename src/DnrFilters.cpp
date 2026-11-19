// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "DnrFilters.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace dnr {

namespace {

const char* adblockType(const std::string& type)
{
    if (type == "main_frame") return "document";
    if (type == "sub_frame") return "subdocument";
    if (type == "stylesheet" || type == "script" || type == "image" || type == "font" || type == "object"
        || type == "xmlhttprequest" || type == "ping" || type == "media" || type == "websocket" || type == "other")
        return type.c_str();
    if (type == "csp_report" || type == "webtransport" || type == "webbundle")
        return "other";
    return nullptr;
}

// Lower-case domain names, or false if one isn't usable in a filter.
bool domainList(const json::Value& list, std::vector<std::string>& out)
{
    for (auto& item : list.items()) {
        std::string name = item.asString();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name.empty() || name.find_first_of("|,$~/ ") != std::string::npos)
            return false;
        out.push_back(name);
    }
    return true;
}

bool hasOnly(const json::Value& object, std::initializer_list<const char*> keys)
{
    for (auto& member : object.members()) {
        if (std::none_of(keys.begin(), keys.end(), [&](const char* key) { return member.first == key; }))
            return false;
    }
    return true;
}

} // namespace

std::string ruleToFilter(const json::Value& rule)
{
    const json::Value& action = rule["action"];
    const json::Value& condition = rule["condition"];
    if (!action.isObject() || !condition.isObject())
        return { };
    if (!hasOnly(condition, { "urlFilter", "regexFilter", "isUrlFilterCaseSensitive", "initiatorDomains", "excludedInitiatorDomains",
        "domains", "excludedDomains", "requestDomains", "resourceTypes", "excludedResourceTypes", "domainType" }))
        return { };

    const std::string& actionType = action["type"].asString();
    std::string prefix;
    if (actionType == "block")
        prefix = "";
    else if (actionType == "allow" || actionType == "allowAllRequests")
        prefix = "@@";
    else
        return { }; // redirect, upgradeScheme, modifyHeaders

    // What to match.
    const json::Value& urlFilter = condition["urlFilter"];
    const json::Value& regexFilter = condition["regexFilter"];
    const json::Value& requestDomains = condition["requestDomains"];
    std::vector<std::string> patterns;
    bool isRegex = false;
    if (urlFilter.isString()) {
        if (!requestDomains.isNull())
            return { }; // both must match: no adblock equivalent
        std::string pattern = urlFilter.asString();
        if (pattern.empty() || pattern.find_first_of("$\n\r") != std::string::npos)
            return { };
        // "/ads/" would read as a regular expression in adblock syntax.
        if (pattern.size() > 1 && pattern.front() == '/' && pattern.back() == '/')
            pattern += '*';
        patterns.push_back(pattern);
    } else if (regexFilter.isString()) {
        if (!requestDomains.isNull())
            return { };
        std::string pattern = regexFilter.asString();
        if (pattern.empty() || pattern.find_first_of("\n\r") != std::string::npos)
            return { };
        // A "$" would start the filter's options in adblock syntax: write
        // the end-of-address anchor as \z (same meaning in adblock-rust's
        // regular expressions) and skip rules with any other "$".
        if (pattern.back() == '$' && (pattern.size() < 2 || pattern[pattern.size() - 2] != '\\'))
            pattern.replace(pattern.size() - 1, 1, "\\z");
        if (pattern.find('$') != std::string::npos)
            return { };
        patterns.push_back("/" + pattern + "/");
        isRegex = true;
    } else if (requestDomains.isArray()) {
        std::vector<std::string> domains;
        if (!domainList(requestDomains, domains) || domains.empty())
            return { };
        for (auto& domain : domains)
            patterns.push_back("||" + domain + "^");
    } else {
        patterns.push_back("*");
    }

    // Options.
    std::vector<std::string> options;
    const json::Value& types = condition["resourceTypes"];
    const json::Value& excludedTypes = condition["excludedResourceTypes"];
    if (types.isArray()) {
        for (auto& type : types.items()) {
            const char* name = adblockType(type.asString());
            if (!name)
                return { };
            if (std::find(options.begin(), options.end(), name) == options.end())
                options.push_back(name);
        }
    } else if (excludedTypes.isArray()) {
        for (auto& type : excludedTypes.items()) {
            const char* name = adblockType(type.asString());
            if (!name)
                return { };
            // adblock-rust doesn't accept "~document", and rules without a
            // type never apply to pages themselves there anyway.
            if (std::string(name) != "document")
                options.push_back(std::string("~") + name);
        }
    } else if (actionType == "allowAllRequests") {
        return { }; // needs resourceTypes (main_frame/sub_frame)
    }
    // Rules without types match everything but pages themselves, which is
    // what adblock filters without a type do too.

    std::vector<std::string> domains;
    const json::Value& initiators = condition["initiatorDomains"].isNull() ? condition["domains"] : condition["initiatorDomains"];
    const json::Value& excludedInitiators = condition["excludedInitiatorDomains"].isNull() ? condition["excludedDomains"] : condition["excludedInitiatorDomains"];
    std::vector<std::string> included, excluded;
    if (!domainList(initiators, included) || !domainList(excludedInitiators, excluded))
        return { };
    if (initiators.isArray() && included.empty())
        return { }; // an empty list matches nothing
    for (auto& name : included)
        domains.push_back(name);
    for (auto& name : excluded)
        domains.push_back("~" + name);
    if (!domains.empty()) {
        std::string option = "domain=";
        for (size_t i = 0; i < domains.size(); ++i)
            option += (i ? "|" : "") + domains[i];
        options.push_back(option);
    }

    const std::string& domainType = condition["domainType"].asString();
    if (domainType == "thirdParty")
        options.push_back("third-party");
    else if (domainType == "firstParty")
        options.push_back("~third-party");
    else if (!domainType.empty())
        return { };

    if (condition["isUrlFilterCaseSensitive"].isBool() && condition["isUrlFilterCaseSensitive"].asBool() && !isRegex)
        options.push_back("match-case");

    std::string optionText;
    for (size_t i = 0; i < options.size(); ++i)
        optionText += (i ? "," : "") + options[i];

    std::string out;
    for (auto& pattern : patterns) {
        if (!out.empty())
            out += '\n';
        out += prefix + pattern + (optionText.empty() ? "" : "$" + optionText);
    }
    return out;
}

void convert(const json::Value& rules, Conversion& out)
{
    for (auto& rule : rules.items()) {
        std::string filter = ruleToFilter(rule);
        if (filter.empty()) {
            ++out.skipped;
            continue;
        }
        out.filters += filter;
        out.filters += '\n';
        ++out.converted;
    }
}

} // namespace dnr
