// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Perf.h"

#include "Json.h"

#include <windows.h>

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace perf {

namespace {

std::atomic<ULONGLONG> g_beat { 0 };   // last time the main thread was free
std::mutex g_mutex;                    // guards everything below
std::string g_current;                 // what the main thread is doing
std::string g_stallLabel;              // what it was doing when the stall was noticed
std::deque<std::string> g_lines;
int g_stalls = 0;
ULONGLONG g_longest = 0;
struct WebRequestStats {
    int slow { 0 };
    int timedOut { 0 };
    ULONGLONG worst { 0 };
    ULONGLONG lastLine { 0 };
    int sinceLine { 0 };
};
std::map<std::string, WebRequestStats> g_webRequest;

constexpr ULONGLONG kStall = 250; // ms without a heartbeat

std::string stamp()
{
    SYSTEMTIME now;
    GetLocalTime(&now);
    char text[16];
    snprintf(text, sizeof(text), "%02d:%02d:%02d", now.wHour, now.wMinute, now.wSecond);
    return text;
}

void addLine(std::string line)
{
    // g_mutex held
    g_lines.push_back(stamp() + "  " + std::move(line));
    while (g_lines.size() > 200)
        g_lines.pop_front();
}

void CALLBACK heartbeat(HWND, UINT, UINT_PTR, DWORD)
{
    ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_beat.exchange(now);
    // Timers only run when the main thread is free, so a long gap is a stall.
    if (last && now - last > kStall + 100) {
        ULONGLONG busy = now - last;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stalls++;
        g_longest = std::max(g_longest, busy);
        std::string what = g_stallLabel.empty() ? std::string("(not inside a marked task: page or WebKit work)") : g_stallLabel;
        addLine("Prism was busy for " + std::to_string(busy) + " ms: " + what + ". Pages waited meanwhile.");
        g_stallLabel.clear();
    }
}

void watchdog()
{
    for (;;) {
        Sleep(100);
        ULONGLONG last = g_beat.load();
        if (last && GetTickCount64() - last > kStall) {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_stallLabel.empty() && !g_current.empty())
                g_stallLabel = g_current;
        }
    }
}

} // namespace

void start()
{
    static bool started = false;
    if (started)
        return;
    started = true;
    g_beat = GetTickCount64();
    SetTimer(nullptr, 0, 50, heartbeat);
    std::thread(watchdog).detach();
}

Scope::Scope(const char* what, const std::string& detail)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    m_previous = std::move(g_current);
    g_current = detail.empty() ? std::string(what) : std::string(what) + " (" + detail.substr(0, 160) + ")";
}

Scope::~Scope()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_current = std::move(m_previous);
}

void noteWebRequest(const std::string& extensionName, unsigned long long waitedMs, bool timedOut, const std::string& url)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    WebRequestStats& stats = g_webRequest[extensionName];
    stats.slow++;
    stats.sinceLine++;
    if (timedOut)
        stats.timedOut++;
    stats.worst = std::max<ULONGLONG>(stats.worst, waitedMs);
    // One line per extension every 5 seconds at most.
    ULONGLONG now = GetTickCount64();
    if (now - stats.lastLine < 5000)
        return;
    std::string host = url;
    size_t start = host.find("://");
    if (start != std::string::npos)
        host = host.substr(start + 3);
    host = host.substr(0, host.find_first_of("/?#"));
    addLine(extensionName + " made a request to " + host + " wait " + std::to_string(waitedMs) + " ms" + (timedOut ? " (no answer; Prism stopped waiting)" : "")
        + (stats.sinceLine > 1 ? "; " + std::to_string(stats.sinceLine) + " slow requests since the last note" : "") + ".");
    stats.lastLine = now;
    stats.sinceLine = 0;
}

void note(const std::string& text)
{
    static std::map<std::string, ULONGLONG> s_last;
    std::lock_guard<std::mutex> lock(g_mutex);
    ULONGLONG now = GetTickCount64();
    auto found = s_last.find(text);
    if (found != s_last.end() && now - found->second < 60000)
        return;
    if (s_last.size() > 64)
        s_last.clear();
    s_last[text] = now;
    addLine(text.size() > 400 ? text.substr(0, 400) + "..." : text);
}

std::string logJSON()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string json = "{\"stalls\":" + std::to_string(g_stalls) + ",\"longest\":" + std::to_string(g_longest) + ",\"extensions\":[";
    bool first = true;
    for (auto& [name, stats] : g_webRequest) {
        json += std::string(first ? "" : ",") + "{\"name\":" + json::quote(name) + ",\"slow\":" + std::to_string(stats.slow) + ",\"timedOut\":"
            + std::to_string(stats.timedOut) + ",\"worst\":" + std::to_string(stats.worst) + "}";
        first = false;
    }
    json += "],\"lines\":[";
    first = true;
    for (auto& line : g_lines) {
        json += std::string(first ? "" : ",") + json::quote(line);
        first = false;
    }
    return json + "]}";
}

void clear()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lines.clear();
    g_webRequest.clear();
    g_stalls = 0;
    g_longest = 0;
}

} // namespace perf
