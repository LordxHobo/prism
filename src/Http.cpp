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

bool get(const std::wstring& url, std::string& body, std::wstring& error, size_t maxBytes, const Progress& progress)
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

    uint64_t total = 0;
    {
        wchar_t length[32] = { };
        DWORD lengthSize = sizeof(length);
        if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &lengthSize, WINHTTP_NO_HEADER_INDEX))
            total = _wcstoui64(length, nullptr, 10);
    }
    if (progress)
        progress(0, total);
    ULONGLONG lastReport = GetTickCount64();

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
        if (progress && GetTickCount64() - lastReport >= 100) {
            lastReport = GetTickCount64();
            progress(body.size(), total);
        }
    }
    if (progress)
        progress(body.size(), total ? total : body.size());
    return true;
}

bool request(const Request& input, Response& output, std::wstring& error, size_t maxBytes)
{
    output = { };
    URL_COMPONENTS parts = { sizeof(parts) };
    wchar_t host[256] = { };
    wchar_t path[8192] = { };
    wchar_t extra[8192] = { };
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(input.url.c_str(), 0, 0, &parts) || (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP)) {
        error = L"Not a valid address: " + input.url;
        return false;
    }

    std::wstring agent = input.userAgent.empty() ? std::wstring(L"Prism/" PRISM_VERSION_WSTRING) : input.userAgent;
    HINTERNET rawSession = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!rawSession)
        rawSession = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    Handle session(rawSession);
    if (!session) {
        error = lastError(L"Starting the request");
        return false;
    }
    WinHttpSetTimeouts(session.value, 15000, 15000, 30000, 60000);
    Handle connection(WinHttpConnect(session.value, host, parts.nPort, 0));
    if (!connection) {
        error = lastError(L"Connecting");
        return false;
    }
    std::wstring object = std::wstring(path) + extra;
    Handle handle(WinHttpOpenRequest(connection.value, input.method.c_str(), object.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!handle) {
        error = lastError(L"Opening the request");
        return false;
    }
    DWORD noCookies = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(handle.value, WINHTTP_OPTION_DISABLE_FEATURE, &noCookies, sizeof(noCookies));
#ifdef WINHTTP_OPTION_DECOMPRESSION
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL; // answers arrive as plain bytes
    WinHttpSetOption(handle.value, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
#endif
    std::wstring headers;
    for (auto& [name, value] : input.headers) {
        // Headers a page couldn't set either.
        std::wstring lower = name;
        for (auto& c : lower)
            c = static_cast<wchar_t>(towlower(c));
        if (lower == L"cookie" || lower == L"host" || lower == L"content-length" || lower == L"connection" || name.find_first_of(L"\r\n:") != std::wstring::npos
            || value.find_first_of(L"\r\n") != std::wstring::npos)
            continue;
        headers += name + L": " + value + L"\r\n";
    }
    if (!WinHttpSendRequest(handle.value, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : static_cast<DWORD>(-1L),
            input.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(input.body.data()), static_cast<DWORD>(input.body.size()), static_cast<DWORD>(input.body.size()), 0)
        || !WinHttpReceiveResponse(handle.value, nullptr)) {
        error = lastError(L"Requesting");
        return false;
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(handle.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    output.status = static_cast<int>(status);
    DWORD length = 0;
    WinHttpQueryHeaders(handle.value, WINHTTP_QUERY_STATUS_TEXT, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &length, WINHTTP_NO_HEADER_INDEX);
    if (length) {
        std::wstring text(length / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(handle.value, WINHTTP_QUERY_STATUS_TEXT, WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &length, WINHTTP_NO_HEADER_INDEX))
            output.statusText = text.c_str();
    }
    length = 0;
    WinHttpQueryHeaders(handle.value, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &length, WINHTTP_NO_HEADER_INDEX);
    if (length) {
        std::wstring raw(length / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(handle.value, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &length, WINHTTP_NO_HEADER_INDEX)) {
            size_t start = raw.find(L"\r\n"); // skip the status line
            while (start != std::wstring::npos) {
                start += 2;
                size_t end = raw.find(L"\r\n", start);
                std::wstring line = raw.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
                size_t colon = line.find(L':');
                if (colon != std::wstring::npos) {
                    std::wstring name = line.substr(0, colon), value = line.substr(colon + 1);
                    while (!value.empty() && value[0] == L' ')
                        value.erase(0, 1);
                    std::wstring lower = name;
                    for (auto& c : lower)
                        c = static_cast<wchar_t>(towlower(c));
                    if (lower != L"set-cookie")
                        output.headers.emplace_back(name, value);
                }
                start = end;
            }
        }
    }
    wchar_t finalURL[8192] = { };
    DWORD finalSize = sizeof(finalURL);
    output.url = WinHttpQueryOption(handle.value, WINHTTP_OPTION_URL, finalURL, &finalSize) ? finalURL : input.url;

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(handle.value, &available)) {
            error = lastError(L"Reading the answer");
            return false;
        }
        if (!available)
            break;
        size_t old = output.body.size();
        if (old + available > maxBytes) {
            error = L"The answer is too large.";
            return false;
        }
        output.body.resize(old + available);
        DWORD read = 0;
        if (!WinHttpReadData(handle.value, output.body.data() + old, available, &read)) {
            error = lastError(L"Reading the answer");
            return false;
        }
        output.body.resize(old + read);
    }
    return true;
}

} // namespace http
