// Prism — a private, independent web browser built on WebKit.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "BrowserWindow.h"
#include "Engine.h"
#include "Settings.h"
#include "Util.h"
#include "Version.h"

#include <ole2.h>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // Only load DLLs from Prism's own folder and System32, never from the
    // current directory (protects against DLL planting).
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    OleInitialize(nullptr);

    engine::deleteProfileIfRequested();

    std::wstring error;
    if (!engine::initialize(error)) {
        MessageBoxW(nullptr, error.c_str(), L"Prism can't start", MB_OK | MB_ICONERROR);
        OleUninitialize();
        return 1;
    }

    if (!BrowserWindow::registerWindowClass(instance)) {
        MessageBoxW(nullptr, L"Couldn't create Prism's window.", L"Prism can't start", MB_OK | MB_ICONERROR);
        OleUninitialize();
        return 1;
    }

    // Pages to open: last time's tabs (if "continue where you left off" is
    // on), plus anything passed on the command line, otherwise a new tab.
    // After an update, the "What's new" page opens once.
    std::vector<std::wstring> urls;
    int argc = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = argv[i];
            if (arg.empty() || arg[0] == L'-')
                continue;
            if (GetFileAttributesW(arg.c_str()) != INVALID_FILE_ATTRIBUTES)
                arg = util::fileURLFromPath(arg); // a local file, e.g. C:\page.html
            else if (arg.find(L':') == std::wstring::npos)
                arg = L"https://" + arg; // "example.com"
            urls.push_back(arg);
        }
        LocalFree(argv);
    }
    if (util::readSetting(L"LastVersion") != PRISM_VERSION_WSTRING) {
        urls.push_back(L"prism://whatsnew");
        util::writeSetting(L"LastVersion", PRISM_VERSION_WSTRING);
    }

    bool opened = settings::restoreSession() && BrowserWindow::restoreSession(urls);
    if (!opened) {
        if (urls.empty() || urls.front() == L"prism://whatsnew")
            urls.insert(urls.begin(), L"prism://newtab");
        opened = BrowserWindow::open(urls) != nullptr;
    }
    if (!opened) {
        MessageBoxW(nullptr, L"Couldn't open a browser window.", L"Prism can't start", MB_OK | MB_ICONERROR);
        OleUninitialize();
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (auto* window = BrowserWindow::fromHWND(msg.hwnd)) {
            if (window->handleShortcut(msg))
                continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (settings::clearOnExit()) {
        // Delete cookies, site data and caches now. Give WebKit up to five
        // seconds; anything left over is wiped at the next start.
        bool done = false;
        engine::clearBrowsingData(true, true, [&done] { done = true; });
        ULONGLONG deadline = GetTickCount64() + 5000;
        while (!done && GetTickCount64() < deadline) {
            MSG pending;
            if (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&pending);
                DispatchMessageW(&pending);
            } else
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
        }
    }

    OleUninitialize();
    return static_cast<int>(msg.wParam);
}
