// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Http.h"

#include "Util.h"
#include "Version.h"

#include <windows.h>
#include <winhttp.h>

namespace http {

namespace {

struct Handle {
    HINTERNET value { nullptr };
    explicit Handle(HINTERNET h) : value(h) { }
    ~Handle() { if (value) WinHttpCloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value != nullptr; }
};

std::wstring lastError(const wchar_t* step)
{
    DWORD code = GetLastError();
    std::wstring message = util::formatWindowsError(code);
    // WinHTTP errors aren't in the system message table.
    if (message == L"Unknown error" || message.empty()) {
        HMODULE module = GetModuleHandleW(L"winhttp.dll");
        wchar_t* text = nullptr;
        if (module && FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                module, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr) && text) {
            message = text;
            while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r'))
                message.pop_back();
        }
        LocalFree(text);
    }
    return std::wstring(step) + L" failed: " + message + L" (" + std::to_wstring(code) + L")";
}

} // namespace

bool get(const std::wstring& url, std::string& body, std::wstring& error, size_t maxBytes)
{
    body.clear();

    URL_COMPONENTS parts = { sizeof(parts) };
    wchar_t host[256] = { };
    wchar_t path[4096] = { };
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    wchar_t extra[4096] = { };
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        error = L"Not a valid address: " + url;
        return false;
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = L"Only https:// downloads are allowed.";
        return false;
    }

    std::wstring agent = L"Prism/" PRISM_VERSION_WSTRING;
    // Use the system's proxy settings, like other Windows apps.
    HINTERNET rawSession = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!rawSession) // Windows versions before 8.1 don't know AUTOMATIC_PROXY.
        rawSession = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    Handle session(rawSession);
    if (!session) {
        error = lastError(L"Starting the download");
        return false;
    }
    WinHttpSetTimeouts(session.value, 15000, 15000, 30000, 60000);

    Handle connection(WinHttpConnect(session.value, host, parts.nPort, 0));
    if (!connection) {
        error = lastError(L"Connecting");
        return false;
    }
    std::wstring object = std::wstring(path) + extra;
    Handle request(WinHttpOpenRequest(connection.value, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) {
        error = lastError(L"Opening the request");
        return false;
    }
    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request.value, nullptr)) {
        error = lastError(L"Downloading");
        return false;
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = L"The server answered with HTTP " + std::to_wstring(status) + (status == 404 ? L" (not found)" : L"");
        return false;
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available)) {
            error = lastError(L"Downloading");
            return false;
        }
        if (!available)
            break;
        size_t old = body.size();
        if (old + available > maxBytes) {
            error = L"The download is too large.";
            return false;
        }
        body.resize(old + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.value, body.data() + old, available, &read)) {
            error = lastError(L"Downloading");
            return false;
        }
        body.resize(old + read);
    }
    return true;
}

} // namespace http
