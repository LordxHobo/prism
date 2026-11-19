// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Permissions.h"

#include "Json.h"
#include "Settings.h"
#include "Util.h"

#include <atomic>
#include <map>
#include <memory>
#include <shellapi.h>
#include <thread>

#include <locationapi.h>

namespace permissions {

using namespace wk;

namespace {

// ---- Saved decisions ------------------------------------------------------------
// permissions.json: {"https://example.com": {"location": true, "notifications": false}}

std::map<std::wstring, std::map<std::wstring, bool>> g_decisions;
bool g_loaded = false;
WKNotificationManagerRef g_notificationManager = nullptr;
std::function<void(WKPageRef)> g_showTab;

std::wstring permissionsFile()
{
    return util::dataDirectory() + L"\\permissions.json";
}

void load()
{
    if (g_loaded)
        return;
    g_loaded = true;
    json::Value root;
    if (!json::parse(util::readFile(permissionsFile()), root))
        return;
    for (auto& [origin, kinds] : root.members()) {
        for (auto& [kind, value] : kinds.members()) {
            if (value.isBool() && (kind == "location" || kind == "notifications"))
                g_decisions[util::fromUTF8(origin)][util::fromUTF8(kind)] = value.asBool();
        }
    }
}

void save()
{
    std::string json = "{";
    bool firstOrigin = true;
    for (auto& [origin, kinds] : g_decisions) {
        if (kinds.empty())
            continue;
        json += (firstOrigin ? "" : ",") + util::jsonString(origin) + ":{";
        firstOrigin = false;
        bool first = true;
        for (auto& [kind, allowed] : kinds) {
            json += (first ? "" : ",") + util::jsonString(kind) + ":" + (allowed ? "true" : "false");
            first = false;
        }
        json += "}";
    }
    json += "}";
    util::writeFileAtomically(permissionsFile(), json);
}

void tellWebKitAboutNotifications(const std::wstring& origin, bool allowed)
{
    if (!g_notificationManager || !WKNotificationManagerProviderDidUpdateNotificationPolicy || !WKSecurityOriginCreateFromString)
        return;
    auto securityOrigin = Ref<WKSecurityOriginRef>::adopt(WKSecurityOriginCreateFromString(string(origin).get()));
    if (securityOrigin)
        WKNotificationManagerProviderDidUpdateNotificationPolicy(g_notificationManager, securityOrigin.get(), allowed);
}

// ---- Location ---------------------------------------------------------------------
// Windows' Location API (the same location service the Maps app uses). Reads
// a position when a page starts watching and then every 15 seconds until it
// stops. Windows only gives it out with Settings > Privacy & security >
// Location > "Let desktop apps access your location" on.

// Declared here so Prism doesn't need locationapi.lib for them.
const CLSID kCLSID_Location = { 0xe5b8e079, 0xee6d, 0x4e33, { 0xa4, 0x38, 0xc8, 0x7f, 0x2e, 0x95, 0x92, 0x54 } };
const IID kIID_ILocation = { 0xab2ece69, 0x56d9, 0x4f28, { 0xb5, 0x25, 0xde, 0x1b, 0x0e, 0xe4, 0x42, 0x37 } };
const IID kIID_ILatLongReport = { 0x7fed806d, 0x0ef8, 0x4f07, { 0x80, 0xac, 0x36, 0xa0, 0xbe, 0xae, 0x31, 0x34 } };

const wchar_t kLocationOff[] = L"Windows didn't give Prism a location. Check that Location is on in Windows Settings > Privacy & security > Location, including \"Let desktop apps access your location\".";

struct LocationResult {
    bool ok { false };
    double latitude { 0 };
    double longitude { 0 };
    double accuracy { 0 };
};

LocationResult readLocation(ILocation* location)
{
    LocationResult result;
    IID types[] = { kIID_ILatLongReport };
    location->RequestPermissions(nullptr, types, 1, FALSE);
    // The first report can take a few seconds.
    for (int attempt = 0; attempt < 30; ++attempt) {
        LOCATION_REPORT_STATUS status = REPORT_NOT_SUPPORTED;
        if (FAILED(location->GetReportStatus(kIID_ILatLongReport, &status)) || status == REPORT_ACCESS_DENIED || status == REPORT_NOT_SUPPORTED)
            return result;
        if (status == REPORT_RUNNING)
            break;
        Sleep(500);
    }
    ILocationReport* report = nullptr;
    if (FAILED(location->GetReport(kIID_ILatLongReport, &report)) || !report)
        return result;
    ILatLongReport* latLong = nullptr;
    if (SUCCEEDED(report->QueryInterface(kIID_ILatLongReport, reinterpret_cast<void**>(&latLong))) && latLong) {
        result.ok = SUCCEEDED(latLong->GetLatitude(&result.latitude)) && SUCCEEDED(latLong->GetLongitude(&result.longitude));
        if (FAILED(latLong->GetErrorRadius(&result.accuracy)) || result.accuracy <= 0)
            result.accuracy = 1000;
        latLong->Release();
    }
    report->Release();
    return result;
}

struct LocationWatch {
    std::atomic<bool> running { true };
    HANDLE wake { CreateEventW(nullptr, FALSE, FALSE, nullptr) };
    ~LocationWatch() { CloseHandle(wake); }
};

std::shared_ptr<LocationWatch> g_watch;

double nowSeconds()
{
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return static_cast<double>(value.QuadPart) / 1e7 - 11644473600.0;
}

void startUpdating(WKGeolocationManagerRef manager, const void*)
{
    if (g_watch)
        return;
    auto watch = std::make_shared<LocationWatch>();
    g_watch = watch;
    std::thread([watch, manager] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ILocation* location = nullptr;
        CoCreateInstance(kCLSID_Location, nullptr, CLSCTX_INPROC_SERVER, kIID_ILocation, reinterpret_cast<void**>(&location));
        while (watch->running) {
            LocationResult result = location ? readLocation(location) : LocationResult { };
            util::postToMainThread([watch, manager, result] {
                if (!watch->running || g_watch != watch)
                    return;
                if (result.ok) {
                    auto position = Ref<WKGeolocationPositionRef>::adopt(WKGeolocationPositionCreate(nowSeconds(), result.latitude, result.longitude, result.accuracy));
                    WKGeolocationManagerProviderDidChangePosition(manager, position.get());
                } else
                    WKGeolocationManagerProviderDidFailToDeterminePositionWithErrorMessage(manager, string(kLocationOff).get());
            });
            if (!result.ok)
                break;
            WaitForSingleObject(watch->wake, 15000);
        }
        if (location)
            location->Release();
        CoUninitialize();
    }).detach();
}

void stopUpdating(WKGeolocationManagerRef, const void*)
{
    if (!g_watch)
        return;
    g_watch->running = false;
    SetEvent(g_watch->wake);
    g_watch.reset();
}

// ---- Notifications -----------------------------------------------------------------
// Shown as Windows notifications from a Prism icon in the notification area,
// which goes away again when the notification does.

constexpr UINT WM_NOTIFY_ICON = WM_APP + 20;
constexpr UINT kIconId = 1;
HWND g_notifyWindow = nullptr;
bool g_iconAdded = false;
uint64_t g_shownId = 0;
WKPageRef g_shownPage = nullptr;

void removeIcon()
{
    if (!g_iconAdded)
        return;
    NOTIFYICONDATAW data = { sizeof(data) };
    data.hWnd = g_notifyWindow;
    data.uID = kIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    g_iconAdded = false;
}

LRESULT CALLBACK notifyWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NOTIFY_ICON) {
        UINT event = LOWORD(lParam);
        if (event == NIN_BALLOONUSERCLICK || event == WM_LBUTTONUP) {
            if (g_notificationManager && g_shownId && WKNotificationManagerProviderDidClickNotification)
                WKNotificationManagerProviderDidClickNotification(g_notificationManager, g_shownId);
            if (g_showTab && g_shownPage)
                g_showTab(g_shownPage);
            removeIcon();
        } else if (event == NIN_BALLOONTIMEOUT || event == NIN_BALLOONHIDE)
            removeIcon();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void ensureNotifyWindow()
{
    if (g_notifyWindow)
        return;
    WNDCLASSW windowClass = { };
    windowClass.lpfnWndProc = notifyWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"PrismNotifications";
    RegisterClassW(&windowClass);
    g_notifyWindow = CreateWindowExW(WS_EX_TOOLWINDOW, L"PrismNotifications", L"Prism notifications", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

void copyInto(wchar_t* target, size_t capacity, const std::wstring& text)
{
    std::wstring clipped = text.size() >= capacity ? text.substr(0, capacity - 2) + L"…" : text;
    wcsncpy_s(target, capacity, clipped.c_str(), _TRUNCATE);
}

void showNotification(WKPageRef page, WKNotificationRef notification, const void*)
{
    std::wstring origin = originString(WKNotificationGetSecurityOrigin(notification));
    // A site that isn't allowed doesn't get to show anything (WebKit checks
    // too; this covers decisions changed since the page loaded).
    if (decision(origin, L"notifications") != L"allow")
        return;
    std::wstring title = toWide(Ref<WKStringRef>::adopt(WKNotificationCopyTitle(notification)).get());
    std::wstring body = toWide(Ref<WKStringRef>::adopt(WKNotificationCopyBody(notification)).get());
    ensureNotifyWindow();

    NOTIFYICONDATAW data = { sizeof(data) };
    data.hWnd = g_notifyWindow;
    data.uID = kIconId;
    data.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_INFO | NIF_SHOWTIP;
    data.uCallbackMessage = WM_NOTIFY_ICON;
    data.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    copyInto(data.szTip, std::size(data.szTip), L"Prism");
    copyInto(data.szInfoTitle, std::size(data.szInfoTitle), title.empty() ? displayName(origin) : title);
    copyInto(data.szInfo, std::size(data.szInfo), (body.empty() ? L"" : body + L"\n") + displayName(origin));
    data.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    data.hBalloonIcon = data.hIcon;
    if (!g_iconAdded) {
        g_iconAdded = Shell_NotifyIconW(NIM_ADD, &data);
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    } else
        Shell_NotifyIconW(NIM_MODIFY, &data);

    g_shownId = WKNotificationGetID(notification);
    g_shownPage = page;
    if (g_notificationManager && WKNotificationManagerProviderDidShowNotification)
        WKNotificationManagerProviderDidShowNotification(g_notificationManager, g_shownId);
}

void cancelNotification(WKNotificationRef notification, const void*)
{
    if (WKNotificationGetID(notification) == g_shownId)
        removeIcon();
}

// What WebKit tells new pages about Notification.permission.
WKDictionaryRef notificationPermissions(const void*)
{
    load();
    WKMutableDictionaryRef dictionary = WKMutableDictionaryCreate();
    for (auto& [origin, kinds] : g_decisions) {
        auto found = kinds.find(L"notifications");
        if (found == kinds.end())
            continue;
        auto value = Ref<WKBooleanRef>::adopt(WKBooleanCreate(found->second));
        WKDictionarySetItem(dictionary, string(origin).get(), value.get());
    }
    return dictionary;
}

} // namespace

std::wstring originString(WKSecurityOriginRef origin)
{
    if (!origin || !WKSecurityOriginCopyToString)
        return { };
    return toWide(Ref<WKStringRef>::adopt(WKSecurityOriginCopyToString(origin)).get());
}

std::wstring displayName(const std::wstring& origin)
{
    size_t start = origin.find(L"://");
    std::wstring host = start == std::wstring::npos ? origin : origin.substr(start + 3);
    // Leave a port on (it's a different site), drop a trailing slash.
    while (!host.empty() && host.back() == L'/')
        host.pop_back();
    if (origin.rfind(L"file:", 0) == 0)
        return L"This file";
    return host.empty() ? origin : host;
}

std::wstring decision(const std::wstring& origin, const std::wstring& kind)
{
    load();
    auto site = g_decisions.find(origin);
    if (site != g_decisions.end()) {
        auto found = site->second.find(kind);
        if (found != site->second.end())
            return found->second ? L"allow" : L"block";
    }
    return settings::permissionDefault(kind) == L"block" ? L"block" : L"";
}

void set(const std::wstring& origin, const std::wstring& kind, bool allow)
{
    load();
    if (origin.empty() || origin == L"null")
        return;
    g_decisions[origin][kind] = allow;
    save();
    if (kind == L"notifications")
        tellWebKitAboutNotifications(origin, allow);
}

void initialize(WKContextRef context, std::function<void(WKPageRef)> showTab)
{
    g_showTab = std::move(showTab);
    if (WKContextGetGeolocationManager && WKGeolocationManagerSetProvider && WKGeolocationPositionCreate
        && WKGeolocationManagerProviderDidChangePosition && WKGeolocationManagerProviderDidFailToDeterminePositionWithErrorMessage) {
        static WKGeolocationProviderV0 provider = { };
        provider.base.version = 0;
        provider.startUpdating = startUpdating;
        provider.stopUpdating = stopUpdating;
        WKGeolocationManagerSetProvider(WKContextGetGeolocationManager(context), &provider.base);
    }
    if (WKContextGetNotificationManager && WKNotificationManagerSetProvider && WKNotificationCopyTitle && WKNotificationCopyBody
        && WKNotificationGetSecurityOrigin && WKNotificationGetID && WKMutableDictionaryCreate && WKDictionarySetItem && WKBooleanCreate) {
        static WKNotificationProviderV0 provider = { };
        provider.base.version = 0;
        provider.show = showNotification;
        provider.cancel = cancelNotification;
        provider.notificationPermissions = notificationPermissions;
        g_notificationManager = WKContextGetNotificationManager(context);
        WKNotificationManagerSetProvider(g_notificationManager, &provider.base);
    }
}

void shutdown()
{
    stopUpdating(nullptr, nullptr);
    removeIcon();
}

std::string handleCommand(const std::wstring& command, const std::wstring& argument)
{
    load();
    if (command == L"list") {
        // [{"origin": "...", "name": "...", "location": true|false|null, "notifications": ...}]
        std::string json = "[";
        bool first = true;
        for (auto& [origin, kinds] : g_decisions) {
            if (kinds.empty())
                continue;
            json += (first ? "" : ",") + std::string("{\"origin\":") + util::jsonString(origin) + ",\"name\":" + util::jsonString(displayName(origin));
            first = false;
            for (const wchar_t* kind : { L"location", L"notifications" }) {
                auto found = kinds.find(kind);
                json += "," + util::jsonString(kind) + ":" + (found == kinds.end() ? "null" : found->second ? "true" : "false");
            }
            json += "}";
        }
        return json + "]";
    }
    if (command == L"remove") {
        // "<origin>\n<kind>": the site gets asked again next time.
        size_t newline = argument.find(L'\n');
        std::wstring origin = argument.substr(0, newline);
        std::wstring kind = newline == std::wstring::npos ? L"" : argument.substr(newline + 1);
        auto site = g_decisions.find(origin);
        if (site != g_decisions.end()) {
            site->second.erase(kind);
            if (site->second.empty())
                g_decisions.erase(site);
            save();
        }
        return "true";
    }
    return "null";
}

} // namespace permissions
