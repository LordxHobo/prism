// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "UserCSS.h"

#include "Engine.h"
#include "Json.h"
#include "Util.h"

#include <vector>

namespace usercss {

namespace {

constexpr size_t kMaxCSS = 512 * 1024;

struct SiteRule {
    std::string sites; // one per line or comma-separated; "*" = every site
    std::string css;
    bool enabled { true };
};

std::string g_interface;
std::vector<SiteRule> g_rules;
std::function<void()> g_interfaceChanged;

std::wstring interfaceFile() { return util::dataDirectory() + L"\\interface.css"; }
std::wstring rulesFile() { return util::dataDirectory() + L"\\site-styles.json"; }

std::string rulesJSON()
{
    std::string json = "[";
    for (size_t i = 0; i < g_rules.size(); ++i) {
        json += (i ? "," : "") + std::string("{\"sites\":") + json::quote(g_rules[i].sites) + ",\"css\":" + json::quote(g_rules[i].css)
            + ",\"enabled\":" + (g_rules[i].enabled ? "true" : "false") + "}";
    }
    return json + "]";
}

std::vector<std::string> siteList(const std::string& text)
{
    std::vector<std::string> sites;
    std::string current;
    for (char c : text + "\n") {
        if (c == '\n' || c == ',' || c == ' ' || c == '\t' || c == '\r') {
            if (!current.empty())
                sites.push_back(current);
            current.clear();
        } else
            current += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return sites;
}

// One user script for all rules. It only runs on websites (http/https),
// never on Prism's own pages, and adds a <style> for each matching rule.
void apply()
{
    std::string rules = "[";
    bool any = false;
    for (auto& rule : g_rules) {
        if (!rule.enabled || rule.css.empty())
            continue;
        std::string sites = "[";
        for (auto& site : siteList(rule.sites)) {
            // "*.example.com" means the same as "example.com" (and its subdomains).
            std::string clean = site.rfind("*.", 0) == 0 ? site.substr(2) : site;
            if (clean.rfind("http://", 0) == 0) clean = clean.substr(7);
            if (clean.rfind("https://", 0) == 0) clean = clean.substr(8);
            if (size_t slash = clean.find('/'); slash != std::string::npos) clean = clean.substr(0, slash);
            sites += (sites.size() > 1 ? "," : "") + json::quote(clean);
        }
        sites += "]";
        if (sites == "[]")
            continue;
        rules += (any ? "," : "") + std::string("{\"sites\":") + sites + ",\"css\":" + json::quote(rule.css) + "}";
        any = true;
    }
    rules += "]";
    std::vector<std::string> scripts;
    if (any) {
        scripts.push_back("(function (rules) {\n"
            "  if (location.protocol !== 'http:' && location.protocol !== 'https:') return;\n"
            "  var host = location.hostname.toLowerCase();\n"
            "  var css = '';\n"
            "  rules.forEach(function (rule) {\n"
            "    var match = rule.sites.some(function (site) {\n"
            "      return site === '*' || host === site || host.slice(-site.length - 1) === '.' + site;\n"
            "    });\n"
            "    if (match) css += rule.css + '\\n';\n"
            "  });\n"
            "  if (!css) return;\n"
            "  var style = document.createElement('style');\n"
            "  style.setAttribute('data-prism', 'site-css');\n"
            "  style.textContent = css;\n"
            "  (document.head || document.documentElement).appendChild(style);\n"
            "})(" + rules + ");");
    }
    engine::setUserScripts("usercss", std::move(scripts));
}

void saveRules()
{
    util::writeFileAtomically(rulesFile(), rulesJSON());
}

} // namespace

void initialize(std::function<void()> interfaceChanged)
{
    g_interfaceChanged = std::move(interfaceChanged);
    g_interface = util::readFile(interfaceFile());
    json::Value root;
    if (json::parse(util::readFile(rulesFile()), root)) {
        for (auto& item : root.items())
            g_rules.push_back({ item["sites"].asString(), item["css"].asString(), item["enabled"].asBool(true) });
    }
    apply();
}

const std::string& interfaceCSS()
{
    return g_interface;
}

std::string handleCommand(const std::wstring& command, const std::wstring& argument)
{
    if (command == L"get")
        return "{\"interface\":" + json::quote(g_interface) + ",\"sites\":" + rulesJSON() + "}";
    if (command == L"setInterface") {
        std::string css = util::toUTF8(argument);
        if (css.size() > kMaxCSS)
            return "false";
        g_interface = css;
        if (css.empty())
            DeleteFileW(interfaceFile().c_str());
        else
            util::writeFileAtomically(interfaceFile(), css);
        if (g_interfaceChanged)
            g_interfaceChanged();
        return "true";
    }
    if (command == L"setSites") {
        // [{"sites": "...", "css": "...", "enabled": bool}]
        json::Value list;
        if (!json::parse(util::toUTF8(argument), list) || !list.isArray())
            return "false";
        std::vector<SiteRule> rules;
        size_t total = 0;
        for (auto& item : list.items()) {
            SiteRule rule { item["sites"].asString(), item["css"].asString(), item["enabled"].asBool(true) };
            total += rule.css.size() + rule.sites.size();
            rules.push_back(std::move(rule));
        }
        if (total > kMaxCSS || rules.size() > 200)
            return "false";
        g_rules = std::move(rules);
        saveRules();
        apply();
        return "true";
    }
    return "null";
}

} // namespace usercss
