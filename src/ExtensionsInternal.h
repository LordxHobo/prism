// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// Shared between Extensions.cpp (installing, the registry, storage, content
// scripts) and ExtensionHost.cpp (background pages, popups, options pages,
// messaging and the chrome.* APIs they use). Not for other files.

#include "Json.h"
#include "WebKitApi.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace extensions {

struct ContentScript {
    std::vector<std::string> matches, excludeMatches, includeGlobs, excludeGlobs, js, css;
    std::string runAt { "document_idle" };
    bool allFrames { false };
};

struct Ruleset {
    std::string id, name, path;
    bool enabledByDefault { false };
};

struct Extension {
    std::string id;          // folder name and the ID scripts see
    std::string name, version, description, homepage;
    int manifestVersion { 2 };
    std::string source;      // chrome, firefox, file, folder
    std::string sourceURL;
    bool enabled { true };
    std::map<std::string, bool> rulesetChoices; // user's on/off per ruleset
    std::vector<ContentScript> contentScripts;
    std::vector<Ruleset> rulesets;
    std::vector<std::string> permissions, hosts;
    std::vector<std::string> optionalPermissions, optionalHosts;
    std::vector<std::string> granted; // optional permissions/hosts the user allowed
    bool hasBackground { false }, hasPopup { false }, hasOptions { false };
    bool unlimitedStorage { false };
    std::string iconDataURL;
    std::string geckoId;
    std::string manifestJSON;  // re-serialized, safe to embed in scripts
    std::string messagesJSON;  // the chosen locale's messages, re-serialized
    std::string uiLanguage;
    std::string token;         // proves messages come from this extension
    size_t blockingDomains { 0 };
    std::filesystem::path dir;

    // Background: an HTML page (MV2 "page"), a list of scripts (MV2
    // "scripts") or a service worker (MV3), which Prism runs as a page.
    std::string backgroundPage;
    std::vector<std::string> backgroundScripts;
    std::string serviceWorker;
    bool serviceWorkerModule { false };

    // Toolbar button: "action" (MV3), "browser_action" or "page_action".
    bool hasAction { false };
    std::string actionTitle, actionPopup, actionIcon; // icon: path in the extension

    std::string optionsPage;
    std::vector<std::string> unsupported; // features Prism can't do yet, in plain words

    bool rulesetEnabled(const Ruleset& ruleset) const
    {
        auto found = rulesetChoices.find(ruleset.id);
        return found != rulesetChoices.end() ? found->second : ruleset.enabledByDefault;
    }
    bool hasPermission(const std::string& permission) const;
};

namespace internal {

// Enabled extensions only.
std::vector<Extension*> enabledExtensions();
Extension* find(const std::string& id); // enabled or not
Extension* findEnabled(const std::string& id);

// file:///.../Extensions/<id>/ (with the trailing slash).
std::string baseURL(const Extension&);
// The extension an extension-page URL belongs to (or null), and the path
// inside it. Accepts file URLs and chrome-extension:// / moz-extension://.
Extension* extensionForURL(const std::string& url, std::string* path = nullptr);
// A file inside the extension, or empty if `relative` escapes its folder.
std::filesystem::path resolve(const Extension&, const std::string& relative);

// chrome.storage. `op` is storage.get/set/remove/clear/bytes.
std::string storage(Extension&, const std::string& op, const std::string& text, const json::Value& message, std::string& error);

// declarativeNetRequest rulesets.
void setRulesetEnabled(Extension&, const std::string& rulesetId, bool enabled);

// Optional permissions the user approved (permissions.request).
void grant(Extension&, const std::vector<std::string>& items);
void revoke(Extension&, const std::vector<std::string>& items);

// A script that runs `code` in a page like a content script (with the
// chrome.* API), for tabs.executeScript / scripting.executeScript files.
std::string injectionScript(const Extension&, const std::string& code);

// Re-reads every extension page script and the content scripts.
void contentScriptsChanged();

} // namespace internal

// ExtensionHost.cpp
namespace host {

// After extensions were installed, removed, turned on or off.
void extensionsChanged();
void installed(const std::string& id, bool update, const std::string& previousVersion);
void removed(const std::string& id);
// Closes the extension's background page and popups (before its files change).
void stop(const std::string& id);
// Content scripts' messages that aren't storage (messaging, ports, polling).
void handleContentMessage(Extension&, WKScriptMessageRef, const std::string& text, const json::Value& message, WKCompletionListenerRef reply);
// Storage changes go to every part of the extension.
void storageChanged(Extension&, const std::string& area, const std::string& changesJSON, int fromContext);

} // namespace host

} // namespace extensions
