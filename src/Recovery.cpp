// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Recovery.h"

#include "Util.h"

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace recovery {

namespace {

int s_recoverableTabs = 0;

std::wstring runningFile(DWORD pid)
{
    return util::dataDirectory() + L"\\running-" + std::to_wstring(pid) + L".txt";
}

std::wstring sessionFile()
{
    return util::dataDirectory() + L"\\session.txt";
}

unsigned long long creationTime(HANDLE process)
{
    FILETIME created { }, exited { }, kernel { }, user { };
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
        return 0;
    return (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

// Whether the Prism that wrote a running-<pid>.txt is still running (Prism
// can have more than one copy open). Process IDs get reused, so the note
// also has the process's start time.
bool stillRunning(DWORD pid, unsigned long long started)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    DWORD exitCode = 0;
    bool running = GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE && creationTime(process) == started;
    CloseHandle(process);
    return running;
}

int countTabs(const std::string& session)
{
    int count = 0;
    size_t position = 0;
    while ((position = session.find("T\t", position)) != std::string::npos) {
        if (position == 0 || session[position - 1] == '\n')
            ++count;
        position += 2;
    }
    return count;
}

// ---- Crash reports ----------------------------------------------------------

using MiniDumpWriteDumpFunction = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
    PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

MiniDumpWriteDumpFunction s_writeDump = nullptr;
wchar_t s_crashFolder[MAX_PATH] { };

void pruneCrashReports(const std::wstring& folder)
{
    std::vector<std::pair<unsigned long long, std::wstring>> reports;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((folder + L"\\prism-*.dmp").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        unsigned long long written = (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
        reports.emplace_back(written, folder + L"\\" + data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    std::sort(reports.begin(), reports.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t i = 5; i < reports.size(); ++i)
        DeleteFileW(reports[i].second.c_str());
}

// Runs in the crashing process, so it only uses what was prepared up front
// and plain Windows calls (no allocations from Prism's own code).
LONG WINAPI writeCrashReport(EXCEPTION_POINTERS* exception)
{
    static volatile LONG entered = 0;
    if (InterlockedExchange(&entered, 1) || !s_writeDump || !s_crashFolder[0])
        return EXCEPTION_CONTINUE_SEARCH;

    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t path[MAX_PATH + 64];
    swprintf_s(path, L"%s\\prism-%04u%02u%02u-%02u%02u%02u-%lu.dmp", s_crashFolder, now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info { };
        info.ThreadId = GetCurrentThreadId();
        info.ExceptionPointers = exception;
        info.ClientPointers = FALSE;
        // Threads, stacks and loaded modules: enough to see where it crashed,
        // without page contents or memory that could hold personal data.
        auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        s_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &info, nullptr, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

bool start()
{
    std::wstring folder = util::dataDirectory();

    // Notes left by Prisms that are gone mean they didn't close normally.
    bool crashed = false;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((folder + L"\\running-*.txt").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring path = folder + L"\\" + data.cFileName;
            DWORD pid = wcstoul(data.cFileName + wcslen(L"running-"), nullptr, 10);
            unsigned long long started = strtoull(util::readFile(path, 64).c_str(), nullptr, 10);
            if (pid == GetCurrentProcessId())
                continue;
            if (stillRunning(pid, started))
                continue;
            crashed = true;
            DeleteFileW(path.c_str());
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }

    // This Prism's own note.
    unsigned long long started = creationTime(GetCurrentProcess());
    util::writeFileAtomically(runningFile(GetCurrentProcessId()), std::to_string(started));

    s_recoverableTabs = 0;
    if (crashed) {
        // Keep the tabs from before the crash aside before this run saves
        // its own session over them.
        std::string session = util::readFile(sessionFile());
        int tabs = countTabs(session);
        if (tabs > 0 && util::writeFileAtomically(recoveryFile(), session))
            s_recoverableTabs = tabs;
    } else {
        // An offer from an earlier crash that was never answered.
        std::string kept = util::readFile(recoveryFile());
        s_recoverableTabs = countTabs(kept);
    }
    return crashed;
}

void finish()
{
    DeleteFileW(runningFile(GetCurrentProcessId()).c_str());
}

int recoverableTabs()
{
    return s_recoverableTabs;
}

std::wstring recoveryFile()
{
    return util::dataDirectory() + L"\\session-recovery.txt";
}

void discard()
{
    s_recoverableTabs = 0;
    DeleteFileW(recoveryFile().c_str());
}

void installCrashHandler()
{
    std::wstring folder = util::dataDirectory() + L"\\crashes";
    util::ensureDirectory(folder);
    pruneCrashReports(folder);
    if (folder.size() >= MAX_PATH)
        return;
    wcscpy_s(s_crashFolder, folder.c_str());
    if (HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        s_writeDump = reinterpret_cast<MiniDumpWriteDumpFunction>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    SetUnhandledExceptionFilter(writeCrashReport);
}

} // namespace recovery
