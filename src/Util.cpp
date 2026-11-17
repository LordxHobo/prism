// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Util.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <vector>
#include <cwchar>
#include <memory>

namespace util {

std::string toUTF8(const std::wstring& value)
{
    if (value.empty())
        return { };
    int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring fromUTF8(const std::string& value)
{
    if (value.empty())
        return { };
    int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::wstring exeDirectory()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD copied = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (copied < path.size()) {
            path.resize(copied);
            break;
        }
        path.resize(path.size() * 2);
    }
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(0, slash);
}

static std::wstring knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR raw = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) && raw)
        result = raw;
    CoTaskMemFree(raw);
    return result;
}

void ensureDirectory(const std::wstring& path)
{
    SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
}

std::wstring dataDirectory()
{
    static std::wstring dir;
    if (dir.empty()) {
        dir = knownFolder(FOLDERID_LocalAppData) + L"\\Prism";
        ensureDirectory(dir);
    }
    return dir;
}

std::wstring downloadsDirectory()
{
    std::wstring dir = knownFolder(FOLDERID_Downloads);
    if (dir.empty())
        dir = knownFolder(FOLDERID_Profile) + L"\\Downloads";
    ensureDirectory(dir);
    return dir;
}

std::wstring fileURLFromPath(const std::wstring& path)
{
    wchar_t buffer[4096];
    DWORD size = ARRAYSIZE(buffer);
    if (SUCCEEDED(UrlCreateFromPathW(path.c_str(), buffer, &size, 0)))
        return buffer;
    std::wstring fallback = L"file:///" + path;
    for (auto& c : fallback) {
        if (c == L'\\')
            c = L'/';
    }
    return fallback;
}

std::wstring formatWindowsError(DWORD code)
{
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring result = text ? text : L"Unknown error";
    LocalFree(text);
    while (!result.empty() && (result.back() == L'\n' || result.back() == L'\r'))
        result.pop_back();
    return result;
}

std::wstring urlEncode(const std::wstring& value)
{
    static const char hex[] = "0123456789ABCDEF";
    std::wstring out;
    for (unsigned char c : toUTF8(value)) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<wchar_t>(c);
        else {
            out += L'%';
            out += static_cast<wchar_t>(hex[c >> 4]);
            out += static_cast<wchar_t>(hex[c & 15]);
        }
    }
    return out;
}

std::wstring urlDecode(const std::wstring& value)
{
    std::string utf8 = toUTF8(value);
    std::string out;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < utf8.size(); ++i) {
        if (utf8[i] == '%' && i + 2 < utf8.size() && hex(utf8[i + 1]) >= 0 && hex(utf8[i + 2]) >= 0) {
            out += static_cast<char>(hex(utf8[i + 1]) * 16 + hex(utf8[i + 2]));
            i += 2;
        } else
            out += utf8[i];
    }
    return fromUTF8(out);
}

std::string jsonString(const std::wstring& value)
{
    std::string utf8 = toUTF8(value);
    std::string out;
    out.reserve(utf8.size() + 2);
    out += '"';
    for (unsigned char c : utf8) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '<': out += "\\u003c"; break; // keeps "</script>" harmless
        default:
            if (c < 0x20) {
                char buf[8];
                wsprintfA(buf, "\\u%04x", c);
                out += buf;
            } else
                out += static_cast<char>(c);
        }
    }
    // U+2028 / U+2029 are valid in JSON but not in older JS string literals.
    size_t pos = 0;
    while ((pos = out.find("\xE2\x80\xA8", pos)) != std::string::npos)
        out.replace(pos, 3, "\\u2028");
    pos = 0;
    while ((pos = out.find("\xE2\x80\xA9", pos)) != std::string::npos)
        out.replace(pos, 3, "\\u2029");
    out += '"';
    return out;
}

std::wstring uniqueFilePath(const std::wstring& folder, std::wstring name)
{
    for (auto& c : name) {
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c))
            c = L'_';
    }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' '))
        name.pop_back();
    if (name.empty())
        name = L"download";

    std::wstring stem = name, ext;
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        stem = name.substr(0, dot);
        ext = name.substr(dot);
    }

    std::wstring candidate = folder + L"\\" + name;
    for (int i = 1; GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES && i < 1000; ++i)
        candidate = folder + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
    return candidate;
}

bool writeFileAtomically(const std::wstring& path, const std::string& contents)
{
    std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    bool ok = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) && written == contents.size();
    ok = FlushFileBuffers(file) && ok;
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

std::string readFile(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return { };
    std::string contents;
    LARGE_INTEGER size;
    if (GetFileSizeEx(file, &size) && size.QuadPart < 16 * 1024 * 1024) {
        contents.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr))
            read = 0;
        contents.resize(read);
    }
    CloseHandle(file);
    return contents;
}

static std::wstring settingsFile()
{
    return dataDirectory() + L"\\prism.ini";
}

std::wstring readSetting(const wchar_t* key, const wchar_t* fallback)
{
    std::vector<wchar_t> buffer(4096);
    GetPrivateProfileStringW(L"Prism", key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), settingsFile().c_str());
    return buffer.data();
}

void writeSetting(const wchar_t* key, const std::wstring& value)
{
    WritePrivateProfileStringW(L"Prism", key, value.c_str(), settingsFile().c_str());
}

namespace {

constexpr UINT WM_PRISM_TASK = WM_APP + 50;
HWND s_dispatcher = nullptr;

LRESULT CALLBACK dispatcherProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_PRISM_TASK) {
        std::unique_ptr<std::function<void()>> task(reinterpret_cast<std::function<void()>*>(lParam));
        if (task && *task)
            (*task)();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

void initMainThread()
{
    if (s_dispatcher)
        return;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = dispatcherProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PrismMainThreadDispatcher";
    RegisterClassExW(&wc);
    // A message-only window: never shown, just receives posted tasks.
    s_dispatcher = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
}

void postToMainThread(std::function<void()> task)
{
    auto* heap = new std::function<void()>(std::move(task));
    if (!s_dispatcher || !PostMessageW(s_dispatcher, WM_PRISM_TASK, 0, reinterpret_cast<LPARAM>(heap)))
        delete heap;
}

std::string base64(const std::string& bytes)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        unsigned n = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) | static_cast<unsigned char>(bytes[i + 2]);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += table[n & 63];
    }
    if (i < bytes.size()) {
        unsigned n = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size())
            n |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? table[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

} // namespace util
