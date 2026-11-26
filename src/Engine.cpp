// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Engine.h"

#include "Blocker.h"
#include "Extensions.h"
#include "Passwords.h"
#include "Aci.h"
#include "Tabs.h"
#include "Translate.h"
#include "Icons.h"
#include "RequestFilter.h"
#include "Shields.h"
#include "Settings.h"
#include "Util.h"

#include <cctype>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <map>

using namespace wk;

namespace engine {

static Ref<WKContextRef> s_context;
static Ref<WKPreferencesRef> s_preferences;
static Ref<WKWebsiteDataStoreRef> s_dataStore;
static Ref<WKPageConfigurationRef> s_pageConfiguration;
static Ref<WKPreferencesRef> s_backgroundPreferences;
static Ref<WKPageConfigurationRef> s_backgroundConfiguration;
static Ref<WKContextRef> s_privateContext;
static Ref<WKWebsiteDataStoreRef> s_privateStore;
static Ref<WKPageConfigurationRef> s_privateConfiguration;
static Ref<WKUserContentControllerRef> s_privateUserContent;
static bool s_privateProxyEnabled = false;

static Ref<WKPreferencesRef> createPreferences()
{
    auto preferences = Ref<WKPreferencesRef>::adopt(WKPreferencesCreate());
    // No DNS prefetching: don't look up links you never clicked.
    if (WKPreferencesSetDNSPrefetchingEnabled)
        WKPreferencesSetDNSPrefetchingEnabled(preferences.get(), false);
    // No <a ping>: links can't silently report your clicks to third parties.
    if (WKPreferencesSetHyperlinkAuditingEnabled)
        WKPreferencesSetHyperlinkAuditingEnabled(preferences.get(), false);
    // Popups only open when you click something.
    if (WKPreferencesSetJavaScriptCanOpenWindowsAutomatically)
        WKPreferencesSetJavaScriptCanOpenWindowsAutomatically(preferences.get(), false);
    // Developer tools (F12).
    if (WKPreferencesSetDeveloperExtrasEnabled)
        WKPreferencesSetDeveloperExtrasEnabled(preferences.get(), true);
    // Element full screen (a video's full screen button); needs a WebKit
    // that tells Prism (WKViewSetFullScreenClient).
    if (WKPreferencesSetFullScreenEnabled && WKViewSetFullScreenClient)
        WKPreferencesSetFullScreenEnabled(preferences.get(), true);
    return preferences;
}
static Ref<WKUserContentControllerRef> s_userContent;
static bool s_proxyEnabled = false;
static std::wstring s_uiDirectory;
static std::wstring s_uiDirectoryURL;
static bool s_darkTheme = true;

static void setDirectory(void (*setter)(WKWebsiteDataStoreConfigurationRef, WKStringRef), WKWebsiteDataStoreConfigurationRef configuration, const std::wstring& path)
{
    if (!setter)
        return;
    util::ensureDirectory(path);
    setter(configuration, string(path).get());
}

void deleteProfileIfRequested()
{
    // "Clear browsing data when Prism closes": WebKit's helper processes may
    // still have the files open while Prism exits, so the profile is wiped
    // here, at the next start, before WebKit is running. Either way nothing
    // from the last session is ever loaded again.
    if (!settings::clearOnExit())
        return;
    std::error_code ignored;
    std::filesystem::remove_all(std::filesystem::path(util::dataDirectory() + L"\\Profile"), ignored);
}

void applyCookiePolicy()
{
    // Block third-party cookies (cookies from sites other than the one in the
    // address bar), the main tool ad networks use to follow you around.
    // Standard still lets a site you've visited directly keep its cookies when
    // it's embedded elsewhere (needed for some logins); strict never does.
    if (!s_dataStore || !WKWebsiteDataStoreGetHTTPCookieStore || !WKHTTPCookieStoreSetHTTPCookieAcceptPolicy)
        return;
    auto policy = settings::strictCookies() ? kWKHTTPCookieAcceptPolicyExclusivelyFromMainDocumentDomain : kWKHTTPCookieAcceptPolicyOnlyFromMainDocumentDomain;
    for (WKWebsiteDataStoreRef store : { s_dataStore.get(), s_privateStore.get() }) {
        if (!store)
            continue;
        if (auto cookieStore = WKWebsiteDataStoreGetHTTPCookieStore(store))
            WKHTTPCookieStoreSetHTTPCookieAcceptPolicy(cookieStore, policy, nullptr, [](void*) { });
    }
}

namespace {

// Counts down the asynchronous removals and reports once all are done.
struct ClearOperation {
    int pending { 1 };
    std::function<void()> done;
};

void clearStepFinished(void* context)
{
    auto* operation = static_cast<ClearOperation*>(context);
    if (--operation->pending == 0) {
        if (operation->done)
            operation->done();
        delete operation;
    }
}

} // namespace

void clearBrowsingData(bool cookiesAndSiteData, bool cache, std::function<void()> done)
{
    auto* operation = new ClearOperation;
    operation->done = std::move(done);
    auto run = [operation](auto function, auto target) {
        if (!function || !target)
            return;
        ++operation->pending;
        function(target, operation, clearStepFinished);
    };

    WKWebsiteDataStoreRef store = s_dataStore.get();
    if (cookiesAndSiteData) {
        WKHTTPCookieStoreRef cookies = WKWebsiteDataStoreGetHTTPCookieStore ? WKWebsiteDataStoreGetHTTPCookieStore(store) : nullptr;
        run(WKHTTPCookieStoreDeleteAllCookies, cookies);
        run(WKWebsiteDataStoreClearStorage, store);
        run(WKWebsiteDataStoreStatisticsClearThroughWebsiteDataRemoval, store);
    }
    if (cache) {
        run(WKWebsiteDataStoreRemoveNetworkCache, store);
        run(WKWebsiteDataStoreRemoveMemoryCaches, store);
        run(WKWebsiteDataStoreRemoveAllFetchCaches, store);
    }
    // Drop the initial count; reports immediately if nothing was started.
    clearStepFinished(operation);
}

static Ref<WKUserContentControllerRef> createUserContent(bool forExtensions);

bool initialize(std::wstring& error)
{
    if (!wk::load(error))
        return false;

    s_uiDirectory = util::exeDirectory() + L"\\ui";
    s_uiDirectoryURL = util::fileURLFromPath(s_uiDirectory);
    if (s_uiDirectoryURL.empty() || s_uiDirectoryURL.back() != L'/')
        s_uiDirectoryURL += L'/';

    if (GetFileAttributesW((s_uiDirectory + L"\\toolbar.html").c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"Prism's interface files are missing.\n\nExpected them in:\n" + s_uiDirectory;
        return false;
    }

    // --- Profile -----------------------------------------------------------
    // Everything a website can store (cookies, caches, local storage, ...)
    // goes into %LOCALAPPDATA%\Prism\Profile. Nothing is stored anywhere else
    // and nothing is ever sent to us: Prism has no telemetry of any kind.
    std::wstring profile = util::dataDirectory() + L"\\Profile";
    util::ensureDirectory(profile);

    auto dataConfig = Ref<WKWebsiteDataStoreConfigurationRef>::adopt(WKWebsiteDataStoreConfigurationCreate());
    setDirectory(WKWebsiteDataStoreConfigurationSetNetworkCacheDirectory, dataConfig.get(), profile + L"\\Cache");
    setDirectory(WKWebsiteDataStoreConfigurationSetIndexedDBDatabaseDirectory, dataConfig.get(), profile + L"\\IndexedDB");
    setDirectory(WKWebsiteDataStoreConfigurationSetLocalStorageDirectory, dataConfig.get(), profile + L"\\LocalStorage");
    setDirectory(WKWebsiteDataStoreConfigurationSetCacheStorageDirectory, dataConfig.get(), profile + L"\\CacheStorage");
    setDirectory(WKWebsiteDataStoreConfigurationSetGeneralStorageDirectory, dataConfig.get(), profile + L"\\Storage");
    setDirectory(WKWebsiteDataStoreConfigurationSetMediaKeysStorageDirectory, dataConfig.get(), profile + L"\\MediaKeys");
    setDirectory(WKWebsiteDataStoreConfigurationSetResourceLoadStatisticsDirectory, dataConfig.get(), profile + L"\\TrackingPrevention");
    setDirectory(WKWebsiteDataStoreConfigurationSetServiceWorkerRegistrationDirectory, dataConfig.get(), profile + L"\\ServiceWorkers");
    if (WKWebsiteDataStoreConfigurationSetCookieStorageFile)
        WKWebsiteDataStoreConfigurationSetCookieStorageFile(dataConfig.get(), string(profile + L"\\cookies.db").get());

    s_dataStore = Ref<WKWebsiteDataStoreRef>::adopt(WKWebsiteDataStoreCreateWithConfiguration(dataConfig.get()));

    // --- Privacy defaults --------------------------------------------------
    // Intelligent Tracking Prevention: WebKit's built-in tracker defence.
    // It learns which sites track you across the web and cuts off their
    // cookies and storage.
    if (WKWebsiteDataStoreSetResourceLoadStatisticsEnabled)
        WKWebsiteDataStoreSetResourceLoadStatisticsEnabled(s_dataStore.get(), true);

    applyCookiePolicy();

    s_preferences = createPreferences();
    // Extension pages (background, popups, options tabs) come from each
    // extension's own address (ExtensionServer.h). Background pages run in
    // hidden windows: WebKit would otherwise slow their timers down.
    s_backgroundPreferences = createPreferences();
    if (WKPreferencesSetHiddenPageDOMTimerThrottlingEnabled)
        WKPreferencesSetHiddenPageDOMTimerThrottlingEnabled(s_backgroundPreferences.get(), false);
    if (WKPreferencesSetPageVisibilityBasedProcessSuppressionEnabled)
        WKPreferencesSetPageVisibilityBasedProcessSuppressionEnabled(s_backgroundPreferences.get(), false);

    // PrismBundle.dll runs in WebKit's content processes and lets Prism
    // filter every request pages make (RequestFilter.h).
    std::wstring bundle = requestfilter::bundlePath();
    if (!bundle.empty() && WKContextConfigurationCreate && WKContextConfigurationSetInjectedBundlePath) {
        Ref<WKContextConfigurationRef> configuration = Ref<WKContextConfigurationRef>::adopt(WKContextConfigurationCreate());
        WKContextConfigurationSetInjectedBundlePath(configuration.get(), string(bundle).get());
        s_context = Ref<WKContextRef>::adopt(WKContextCreateWithConfiguration(configuration.get()));
        requestfilter::initialize(s_context.get());
    } else {
        s_context = Ref<WKContextRef>::adopt(WKContextCreateWithConfiguration(nullptr));
    }

    s_pageConfiguration = Ref<WKPageConfigurationRef>::adopt(WKPageConfigurationCreate());
    WKPageConfigurationSetContext(s_pageConfiguration.get(), s_context.get());
    WKPageConfigurationSetPreferences(s_pageConfiguration.get(), s_preferences.get());
    WKPageConfigurationSetWebsiteDataStore(s_pageConfiguration.get(), s_dataStore.get());

    // Extension content scripts and their messages to Prism.
    s_userContent = createUserContent(true);
    WKPageConfigurationSetUserContentController(s_pageConfiguration.get(), s_userContent.get());

    s_backgroundConfiguration = Ref<WKPageConfigurationRef>::adopt(WKPageConfigurationCreate());
    WKPageConfigurationSetContext(s_backgroundConfiguration.get(), s_context.get());
    WKPageConfigurationSetPreferences(s_backgroundConfiguration.get(), s_backgroundPreferences.get());
    WKPageConfigurationSetWebsiteDataStore(s_backgroundConfiguration.get(), s_dataStore.get());
    WKPageConfigurationSetUserContentController(s_backgroundConfiguration.get(), s_userContent.get());
    return true;
}

// Messages from extension scripts in private windows only count with
// "Extensions in private windows" on.
static bool extensionMessageAllowed(WKScriptMessageRef message)
{
    if (settings::extensionsInPrivate())
        return true;
    WKFrameInfoRef frame = WKScriptMessageGetFrameInfo(message);
    tabs::Info tab;
    return !(frame && tabs::findByPageAnyWindow(WKFrameInfoGetPage(frame), tab) && tab.isPrivate);
}

// Script message handlers for pages: extensions' and Prism's own (Shields,
// reading view, passwords).
static Ref<WKUserContentControllerRef> createUserContent(bool forExtensions)
{
    auto controller = Ref<WKUserContentControllerRef>::adopt(WKUserContentControllerCreate());
    WKUserContentControllerRef raw = controller.get();
    if (forExtensions) {
        WKUserContentControllerAddScriptMessageHandler(raw, stringUTF8("prismExtension").get(),
            [](WKScriptMessageRef message, WKCompletionListenerRef reply, const void*) {
                if (!extensionMessageAllowed(message)) {
                    WKCompletionListenerComplete(reply, stringUTF8("null").get());
                    return;
                }
                extensions::handleScriptMessage(message, reply);
            }, nullptr);
        WKUserContentControllerAddScriptMessageHandler(raw, stringUTF8("prismExtensionPage").get(),
            [](WKScriptMessageRef message, WKCompletionListenerRef reply, const void*) {
                if (!extensionMessageAllowed(message)) {
                    WKCompletionListenerComplete(reply, stringUTF8("null").get());
                    return;
                }
                extensions::handlePageMessage(message, reply);
            }, nullptr);
    }
    WKUserContentControllerAddScriptMessageHandler(raw, stringUTF8("prismShields").get(),
        [](WKScriptMessageRef message, WKCompletionListenerRef reply, const void*) { shields::handleScriptMessage(message, reply); }, nullptr);
    // ACI reading views running in the page's world (no PrismBundle): only
    // with the per-start key the view was given.
    WKUserContentControllerAddScriptMessageHandler(raw, stringUTF8("prismAci").get(),
        [](WKScriptMessageRef message, WKCompletionListenerRef reply, const void*) {
            WKTypeRef body = WKScriptMessageGetBody(message);
            WKFrameInfoRef frame = WKScriptMessageGetFrameInfo(message);
            std::string answer = "null";
            if (body && WKGetTypeID(body) == WKStringGetTypeID() && frame && WKFrameInfoGetIsMainFrame(frame)) {
                std::string text = util::toUTF8(toWide(static_cast<WKStringRef>(body)));
                answer = aci::handleFallbackMessage(WKFrameInfoGetPage(frame), text);
            }
            WKCompletionListenerComplete(reply, stringUTF8(answer).get());
        }, nullptr);
    // Site icons the page fetched itself (when Prism couldn't): only for the
    // tab's own site, from its main frame.
    WKUserContentControllerAddScriptMessageHandler(raw, stringUTF8("prismIcon").get(),
        [](WKScriptMessageRef message, WKCompletionListenerRef reply, const void*) {
            WKCompletionListenerComplete(reply, stringUTF8("null").get());
            WKTypeRef body = WKScriptMessageGetBody(message);
            WKFrameInfoRef frame = WKScriptMessageGetFrameInfo(message);
            if (!body || WKGetTypeID(body) != WKStringGetTypeID() || !frame || !WKFrameInfoGetIsMainFrame(frame))
                return;
            tabs::Info tab;
            if (!tabs::findByPage(WKFrameInfoGetPage(frame), tab) || isInternalURL(tab.url))
                return;
            icons::storeFromPage(tab.url, util::toUTF8(toWide(static_cast<WKStringRef>(body))));
        }, nullptr);
    WKUserContentControllerAddScriptMessageHandler(raw, stringUTF8("prismPasswords").get(),
        [](WKScriptMessageRef message, WKCompletionListenerRef reply, const void*) { passwords::handleScriptMessage(message, reply); }, nullptr);
    return controller;
}

WKPageConfigurationRef extensionPageConfiguration()
{
    return s_backgroundConfiguration.get();
}

WKUserContentControllerRef userContentController()
{
    return s_userContent.get();
}

static void applyProxy(WKWebsiteDataStoreRef store, bool& enabled, bool wanted)
{
    if (!store)
        return;
    if (wanted && !enabled) {
        uint16_t port = blocker::startProxy();
        if (!port)
            return;
        // Local addresses (your router, local dev servers) skip the proxy.
        WKWebsiteDataStoreEnableCustomNetworkProxySettings(store,
            url(L"http://127.0.0.1:" + std::to_wstring(port)).get(), stringUTF8("localhost,.localhost,127.0.0.1,::1").get());
        enabled = true;
    } else if (!wanted && enabled) {
        WKWebsiteDataStoreDisableNetworkProxySettings(store);
        enabled = false;
    }
}

void updateBlockingProxy()
{
    if (!s_dataStore || !WKWebsiteDataStoreEnableCustomNetworkProxySettings || !WKWebsiteDataStoreDisableNetworkProxySettings)
        return;
    bool wanted = blocker::hasAnyLists();
    applyProxy(s_dataStore.get(), s_proxyEnabled, wanted);
    applyProxy(s_privateStore.get(), s_privateProxyEnabled, wanted);
}

bool blockingProxyActive()
{
    return s_proxyEnabled;
}

// ---- User scripts ----------------------------------------------------------

static std::map<std::string, std::vector<std::string>> s_userScripts;

static void registerUserScript(WKUserContentControllerRef controller, const std::string& source)
{
    auto script = Ref<WKUserScriptRef>::adopt(WKUserScriptCreateWithSource(stringUTF8(source).get(), kWKInjectAtDocumentStart, false));
    WKUserContentControllerAddUserScript(controller, script.get());
}

// Private pages get every group except extensions'.
static void registerAllUserScripts(WKUserContentControllerRef controller, bool withExtensions)
{
    WKUserContentControllerRemoveAllUserScripts(controller);
    // Shields first, so its scriptlets run before extensions' scripts.
    for (const char* name : { "shields", "extensions" }) {
        if (!withExtensions && std::string(name) == "extensions")
            continue;
        for (auto& source : s_userScripts[name])
            registerUserScript(controller, source);
    }
    for (auto& [name, sources] : s_userScripts) {
        if (name == "shields" || name == "extensions")
            continue;
        for (auto& source : sources)
            registerUserScript(controller, source);
    }
}

void setUserScripts(const std::string& group, std::vector<std::string> sources)
{
    if (!s_userContent)
        return;
    s_userScripts[group] = std::move(sources);
    registerAllUserScripts(s_userContent.get(), true);
    if (s_privateUserContent)
        registerAllUserScripts(s_privateUserContent.get(), settings::extensionsInPrivate());
}

void addUserScript(const std::string& group, const std::string& source)
{
    if (!s_userContent)
        return;
    s_userScripts[group].push_back(source);
    registerUserScript(s_userContent.get(), source);
    if (s_privateUserContent && (group != "extensions" || settings::extensionsInPrivate()))
        registerUserScript(s_privateUserContent.get(), source);
}

void restartNetworkProcess()
{
    if (s_dataStore && WKWebsiteDataStoreTerminateNetworkProcess)
        WKWebsiteDataStoreTerminateNetworkProcess(s_dataStore.get());
}

WKContextRef context()
{
    return s_context.get();
}

WKPageConfigurationRef pageConfiguration()
{
    return s_pageConfiguration.get();
}

WKPageConfigurationRef privatePageConfiguration()
{
    if (s_privateConfiguration)
        return s_privateConfiguration.get();
    if (!WKWebsiteDataStoreCreateNonPersistentDataStore || !s_context)
        return nullptr;

    // Its own browsing context: separate web processes, which PrismBundle
    // starts without extension content scripts (RequestFilter.h). Shields
    // still filters every request.
    if (!s_privateContext) {
        std::wstring bundle = requestfilter::bundlePath();
        if (!bundle.empty() && WKContextConfigurationCreate && WKContextConfigurationSetInjectedBundlePath) {
            Ref<WKContextConfigurationRef> configuration = Ref<WKContextConfigurationRef>::adopt(WKContextConfigurationCreate());
            WKContextConfigurationSetInjectedBundlePath(configuration.get(), string(bundle).get());
            s_privateContext = Ref<WKContextRef>::adopt(WKContextCreateWithConfiguration(configuration.get()));
            requestfilter::initialize(s_privateContext.get());
        } else {
            s_privateContext = Ref<WKContextRef>::adopt(WKContextCreateWithConfiguration(nullptr));
        }
    }
    if (!s_privateUserContent) {
        s_privateUserContent = createUserContent(true);
        registerAllUserScripts(s_privateUserContent.get(), settings::extensionsInPrivate());
    }

    // Cookies, storage and cache only in memory.
    s_privateStore = Ref<WKWebsiteDataStoreRef>::adopt(WKWebsiteDataStoreCreateNonPersistentDataStore());
    if (!s_privateStore)
        return nullptr;
    if (WKWebsiteDataStoreSetResourceLoadStatisticsEnabled)
        WKWebsiteDataStoreSetResourceLoadStatisticsEnabled(s_privateStore.get(), true);
    s_privateProxyEnabled = false;
    applyCookiePolicy();
    updateBlockingProxy();

    s_privateConfiguration = Ref<WKPageConfigurationRef>::adopt(WKPageConfigurationCreate());
    WKPageConfigurationSetContext(s_privateConfiguration.get(), s_privateContext.get());
    WKPageConfigurationSetPreferences(s_privateConfiguration.get(), s_preferences.get());
    WKPageConfigurationSetWebsiteDataStore(s_privateConfiguration.get(), s_privateStore.get());
    WKPageConfigurationSetUserContentController(s_privateConfiguration.get(), s_privateUserContent.get());
    return s_privateConfiguration.get();
}

WKContextRef privateContextIfCreated()
{
    return s_privateContext.get();
}

void privateExtensionsChanged()
{
    static int s_last = -1;
    int now = settings::extensionsInPrivate() ? 1 : 0;
    if (now == s_last)
        return;
    s_last = now;
    if (s_privateUserContent)
        registerAllUserScripts(s_privateUserContent.get(), now == 1);
    // PrismBundle in private web processes: the scripts for documents loaded
    // from now on (no extensions' when off).
    if (s_privateContext && WKContextPostMessageToInjectedBundle)
        WKContextPostMessageToInjectedBundle(s_privateContext.get(), stringUTF8("prism:contentScripts").get(), stringUTF8(contentScriptsPayload(true)).get());
}

std::string contentScriptsPayload(bool privateContext)
{
    std::string payload = privateContext && !settings::extensionsInPrivate() ? std::string("0\n") : extensions::contentScriptsPayload();
    return translate::withTranslator(payload.empty() ? std::string("0\n") : payload);
}

void sendContentScripts()
{
    if (!WKContextPostMessageToInjectedBundle)
        return;
    if (s_context)
        WKContextPostMessageToInjectedBundle(s_context.get(), stringUTF8("prism:contentScripts").get(), stringUTF8(contentScriptsPayload(false)).get());
    if (s_privateContext)
        WKContextPostMessageToInjectedBundle(s_privateContext.get(), stringUTF8("prism:contentScripts").get(), stringUTF8(contentScriptsPayload(true)).get());
}

bool isPrivateContext(WKContextRef context)
{
    return context && context == s_privateContext.get();
}

void releasePrivateData()
{
    // Pages hold their own references to the store until they're gone; once
    // the last one goes, so does everything it kept in memory. The next
    // private window starts with an empty store.
    if (s_privateStore && s_privateProxyEnabled && WKWebsiteDataStoreDisableNetworkProxySettings)
        WKWebsiteDataStoreDisableNetworkProxySettings(s_privateStore.get());
    s_privateProxyEnabled = false;
    s_privateConfiguration.reset();
    s_privateStore.reset();
}

void configurePage(WKPageRef page, double scaleFactor)
{
    // Identify as Safari. A rare user agent makes you easier to fingerprint,
    // and many sites only test their WebKit support against Safari.
    if (WKPageSetApplicationNameForUserAgent)
        WKPageSetApplicationNameForUserAgent(page, stringUTF8("Version/26.0 Safari/605.1.15").get());
    if (WKPageSetCustomBackingScaleFactor)
        WKPageSetCustomBackingScaleFactor(page, scaleFactor);
}

std::wstring uiDirectoryURL()
{
    return s_uiDirectoryURL;
}

void setDarkTheme(bool dark)
{
    s_darkTheme = dark;
}

bool darkTheme()
{
    return s_darkTheme;
}

std::wstring internalPageURL(const std::wstring& name, const std::wstring& query)
{
    std::wstring url = s_uiDirectoryURL + name + L".html?theme=" + (s_darkTheme ? L"dark" : L"light")
        + L"&accent=" + util::urlEncode(settings::accent()) + L"&palette=" + settings::palette();
    if (!query.empty())
        url += L"&" + query;
    return url;
}

void loadInternalPage(WKPageRef page, const std::wstring& name, const std::wstring& query)
{
    // LoadFile (rather than LoadURL) lets the page read the other files in
    // the ui folder (its CSS and scripts) and nothing outside it.
    WKPageLoadFile(page, url(internalPageURL(name, query)).get(), url(s_uiDirectoryURL).get());
}

// File URLs can be spelled several ways (C: vs c:, %20 vs a space, ...), so
// compare a normalized form: decoded, lowercase, no query or fragment.
static std::wstring normalizeFileURL(const std::wstring& value)
{
    std::wstring text = value.substr(0, value.find_first_of(L"?#"));
    std::string bytes;
    std::string utf8 = util::toUTF8(text);
    for (size_t i = 0; i < utf8.size(); ++i) {
        if (utf8[i] == '%' && i + 2 < utf8.size() && isxdigit(static_cast<unsigned char>(utf8[i + 1])) && isxdigit(static_cast<unsigned char>(utf8[i + 2]))) {
            bytes += static_cast<char>(std::stoi(utf8.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else
            bytes += utf8[i];
    }
    std::wstring result = util::fromUTF8(bytes);
    for (auto& c : result) {
        c = static_cast<wchar_t>(towlower(c));
        if (c == L'\\')
            c = L'/';
    }
    return result;
}

bool isInternalURL(const std::wstring& value)
{
    static std::wstring base = normalizeFileURL(s_uiDirectoryURL);
    return normalizeFileURL(value).rfind(base, 0) == 0;
}

bool isInternalPage(const std::wstring& value, const std::wstring& name)
{
    return normalizeFileURL(value) == normalizeFileURL(s_uiDirectoryURL + name + L".html");
}

std::wstring searchURLTemplate()
{
    return settings::searchURL();
}

} // namespace engine
