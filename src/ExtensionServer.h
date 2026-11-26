// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// The local web server that gives every extension its own address,
// http://<extension>.localhost:<port>/ (see ExtensionServer.cpp).
namespace extensions::server {

struct Site {
    std::string id;
    std::filesystem::path dir;
    std::vector<std::string> webAccessible; // paths (globs) web pages may load
    // Folders served under a first path segment ("files" -> files/...).
    std::vector<std::pair<std::string, std::filesystem::path>> mounts;
};

// Starts the server (once) and returns its port, or 0 if it couldn't.
uint16_t start();
uint16_t port();

// The extensions it serves (call after every change).
void setSites(std::vector<Site>);

// Prism's own pages (the translator), kept apart from the extensions.
void setPrismSite(Site);

// The host-name label for an extension ID.
std::string hostLabel(const std::string& id);

} // namespace extensions::server
