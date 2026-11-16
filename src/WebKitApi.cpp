// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "WebKitApi.h"

#include "Util.h"

#include <vector>

namespace wk {

#define PRISM_WK_DEFINE(name, ret, params) ret (*name) params = nullptr;
PRISM_WK_REQUIRED(PRISM_WK_DEFINE)
PRISM_WK_OPTIONAL(PRISM_WK_DEFINE)
#undef PRISM_WK_DEFINE

bool load(std::wstring& error)
{
    std::wstring dir = util::exeDirectory();
    std::wstring dllPath = dir + L"\\WebKit2.dll";

    // WebKit's own DLLs and its support libraries live next to Prism.exe.
    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR makes Windows look for WebKit2.dll's
    // dependencies in that same folder (and not in random PATH folders).
    HMODULE module = LoadLibraryExW(dllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
        DWORD code = GetLastError();
        error = L"Prism couldn't load the WebKit engine.\n\n"
            L"Expected it here:\n" + dllPath + L"\n\n"
            L"Windows error " + std::to_wstring(code) + L": " + util::formatWindowsError(code) + L"\n\n"
            L"Run scripts\\fetch-webkit.ps1 (see docs\\BUILDING.md) to download the WebKit runtime, "
            L"and install the Microsoft Visual C++ Redistributable (x64) if it isn't installed yet.";
        return false;
    }

    std::vector<std::wstring> missing;

#define PRISM_WK_LOAD_REQUIRED(name, ret, params) \
    name = reinterpret_cast<ret (*) params>(reinterpret_cast<void*>(GetProcAddress(module, #name))); \
    if (!name) missing.push_back(util::fromUTF8(#name));
    PRISM_WK_REQUIRED(PRISM_WK_LOAD_REQUIRED)
#undef PRISM_WK_LOAD_REQUIRED

#define PRISM_WK_LOAD_OPTIONAL(name, ret, params) \
    name = reinterpret_cast<ret (*) params>(reinterpret_cast<void*>(GetProcAddress(module, #name)));
    PRISM_WK_OPTIONAL(PRISM_WK_LOAD_OPTIONAL)
#undef PRISM_WK_LOAD_OPTIONAL

    if (!missing.empty()) {
        error = L"This WebKit build is missing functions Prism needs:\n\n";
        for (size_t i = 0; i < missing.size() && i < 12; ++i)
            error += L"  " + missing[i] + L"\n";
        if (missing.size() > 12)
            error += L"  ...and " + std::to_wstring(missing.size() - 12) + L" more\n";
        error += L"\nThe WebKit runtime is probably too old or too new for this version of Prism. "
            L"Re-run scripts\\fetch-webkit.ps1 to get the version pinned in webkit.lock.";
        return false;
    }
    return true;
}

Ref<WKStringRef> stringUTF8(const std::string& value)
{
    return Ref<WKStringRef>::adopt(WKStringCreateWithUTF8CString(value.c_str()));
}

Ref<WKStringRef> string(const std::wstring& value)
{
    return stringUTF8(util::toUTF8(value));
}

Ref<WKURLRef> url(const std::wstring& value)
{
    return Ref<WKURLRef>::adopt(WKURLCreateWithUTF8CString(util::toUTF8(value).c_str()));
}

std::wstring toWide(WKStringRef value)
{
    if (!value)
        return { };
    size_t size = WKStringGetMaximumUTF8CStringSize(value);
    std::string buffer(size, '\0');
    size_t length = WKStringGetUTF8CString(value, buffer.data(), size);
    // The returned length includes the terminating NUL.
    if (length > 0)
        buffer.resize(length - 1);
    else
        buffer.clear();
    return util::fromUTF8(buffer);
}

std::wstring toWide(WKURLRef value)
{
    if (!value)
        return { };
    auto str = Ref<WKStringRef>::adopt(WKURLCopyString(value));
    return toWide(str.get());
}

} // namespace wk
