// Prism — PrismBundle.dll, loaded by WebKit into every web content process.
//
// WebKit calls WKBundleInitialize when a content process starts. The bundle
// then watches every request each page makes (pictures, scripts, frames,
// fetches...) and asks Prism whether to let it through. Prism answers with
// Prism Shields' filter lists and content-blocking extensions' rules, which
// can see the full address of each request this way (the filtering proxy
// only sees which site a connection is for).
//
// Answers are cached per page address and request, so each one costs a
// round trip to Prism only once. Prism tells the bundle when its rules
// change so the cache starts over.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "BundleApi.h"
#include "RequestKind.h"

#include <windows.h>

#include <string>
#include <unordered_map>

namespace {

#define PRISM_BUNDLE_DEFINE(name, ret, params) ret (*name) params = nullptr;
PRISM_BUNDLE_REQUIRED(PRISM_BUNDLE_DEFINE)
PRISM_BUNDLE_OPTIONAL(PRISM_BUNDLE_DEFINE)
#undef PRISM_BUNDLE_DEFINE

WKBundleRef g_bundle = nullptr;
bool g_filtering = false;
unsigned long g_generation = 0;
std::unordered_map<std::string, bool> g_decisions; // cache key -> block
constexpr size_t kMaxDecisions = 8192;

bool loadFunctions()
{
    HMODULE module = GetModuleHandleW(L"WebKit2.dll");
    if (!module)
        module = GetModuleHandleW(L"WebKit.dll");
    if (!module)
        return false;
    bool ok = true;
#define PRISM_BUNDLE_LOAD_REQUIRED(name, ret, params) \
    name = reinterpret_cast<ret (*) params>(reinterpret_cast<void*>(GetProcAddress(module, #name))); \
    if (!name) ok = false;
    PRISM_BUNDLE_REQUIRED(PRISM_BUNDLE_LOAD_REQUIRED)
#undef PRISM_BUNDLE_LOAD_REQUIRED
#define PRISM_BUNDLE_LOAD_OPTIONAL(name, ret, params) \
    name = reinterpret_cast<ret (*) params>(reinterpret_cast<void*>(GetProcAddress(module, #name)));
    PRISM_BUNDLE_OPTIONAL(PRISM_BUNDLE_LOAD_OPTIONAL)
#undef PRISM_BUNDLE_LOAD_OPTIONAL
    return ok;
}

std::string toUTF8(WKStringRef string)
{
    if (!string)
        return { };
    size_t size = WKStringGetMaximumUTF8CStringSize(string);
    std::string out(size, '\0');
    size_t length = WKStringGetUTF8CString(string, out.data(), size);
    out.resize(length ? length - 1 : 0); // length includes the terminator
    return out;
}

std::string takeURL(WKURLRef url)
{
    if (!url)
        return { };
    WKStringRef string = WKURLCopyString(url);
    std::string out = toUTF8(string);
    if (string)
        WKRelease(string);
    WKRelease(url);
    return out;
}

std::string bodyString(WKTypeRef body)
{
    if (!body || WKGetTypeID(body) != WKStringGetTypeID())
        return { };
    return toUTF8(static_cast<WKStringRef>(body));
}

// Settings from Prism: "<1|0>\t<generation>".
void applyConfig(const std::string& text)
{
    size_t tab = text.find('\t');
    g_filtering = !text.empty() && text[0] == '1';
    unsigned long generation = tab == std::string::npos ? 0 : std::strtoul(text.c_str() + tab + 1, nullptr, 10);
    if (generation != g_generation) {
        g_generation = generation;
        g_decisions.clear();
    }
}

uint64_t pageIdentifier(WKBundlePageRef page)
{
    return WKBundlePageGetWebPageProxyIdentifierForTesting ? WKBundlePageGetWebPageProxyIdentifierForTesting(page) : 0;
}

// Asks Prism. Returns true to block.
bool askPrism(WKBundlePageRef page, const char* kind, const std::string& firstParty, const std::string& url)
{
    std::string key = std::string(kind) + '\n' + firstParty + '\n' + url;
    auto cached = g_decisions.find(key);
    if (cached != g_decisions.end())
        return cached->second;

    std::string message = std::to_string(pageIdentifier(page)) + '\t' + kind + '\t' + firstParty + '\t' + url;
    WKStringRef name = WKStringCreateWithUTF8CString("prism:check");
    WKStringRef body = WKStringCreateWithUTF8CString(message.c_str());
    WKTypeRef reply = nullptr;
    WKBundlePostSynchronousMessage(g_bundle, name, body, &reply);
    WKRelease(body);
    WKRelease(name);
    bool block = bodyString(reply) == "1";
    if (reply)
        WKRelease(reply);

    if (g_decisions.size() >= kMaxDecisions)
        g_decisions.clear();
    g_decisions.emplace(std::move(key), block);
    return block;
}

WKURLRequestRef willSendRequest(WKBundlePageRef page, WKBundleFrameRef frame, uint64_t, WKURLRequestRef request, WKURLResponseRef redirectResponse, const void*)
{
    auto allow = [&] { return static_cast<WKURLRequestRef>(WKRetain(request)); };
    if (!g_filtering || !request)
        return allow();

    std::string url = takeURL(WKURLRequestCopyURL(request));
    std::string host = requestkind::hostOf(url);
    if (host.empty() || requestkind::isLocalHost(host))
        return allow(); // not http(s), or this PC

    // The site in the address bar. Requests made by Prism's own pages
    // (toolbar, extension pages) and by local pages aren't filtered.
    std::string firstParty = takeURL(WKURLRequestCopyFirstPartyForCookies(request));
    std::string topHost = requestkind::hostOf(firstParty);
    if (topHost.empty() || requestkind::isLocalHost(topHost))
        return allow();

    // A frame's own document (or a redirect of it).
    std::string provisional = takeURL(WKBundleFrameCopyProvisionalURL(frame));
    bool isDocument = !provisional.empty()
        && (provisional == url || (redirectResponse && takeURL(WKURLResponseCopyURL(redirectResponse)) == provisional));
    if (isDocument && WKBundleFrameIsMainFrame(frame))
        return allow(); // pages themselves are checked by Prism before they load

    const char* kind = isDocument ? "subdocument" : requestkind::guess(url);
    if (askPrism(page, kind, firstParty, url))
        return nullptr; // cancels the load
    return allow();
}

WKBundlePageResourceLoadClientV0 g_resourceClient = {
    { 0, nullptr },
    nullptr,
    willSendRequest,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

void didCreatePage(WKBundleRef, WKBundlePageRef page, const void*)
{
    WKBundlePageSetResourceLoadClient(page, &g_resourceClient.base);
    // Lets Prism match this page's requests to its tab.
    WKStringRef name = WKStringCreateWithUTF8CString("prism:page");
    WKStringRef body = WKStringCreateWithUTF8CString(std::to_string(pageIdentifier(page)).c_str());
    WKBundlePagePostMessage(page, name, body);
    WKRelease(body);
    WKRelease(name);
}

void didReceiveMessage(WKBundleRef, WKStringRef name, WKTypeRef body, const void*)
{
    if (toUTF8(name) == "prism:filter")
        applyConfig(bodyString(body));
}

WKBundleClientV1 g_bundleClient = {
    { 1, nullptr },
    didCreatePage,
    nullptr,
    nullptr,
    didReceiveMessage,
    nullptr,
};

} // namespace

extern "C" __declspec(dllexport) void WKBundleInitialize(WKBundleRef bundle, WKTypeRef initializationUserData)
{
    if (!loadFunctions())
        return; // a WebKit this bundle doesn't know: pages just aren't filtered
    g_bundle = bundle;
    applyConfig(bodyString(initializationUserData));
    WKBundleSetClient(bundle, &g_bundleClient.base);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
