// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "BundleApi.h"

#include <string>

// Extension content scripts in isolated worlds (see ContentScripts.cpp).
namespace content {

// Loads what it needs and asks Prism for the scripts. False if this WebKit
// lacks it (Prism then runs content scripts in the page's world).
bool initialize(WKBundleRef);
void pageCreated(WKBundlePageRef);
void pageDestroyed(WKBundlePageRef);
// "prism:contentScripts" (to the whole process).
void scriptsChanged(const std::string& payload);
// "prism:contentReply" (to a page).
void reply(const std::string& body);
// "prism:exec" (to a page): executeScript in the extension's world.
void execute(WKBundlePageRef, const std::string& body);

} // namespace content
