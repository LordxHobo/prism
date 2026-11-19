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
// change so the cache starts over. Requests an extension watches with
// webRequest aren't cached (the extension decides each time), may be
// redirected, and Prism hears how they ended.
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

// Frame IDs as extensions see them: 0 for a page's main frame, then a
// number per frame (kept for the frame's life in this process).
std::unordered_map<WKBundleFrameRef, int> g_frameIds;
int g_nextFrameId = 1;

int frameIdOf(WKBundleFrameRef frame)
{
    if (!frame || WKBundleFrameIsMainFrame(frame))
        return 0;
    auto found = g_frameIds.find(frame);
    if (found != g_frameIds.end())
        return found->second;
    if (g_frameIds.size() > 4096)
        g_frameIds.clear();
    int id = g_nextFrameId++;
    g_frameIds.emplace(frame, id);
    return id;
}

int parentFrameIdOf(WKBundleFrameRef frame)
{
    if (!frame || WKBundleFrameIsMainFrame(frame))
        return -1;
    WKBundleFrameRef parent = WKBundleFrameGetParentFrame ? WKBundleFrameGetParentFrame(frame) : nullptr;
    return parent ? frameIdOf(parent) : 0;
}

// Requests Prism asked to hear the end of (extensions' webRequest
// onCompleted / onErrorOccurred): resource id -> Prism's request id.
std::unordered_map<uint64_t, std::string> g_tracked;

struct Answer {
    char action = '0';    // '0' let through, '1' block, 'r' redirect
    bool cacheable = true;
    std::string requestId; // set when Prism wants to hear how it ended
    std::string redirectURL;
};

// Prism's reply: "<action>\t<flags>\t<request id>\t<redirect URL>".
Answer parseAnswer(const std::string& text)
{
    Answer answer;
    if (text.empty())
        return answer;
    answer.action = text[0];
    size_t a = text.find('\t');
    if (a == std::string::npos)
        return answer;
    size_t b = text.find('\t', a + 1);
    std::string flags = text.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    answer.cacheable = flags.find('n') == std::string::npos;
    if (b == std::string::npos)
        return answer;
    size_t c = text.find('\t', b + 1);
    answer.requestId = text.substr(b + 1, c == std::string::npos ? std::string::npos : c - b - 1);
    if (c != std::string::npos)
        answer.redirectURL = text.substr(c + 1);
    return answer;
}

// Asks Prism about one request.
Answer askPrism(WKBundlePageRef page, WKBundleFrameRef frame, uint64_t resourceId, const char* kind,
    const std::string& firstParty, const std::string& url, WKURLRequestRef request)
{
    std::string key = std::string(kind) + '\n' + firstParty + '\n' + url;
    auto cached = g_decisions.find(key);
    if (cached != g_decisions.end()) {
        Answer answer;
        answer.action = cached->second ? '1' : '0';
        return answer;
    }

    std::string method;
    if (WKStringRef string = WKURLRequestCopyHTTPMethod(request)) {
        method = toUTF8(string);
        WKRelease(string);
    }
    std::string frameURL = takeURL(WKBundleFrameCopyURL(frame));
    // "<page id>\t<kind>\t<first party>\t<url>\t<resource id>\t<frame id>\t<parent frame id>\t<method>\t<frame url>"
    std::string message = std::to_string(pageIdentifier(page)) + '\t' + kind + '\t' + firstParty + '\t' + url
        + '\t' + std::to_string(resourceId) + '\t' + std::to_string(frameIdOf(frame)) + '\t' + std::to_string(parentFrameIdOf(frame))
        + '\t' + method + '\t' + frameURL;
    WKStringRef name = WKStringCreateWithUTF8CString("prism:check");
    WKStringRef body = WKStringCreateWithUTF8CString(message.c_str());
    WKTypeRef reply = nullptr;
    WKBundlePostSynchronousMessage(g_bundle, name, body, &reply);
    WKRelease(body);
    WKRelease(name);
    Answer answer = parseAnswer(bodyString(reply));
    if (reply)
        WKRelease(reply);

    if (answer.cacheable && (answer.action == '0' || answer.action == '1')) {
        if (g_decisions.size() >= kMaxDecisions)
            g_decisions.clear();
        g_decisions.emplace(std::move(key), answer.action == '1');
    }
    return answer;
}

void tellPrism(const char* what, const std::string& text)
{
    WKStringRef name = WKStringCreateWithUTF8CString(what);
    WKStringRef body = WKStringCreateWithUTF8CString(text.c_str());
    WKBundlePostMessage(g_bundle, name, body);
    WKRelease(body);
    WKRelease(name);
}

WKURLRequestRef willSendRequest(WKBundlePageRef page, WKBundleFrameRef frame, uint64_t resourceId, WKURLRequestRef request, WKURLResponseRef redirectResponse, const void*)
{
    auto allow = [&] { return static_cast<WKURLRequestRef>(WKRetain(request)); };
    if (!g_filtering || !request)
        return allow();

    std::string url = takeURL(WKURLRequestCopyURL(request));
    std::string host = requestkind::hostOf(url);
    if (host.empty() || requestkind::isLocalHost(host))
        return allow(); // not http(s), or this PC (also Prism's own redirects)

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
    Answer answer = askPrism(page, frame, resourceId, kind, firstParty, url, request);
    if (!answer.requestId.empty()) {
        if (g_tracked.size() > 4096)
            g_tracked.clear();
        g_tracked[resourceId] = answer.requestId;
    }
    if (answer.action == '1')
        return nullptr; // cancels the load
    if (answer.action == 'r' && !answer.redirectURL.empty()) {
        if (WKURLRef target = WKURLCreateWithUTF8CString(answer.redirectURL.c_str())) {
            WKURLRequestRef replacement = WKURLRequestCreateWithWKURL(target);
            WKRelease(target);
            if (replacement)
                return replacement; // already retained (created)
        }
    }
    return allow();
}

void didFinishLoad(WKBundlePageRef, WKBundleFrameRef, uint64_t resourceId, const void*)
{
    auto found = g_tracked.find(resourceId);
    if (found == g_tracked.end())
        return;
    tellPrism("prism:done", found->second + "\t1\t");
    g_tracked.erase(found);
}

void didFailLoad(WKBundlePageRef, WKBundleFrameRef, uint64_t resourceId, WKErrorRef error, const void*)
{
    auto found = g_tracked.find(resourceId);
    if (found == g_tracked.end())
        return;
    std::string description = "net::ERR_FAILED";
    if (error && WKErrorCopyLocalizedDescription) {
        if (WKStringRef text = WKErrorCopyLocalizedDescription(error)) {
            std::string value = toUTF8(text);
            WKRelease(text);
            if (!value.empty())
                description = value;
        }
    }
    tellPrism("prism:done", found->second + "\t0\t" + description);
    g_tracked.erase(found);
}

WKBundlePageResourceLoadClientV0 g_resourceClient = {
    { 0, nullptr },
    nullptr,
    willSendRequest,
    nullptr,
    nullptr,
    didFinishLoad,
    didFailLoad,
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
