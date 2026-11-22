// Prism — extension content scripts in their own JavaScript worlds.
//
// Like Chrome, each extension's content scripts run in an isolated world:
// they share the page's DOM but not its JavaScript, so a page can't see,
// hook or tamper with them (and they can't be confused by the page's own
// globals). WebKit's UI-process API can only add scripts to the page's
// world, so the bundle does it: when a frame gets a new window object, it
// runs every extension's content script runtime (ui/extension-runtime.js
// with that extension's scripts; it checks the URL itself) in a world of
// that extension's own.
//
// The scripts talk to Prism through a native function, __prismNative(text,
// id), only defined in their world. Messages go to Prism as page messages
// ("prism:content"); answers come back as messages to the page
// ("prism:contentReply") and are handed to the world's __prismReceive(id,
// text).
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ContentScripts.h"

#include <windows.h>

#include <map>
#include <string>
#include <vector>

#define PRISM_BUNDLE_DECLARE(name, ret, params) extern ret (*name) params;
PRISM_BUNDLE_REQUIRED(PRISM_BUNDLE_DECLARE)
#undef PRISM_BUNDLE_DECLARE

namespace content {

namespace {

#define PRISM_CONTENT_DEFINE(name, ret, params) ret (*name) params = nullptr;
PRISM_BUNDLE_CONTENT(PRISM_CONTENT_DEFINE)
PRISM_JSC(PRISM_CONTENT_DEFINE)
#undef PRISM_CONTENT_DEFINE

bool g_available = false;
WKBundleRef g_bundle = nullptr;

struct Script {
    std::string extensionId;
    std::string source;
};
std::vector<Script> g_scripts;
std::map<std::string, WKBundleScriptWorldRef> g_worlds; // per extension, kept for the process's life

// One extension's world in one document.
struct Document {
    JSGlobalContextRef context { nullptr }; // retained
    WKBundlePageRef page { nullptr };
    WKBundleFrameRef frame { nullptr };
    std::string extensionId;
    bool mainFrame { false };
};
std::map<unsigned long long, Document> g_documents;
std::map<JSGlobalContextRef, unsigned long long> g_byContext;
unsigned long long g_nextDocument = 1;
constexpr size_t kMaxDocuments = 2000;

std::string toUTF8(JSStringRef string)
{
    if (!string)
        return { };
    size_t size = JSStringGetMaximumUTF8CStringSize(string);
    std::string out(size, '\0');
    size_t length = JSStringGetUTF8CString(string, out.data(), size);
    out.resize(length ? length - 1 : 0);
    return out;
}

void post(WKBundlePageRef page, const char* name, const std::string& body)
{
    WKStringRef nameString = WKStringCreateWithUTF8CString(name);
    WKStringRef bodyString = WKStringCreateWithUTF8CString(body.c_str());
    WKBundlePagePostMessage(page, nameString, bodyString);
    WKRelease(bodyString);
    WKRelease(nameString);
}

void forget(unsigned long long key)
{
    auto found = g_documents.find(key);
    if (found == g_documents.end())
        return;
    g_byContext.erase(found->second.context);
    JSGlobalContextRelease(found->second.context);
    g_documents.erase(found);
}

void forgetWhere(bool (*matches)(const Document&, const void*), const void* data)
{
    std::vector<unsigned long long> keys;
    for (auto& [key, document] : g_documents) {
        if (matches(document, data))
            keys.push_back(key);
    }
    for (auto key : keys)
        forget(key);
}

// __prismNative(text, id): a message from the extension's scripts to Prism.
JSValueRef nativeCall(JSContextRef context, JSObjectRef, JSObjectRef, size_t count, const JSValueRef arguments[], JSValueRef* exception)
{
    JSValueRef undefined = JSValueMakeUndefined(context);
    if (count < 2)
        return undefined;
    auto found = g_byContext.find(JSContextGetGlobalContext(context));
    if (found == g_byContext.end())
        return undefined;
    const Document& document = g_documents[found->second];
    JSStringRef text = JSValueToStringCopy(context, arguments[0], exception);
    if (!text)
        return undefined;
    std::string message = toUTF8(text);
    JSStringRelease(text);
    double id = JSValueToNumber(context, arguments[1], exception);
    // "<document>\t<1 = main frame>\t<extension>\t<call id>\t<message>"
    post(document.page, "prism:content", std::to_string(found->second) + "\t" + (document.mainFrame ? "1" : "0") + "\t" + document.extensionId
        + "\t" + std::to_string(static_cast<long long>(id)) + "\t" + message);
    return undefined;
}

// The extension's world in a frame's current document, with __prismNative
// in it (made the first time).
JSGlobalContextRef documentFor(WKBundlePageRef page, WKBundleFrameRef frame, const std::string& extensionId)
{
    WKBundleScriptWorldRef& world = g_worlds[extensionId];
    if (!world)
        world = WKBundleScriptWorldCreateWorld();
    if (!world)
        return nullptr;
    JSGlobalContextRef context = WKBundleFrameGetJavaScriptContextForWorld(frame, world);
    if (!context)
        return nullptr;
    if (g_byContext.count(context))
        return context;
    while (g_documents.size() >= kMaxDocuments)
        forget(g_documents.begin()->first);
    unsigned long long key = g_nextDocument++;
    Document document;
    document.context = JSGlobalContextRetain(context);
    document.page = page;
    document.frame = frame;
    document.extensionId = extensionId;
    document.mainFrame = WKBundleFrameIsMainFrame(frame);
    g_byContext[document.context] = key;
    g_documents[key] = document;

    JSObjectRef global = JSContextGetGlobalObject(context);
    JSStringRef name = JSStringCreateWithUTF8CString("__prismNative");
    JSObjectRef function = JSObjectMakeFunctionWithCallback(context, name, nativeCall);
    JSObjectSetProperty(context, global, name, function, kPrismJSReadOnlyDontEnumDontDelete, nullptr);
    JSStringRelease(name);
    return context;
}

void runScripts(WKBundlePageRef page, WKBundleFrameRef frame)
{
    // This frame's previous documents are gone.
    forgetWhere([](const Document& document, const void* data) { return document.frame == static_cast<WKBundleFrameRef>(data); }, frame);
    for (auto& script : g_scripts) {
        JSGlobalContextRef context = documentFor(page, frame, script.extensionId);
        if (!context)
            continue;
        JSStringRef source = JSStringCreateWithUTF8CString(script.source.c_str());
        JSStringRef url = JSStringCreateWithUTF8CString(("prism-extension://" + script.extensionId + "/content-scripts.js").c_str());
        JSValueRef exception = nullptr;
        JSEvaluateScript(context, source, nullptr, url, 1, &exception);
        JSStringRelease(url);
        JSStringRelease(source);
    }
}

void didClearWindowObject(WKBundlePageRef page, WKBundleFrameRef frame, WKBundleScriptWorldRef world, const void*)
{
    // Called once per world that has a window; act on the page's own world,
    // which every new document has.
    if (world != WKBundleScriptWorldNormalWorld() || g_scripts.empty())
        return;
    runScripts(page, frame);
}

void didRemoveFrame(WKBundlePageRef, WKBundleFrameRef frame, WKTypeRef*, const void*)
{
    forgetWhere([](const Document& document, const void* data) { return document.frame == static_cast<WKBundleFrameRef>(data); }, frame);
}

WKBundlePageLoaderClientV0 makeLoaderClient()
{
    WKBundlePageLoaderClientV0 client = { };
    client.base.version = 0;
    client.beforeClearWindowObject[11] = reinterpret_cast<void*>(didRemoveFrame); // didRemoveFrameFromHierarchy
    client.didClearWindowObjectForFrame = didClearWindowObject;
    return client;
}
WKBundlePageLoaderClientV0 g_loaderClient = makeLoaderClient();

// "<count>\n" then per script "<extension id>\t<byte length>\n<source>".
void parseScripts(const std::string& payload)
{
    std::vector<Script> scripts;
    size_t at = payload.find('\n');
    if (at == std::string::npos)
        return;
    int count = atoi(payload.substr(0, at).c_str());
    at++;
    for (int i = 0; i < count && at < payload.size(); i++) {
        size_t tab = payload.find('\t', at);
        size_t newline = payload.find('\n', at);
        if (tab == std::string::npos || newline == std::string::npos || tab > newline)
            break;
        Script script;
        script.extensionId = payload.substr(at, tab - at);
        size_t length = static_cast<size_t>(_strtoui64(payload.substr(tab + 1, newline - tab - 1).c_str(), nullptr, 10));
        at = newline + 1;
        if (at + length > payload.size())
            break;
        script.source = payload.substr(at, length);
        at += length;
        scripts.push_back(std::move(script));
    }
    g_scripts = std::move(scripts);
}

} // namespace

bool initialize(WKBundleRef bundle)
{
    HMODULE webkit = GetModuleHandleW(L"WebKit2.dll");
    if (!webkit)
        webkit = GetModuleHandleW(L"WebKit.dll");
    HMODULE jsc = GetModuleHandleW(L"JavaScriptCore.dll");
    if (!webkit || !jsc)
        return false;
    bool ok = true;
#define PRISM_CONTENT_LOAD(module, name, ret, params) \
    name = reinterpret_cast<ret (*) params>(reinterpret_cast<void*>(GetProcAddress(module, #name))); \
    if (!name) ok = false;
#define PRISM_CONTENT_LOAD_WEBKIT(name, ret, params) PRISM_CONTENT_LOAD(webkit, name, ret, params)
#define PRISM_CONTENT_LOAD_JSC(name, ret, params) PRISM_CONTENT_LOAD(jsc, name, ret, params)
    PRISM_BUNDLE_CONTENT(PRISM_CONTENT_LOAD_WEBKIT)
    PRISM_JSC(PRISM_CONTENT_LOAD_JSC)
#undef PRISM_CONTENT_LOAD_JSC
#undef PRISM_CONTENT_LOAD_WEBKIT
#undef PRISM_CONTENT_LOAD
    g_available = ok;
    g_bundle = bundle;
    if (!ok)
        return false;
    // The scripts as they are now; later changes arrive as messages.
    WKStringRef name = WKStringCreateWithUTF8CString("prism:contentScripts?");
    WKStringRef body = WKStringCreateWithUTF8CString("");
    WKTypeRef reply = nullptr;
    WKBundlePostSynchronousMessage(bundle, name, body, &reply);
    WKRelease(body);
    WKRelease(name);
    if (reply) {
        if (WKGetTypeID(reply) == WKStringGetTypeID()) {
            WKStringRef text = static_cast<WKStringRef>(reply);
            size_t size = WKStringGetMaximumUTF8CStringSize(text);
            std::string payload(size, '\0');
            size_t length = WKStringGetUTF8CString(text, payload.data(), size);
            payload.resize(length ? length - 1 : 0);
            parseScripts(payload);
        }
        WKRelease(reply);
    }
    return true;
}

void pageCreated(WKBundlePageRef page)
{
    if (g_available)
        WKBundlePageSetPageLoaderClient(page, &g_loaderClient.base);
}

void pageDestroyed(WKBundlePageRef page)
{
    if (g_available)
        forgetWhere([](const Document& document, const void* data) { return document.page == static_cast<WKBundlePageRef>(data); }, page);
}

void scriptsChanged(const std::string& payload)
{
    if (g_available)
        parseScripts(payload); // for documents loaded from now on
}

void execute(WKBundlePageRef page, const std::string& body)
{
    if (!g_available)
        return;
    // "<call id>\t<extension>\t<script>": runs in the main frame, in the
    // extension's world; the script's value (text) goes back.
    size_t first = body.find('\t');
    size_t second = first == std::string::npos ? std::string::npos : body.find('\t', first + 1);
    if (second == std::string::npos)
        return;
    std::string callId = body.substr(0, first);
    std::string extensionId = body.substr(first + 1, second - first - 1);
    std::string answer = "{\"ok\":false,\"error\":\"The page isn't available.\"}";
    WKBundleFrameRef frame = WKBundlePageGetMainFrame(page);
    if (JSGlobalContextRef context = frame ? documentFor(page, frame, extensionId) : nullptr) {
        JSStringRef source = JSStringCreateWithUTF8CString(body.c_str() + second + 1);
        JSStringRef url = JSStringCreateWithUTF8CString(("prism-extension://" + extensionId + "/executeScript.js").c_str());
        JSValueRef exception = nullptr;
        JSValueRef value = JSEvaluateScript(context, source, nullptr, url, 1, &exception);
        JSStringRelease(url);
        JSStringRelease(source);
        if (value) {
            if (JSStringRef text = JSValueToStringCopy(context, value, nullptr)) {
                answer = toUTF8(text);
                JSStringRelease(text);
            }
        }
    }
    post(page, "prism:execResult", callId + "\t" + answer);
}

void reply(const std::string& body)
{
    if (!g_available)
        return;
    // "<document>\t<call id>\t<answer>"
    size_t first = body.find('\t');
    size_t second = first == std::string::npos ? std::string::npos : body.find('\t', first + 1);
    if (second == std::string::npos)
        return;
    auto found = g_documents.find(_strtoui64(body.substr(0, first).c_str(), nullptr, 10));
    if (found == g_documents.end())
        return;
    JSContextRef context = found->second.context;
    JSObjectRef global = JSContextGetGlobalObject(context);
    JSStringRef name = JSStringCreateWithUTF8CString("__prismReceive");
    JSValueRef value = JSObjectGetProperty(context, global, name, nullptr);
    JSStringRelease(name);
    if (!value || !JSValueIsObject(context, value))
        return;
    JSObjectRef function = JSValueToObject(context, value, nullptr);
    if (!function || !JSObjectIsFunction(context, function))
        return;
    JSStringRef answer = JSStringCreateWithUTF8CString(body.c_str() + second + 1);
    JSValueRef arguments[2] = { JSValueMakeNumber(context, atof(body.substr(first + 1, second - first - 1).c_str())), JSValueMakeString(context, answer) };
    JSValueRef exception = nullptr;
    JSObjectCallAsFunction(context, function, nullptr, 2, arguments, &exception);
    JSStringRelease(answer);
}

} // namespace content
