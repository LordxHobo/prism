// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Passwords.h"

#include "BrowserWindow.h"
#include "Dialogs.h"
#include "Engine.h"
#include "Json.h"
#include "PasswordVault.h"
#include "Util.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

using namespace wk;

namespace passwords {

namespace {

// A login sent on a tab, waiting for "Save password?".
struct Pending {
    std::string origin;
    std::string username;
    std::string password;
    std::string existingId; // an update of this login
    bool shown { true };    // on the info bar (false while waiting for an unlock)
    bool saveAfterUnlock { false };
    bool neverAfterUnlock { false };
    int commits { 0 };
    ULONGLONG at { 0 };
};

std::map<int, Pending> g_pending;
std::set<std::string> g_neverThisSession; // "Never" while the vault was locked
std::set<int> g_lockedMenuShown;          // tabs that already showed "Unlock…" since their last navigation
ULONGLONG g_lastUse = 0;
ULONGLONG g_verifiedAt = 0;               // master password last typed
UINT_PTR g_timer = 0;
// Clipboard copies of passwords are cleared again after a while.
DWORD g_clipboardSequence = 0;
ULONGLONG g_clipboardAt = 0;

constexpr ULONGLONG kVerifyWindow = 5 * 60 * 1000;
constexpr ULONGLONG kClipboardClear = 45 * 1000;

void used()
{
    g_lastUse = GetTickCount64();
}

int autoLockMinutes()
{
    std::wstring value = util::readSetting(L"PasswordsAutoLock", L"15");
    int minutes = _wtoi(value.c_str());
    return minutes < 0 ? 15 : minutes;
}

bool recentlyVerified()
{
    return g_verifiedAt && GetTickCount64() - g_verifiedAt < kVerifyWindow;
}

std::wstring wide(const std::string& text)
{
    return util::fromUTF8(text);
}

std::string displaySite(const std::string& origin)
{
    size_t scheme = origin.find("://");
    std::string host = scheme == std::string::npos ? origin : origin.substr(scheme + 3);
    if (host.rfind("www.", 0) == 0)
        host.erase(0, 4);
    return host;
}

bool findTab(WKPageRef page, BrowserWindow*& window, Tab*& tab)
{
    for (BrowserWindow* candidate : BrowserWindow::all()) {
        for (auto& item : candidate->tabList()) {
            if (item->page == page) {
                window = candidate;
                tab = item.get();
                return true;
            }
        }
    }
    return false;
}

bool findTab(int tabId, BrowserWindow*& window, Tab*& tab)
{
    for (BrowserWindow* candidate : BrowserWindow::all()) {
        for (auto& item : candidate->tabList()) {
            if (item->id == tabId) {
                window = candidate;
                tab = item.get();
                return true;
            }
        }
    }
    return false;
}

// The site a tab shows, as Prism knows it (never what the page says).
std::string tabOrigin(const Tab* tab)
{
    if (!tab || tab->extensionPages)
        return { };
    std::string origin = vault::originOf(util::toUTF8(tab->url));
    std::string host = origin.substr(origin.find("://") == std::string::npos ? 0 : origin.find("://") + 3);
    size_t port = host.rfind(':');
    std::string bare = port == std::string::npos ? host : host.substr(0, port);
    if (bare == "localhost" || (bare.size() > 10 && bare.compare(bare.size() - 10, 10, ".localhost") == 0 && port != std::string::npos))
        return { }; // extension pages (and their local server)
    return origin;
}

void wipePending(Pending& pending)
{
    vault::wipe(pending.password);
}

void erasePending(int tabId)
{
    auto found = g_pending.find(tabId);
    if (found == g_pending.end())
        return;
    wipePending(found->second);
    g_pending.erase(found);
}

// Saves a waiting login (the vault must be open). Returns a message.
std::wstring savePending(Pending& pending)
{
    vault::Login login;
    if (!pending.existingId.empty()) {
        if (const vault::Login* existing = vault::find(pending.existingId))
            login = *existing;
    } else {
        // An update after all (the vault was locked when it was sent)?
        for (const vault::Login* candidate : vault::matching(pending.origin)) {
            if (candidate->origin == pending.origin && candidate->username == pending.username) {
                login = *candidate;
                break;
            }
        }
    }
    bool update = !login.id.empty();
    login.origin = pending.origin;
    login.username = pending.username;
    vault::wipe(login.password);
    login.password = pending.password;
    std::string id = vault::put(login);
    vault::wipe(login.password);
    used();
    if (id.empty())
        return L"Prism Passwords is locked.";
    return update ? L"Password updated" : L"Password saved";
}

void offerChanged(int tabId)
{
    BrowserWindow::passwordOfferChanged(tabId);
}

// After the vault is opened from prism://passwords: logins that were waiting
// for it.
std::wstring finishWaiting()
{
    std::wstring message;
    for (auto& [tabId, pending] : g_pending) {
        if (pending.saveAfterUnlock) {
            message = savePending(pending);
            pending.saveAfterUnlock = false;
            pending.shown = false;
        }
    }
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (!it->second.shown) {
            wipePending(it->second);
            it = g_pending.erase(it);
        } else
            ++it;
    }
    for (auto& origin : g_neverThisSession)
        vault::setNeverSave(origin, true);
    g_neverThisSession.clear();
    return message;
}

// ---- Clipboard --------------------------------------------------------------

bool copyText(HWND owner, const std::string& text, bool secret)
{
    std::wstring value = wide(text);
    if (!OpenClipboard(owner))
        return false;
    EmptyClipboard();
    size_t bytes = (value.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (memory) {
        memcpy(GlobalLock(memory), value.c_str(), bytes);
        GlobalUnlock(memory);
        ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        if (!ok)
            GlobalFree(memory);
    }
    if (ok && secret) {
        // Keep it out of Windows' clipboard history and cloud clipboard.
        UINT exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
        if (HGLOBAL flag = GlobalAlloc(GMEM_MOVEABLE, 1)) {
            if (!SetClipboardData(exclude, flag))
                GlobalFree(flag);
        }
        for (const wchar_t* name : { L"CanIncludeInClipboardHistory", L"CanUploadToCloudClipboard" }) {
            if (HGLOBAL zero = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD))) {
                if (!SetClipboardData(RegisterClipboardFormatW(name), zero))
                    GlobalFree(zero);
            }
        }
    }
    CloseClipboard();
    SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
    if (ok && secret) {
        g_clipboardSequence = GetClipboardSequenceNumber();
        g_clipboardAt = GetTickCount64();
    }
    return ok;
}

void CALLBACK timerTick(HWND, UINT, UINT_PTR, DWORD)
{
    ULONGLONG now = GetTickCount64();
    // A copied password is cleared unless something else was copied since.
    if (g_clipboardAt && now - g_clipboardAt > kClipboardClear) {
        if (GetClipboardSequenceNumber() == g_clipboardSequence && OpenClipboard(nullptr)) {
            EmptyClipboard();
            CloseClipboard();
        }
        g_clipboardAt = 0;
    }
    // Auto-lock (not when Windows opens the vault anyway).
    int minutes = autoLockMinutes();
    if (vault::state() == vault::State::Unlocked && !vault::windowsUnlockEnabled() && minutes > 0 && now - g_lastUse > static_cast<ULONGLONG>(minutes) * 60 * 1000) {
        vault::lock();
        g_verifiedAt = 0;
        BrowserWindow::evaluateInInternalPages(L"passwords", "window.prismPasswordsChanged && prismPasswordsChanged();");
    }
}

// ---- The login menu on login fields ------------------------------------------------

void fill(Tab* tab, const std::string& origin, const std::string& dataJSON)
{
    // The tab may have moved on while the menu was open.
    if (tabOrigin(tab) != origin)
        return;
    std::string script = "window.__prismPasswords && window.__prismPasswords.fill(" + dataJSON + ");";
    WKPageEvaluateJavaScriptInMainFrame(tab->page, stringUTF8(script).get(), nullptr, nullptr);
    vault::wipe(script);
}

void showFieldMenu(BrowserWindow* window, Tab* tab, const std::string& origin, const std::string& kind, const json::Value& rect)
{
    vault::State state = vault::state();
    if (state == vault::State::None)
        return;
    HMENU menu = CreatePopupMenu();
    std::vector<std::string> ids;
    bool suggest = kind == "new-password";
    if (state == vault::State::Locked) {
        // Asked once per page, so it doesn't follow you around.
        if (kind == "username" || g_lockedMenuShown.count(tab->id)) {
            DestroyMenu(menu);
            return;
        }
        g_lockedMenuShown.insert(tab->id);
        AppendMenuW(menu, MF_STRING, 1, L"Unlock Prism Passwords\x2026");
    } else {
        auto logins = vault::matching(origin);
        if (logins.empty() && !suggest) {
            DestroyMenu(menu);
            return;
        }
        if (suggest)
            AppendMenuW(menu, MF_STRING, 2, L"Suggest a strong password");
        if (suggest && !logins.empty())
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        for (const vault::Login* login : logins) {
            if (ids.size() >= 12)
                break;
            std::wstring label = login->username.empty() ? L"(no user name)" : wide(login->username);
            if (login->origin != origin)
                label += L"  \x2014  " + wide(displaySite(login->origin));
            for (size_t at = label.find(L'&'); at != std::wstring::npos; at = label.find(L'&', at + 2))
                label.insert(at, 1, L'&');
            AppendMenuW(menu, MF_STRING, 100 + ids.size(), label.c_str());
            ids.push_back(login->id);
        }
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, L"Manage passwords");

    POINT point = { static_cast<LONG>(rect["left"].asNumber()), static_cast<LONG>(rect["top"].asNumber() + rect["height"].asNumber()) };
    ClientToScreen(tab->hwnd, &point);
    UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, point.x, point.y + 2, 0, window->hwnd(), nullptr);
    DestroyMenu(menu);
    if (choice == 1 || choice == 3) {
        window->openInternalPage(L"passwords");
    } else if (choice == 2) {
        std::string generated = vault::randomPassword(20);
        std::string data = "{\"mode\":\"new\",\"password\":" + json::quote(generated) + "}";
        fill(tab, origin, data);
        vault::wipe(data);
        vault::wipe(generated);
    } else if (choice >= 100 && choice - 100 < ids.size()) {
        if (const vault::Login* login = vault::find(ids[choice - 100])) {
            std::string data = "{\"mode\":\"login\",\"username\":" + json::quote(login->username) + ",\"password\":" + json::quote(login->password) + "}";
            fill(tab, origin, data);
            vault::wipe(data);
            vault::markUsed(login->id);
            used();
        }
    }
}

// ---- CSV import and export ---------------------------------------------------

std::vector<std::vector<std::string>> parseCSV(const std::string& text)
{
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    size_t i = text.rfind("\xEF\xBB\xBF", 0) == 0 ? 3 : 0;
    for (; i < text.size(); i++) {
        char c = text[i];
        if (quoted) {
            if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') {
                field += '"';
                i++;
            } else if (c == '"')
                quoted = false;
            else
                field += c;
        } else if (c == '"')
            quoted = true;
        else if (c == ',') {
            row.push_back(std::move(field));
            field.clear();
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                i++;
            row.push_back(std::move(field));
            field.clear();
            if (!(row.size() == 1 && row[0].empty()))
                rows.push_back(std::move(row));
            row.clear();
        } else
            field += c;
    }
    if (!field.empty() || !row.empty()) {
        row.push_back(std::move(field));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string csvField(const std::string& value)
{
    if (value.find_first_of(",\"\r\n") == std::string::npos)
        return value;
    std::string out = "\"";
    for (char c : value)
        out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

// Chrome/Edge/Brave (name,url,username,password,note), Firefox (url,
// username,password,...), Bitwarden (login_uri, login_username,
// login_password, notes), 1Password and most others (url/website, username,
// password).
std::string importCSV(const std::string& text)
{
    auto rows = parseCSV(text);
    if (rows.size() < 2)
        return "{\"error\":\"That file has no passwords in it.\"}";
    auto column = [&](std::initializer_list<const char*> names) {
        for (const char* name : names) {
            for (size_t i = 0; i < rows[0].size(); i++) {
                std::string header = rows[0][i];
                std::transform(header.begin(), header.end(), header.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
                while (!header.empty() && (header.back() == ' ' || header.back() == '\t'))
                    header.pop_back();
                if (header == name)
                    return static_cast<int>(i);
            }
        }
        return -1;
    };
    int url = column({ "url", "login_uri", "website", "web site", "origin", "uri", "login url" });
    int user = column({ "username", "login_username", "user name", "login", "email", "user" });
    int pass = column({ "password", "login_password" });
    int note = column({ "note", "notes", "extra" });
    if (url < 0 || pass < 0)
        return "{\"error\":\"Prism couldn't find the address and password columns in that file.\"}";
    int added = 0, updated = 0, skipped = 0;
    for (size_t r = 1; r < rows.size(); r++) {
        auto& row = rows[r];
        auto cell = [&](int index) { return index >= 0 && static_cast<size_t>(index) < row.size() ? row[index] : std::string(); };
        std::string address = cell(url);
        // Bitwarden puts several addresses in one cell, one per line or comma.
        address = address.substr(0, address.find_first_of(",\r\n"));
        if (!address.empty() && address.find("://") == std::string::npos)
            address = "https://" + address;
        std::string origin = vault::originOf(address);
        std::string password = cell(pass);
        if (origin.empty() || password.empty()) {
            skipped++;
            vault::wipe(password);
            continue;
        }
        std::string username = cell(user);
        bool done = false;
        for (const vault::Login* existing : vault::matching(origin)) {
            if (existing->origin == origin && existing->username == username) {
                if (existing->password != password) {
                    vault::Login changed = *existing;
                    changed.password = password;
                    vault::put(changed);
                    vault::wipe(changed.password);
                    updated++;
                } else
                    skipped++;
                done = true;
                break;
            }
        }
        if (!done) {
            vault::Login login;
            login.origin = origin;
            login.username = username;
            login.password = password;
            login.note = cell(note);
            vault::put(login);
            vault::wipe(login.password);
            added++;
        }
        vault::wipe(password);
    }
    for (auto& row : rows)
        for (auto& value : row)
            vault::wipe(value);
    return "{\"added\":" + std::to_string(added) + ",\"updated\":" + std::to_string(updated) + ",\"skipped\":" + std::to_string(skipped) + "}";
}

std::string stateJSON()
{
    vault::State state = vault::state();
    std::string json = std::string("{\"state\":\"") + (state == vault::State::None ? "none" : state == vault::State::Locked ? "locked" : "unlocked") + "\"";
    json += ",\"windowsUnlock\":" + std::string(vault::windowsUnlockEnabled() ? "true" : "false");
    json += ",\"autoLock\":" + std::to_string(autoLockMinutes());
    json += ",\"verified\":" + std::string(recentlyVerified() ? "true" : "false");
    for (auto& [tabId, pending] : g_pending) {
        if (pending.saveAfterUnlock) {
            json += ",\"waiting\":{\"site\":" + json::quote(displaySite(pending.origin)) + ",\"username\":" + json::quote(pending.username) + "}";
            break;
        }
    }
    return json + "}";
}

std::string listJSON()
{
    std::string json = "{\"logins\":[";
    bool first = true;
    for (auto& login : vault::logins()) {
        json += std::string(first ? "" : ",") + "{\"id\":" + json::quote(login.id) + ",\"origin\":" + json::quote(login.origin)
            + ",\"site\":" + json::quote(displaySite(login.origin)) + ",\"username\":" + json::quote(login.username)
            + ",\"note\":" + json::quote(login.note) + ",\"updated\":" + std::to_string(static_cast<long long>(login.updated))
            + ",\"used\":" + std::to_string(static_cast<long long>(login.used)) + "}";
        first = false;
    }
    json += "],\"never\":[";
    first = true;
    for (auto& origin : vault::neverSaveList()) {
        json += std::string(first ? "" : ",") + "{\"origin\":" + json::quote(origin) + ",\"site\":" + json::quote(displaySite(origin)) + "}";
        first = false;
    }
    return json + "]}";
}

std::string errorJSON(const std::string& message)
{
    return "{\"error\":" + json::quote(message) + "}";
}

} // namespace

void initialize()
{
    vault::unlockWithWindows();
    used();
    std::string script = util::readFile(util::exeDirectory() + L"\\ui\\passwords-runtime.js");
    if (!script.empty())
        engine::addUserScript("passwords", script);
    if (!g_timer)
        g_timer = SetTimer(nullptr, 0, 15000, timerTick);
}

void handleScriptMessage(WKScriptMessageRef message, WKCompletionListenerRef listener)
{
    WKCompletionListenerComplete(listener, stringUTF8("null").get());
    WKTypeRef body = WKScriptMessageGetBody(message);
    WKFrameInfoRef frame = WKScriptMessageGetFrameInfo(message);
    if (!body || WKGetTypeID(body) != WKStringGetTypeID() || !frame || !WKFrameInfoGetIsMainFrame(frame))
        return;
    BrowserWindow* window = nullptr;
    Tab* tab = nullptr;
    if (!findTab(WKFrameInfoGetPage(frame), window, tab))
        return;
    std::string origin = tabOrigin(tab);
    if (origin.empty())
        return;
    std::string text = util::toUTF8(toWide(static_cast<WKStringRef>(body)));
    json::Value parsed;
    if (text.size() > 64 * 1024 || !json::parse(text, parsed) || !parsed.isObject()) {
        vault::wipe(text);
        return;
    }
    const std::string& op = parsed["op"].asString();

    if (op == "submitted") {
        std::string username = parsed["username"].asString().substr(0, 512);
        std::string password = parsed["password"].asString();
        vault::wipe(text);
        if (password.empty() || password.size() > 512 || g_neverThisSession.count(origin))
            return;
        Pending pending;
        pending.origin = origin;
        pending.username = username;
        pending.at = GetTickCount64();
        if (vault::state() == vault::State::Unlocked) {
            if (vault::neverSave(origin))
                return;
            for (const vault::Login* login : vault::matching(origin)) {
                if (login->origin != origin)
                    continue;
                if (login->username == username || (username.empty() && login->password == password)) {
                    if (login->password == password) {
                        vault::markUsed(login->id); // nothing new
                        return;
                    }
                    pending.existingId = login->id;
                    break;
                }
            }
        }
        erasePending(tab->id);
        pending.password = std::move(password);
        g_pending[tab->id] = std::move(pending);
        offerChanged(tab->id);
        return;
    }

    if (op == "field") {
        vault::wipe(text);
        // Shown after this message's handler returns, so the page isn't kept
        // waiting while the menu is open.
        std::string kind = parsed["kind"].asString();
        double left = parsed["rect"]["left"].asNumber(), top = parsed["rect"]["top"].asNumber(), height = parsed["rect"]["height"].asNumber();
        int tabId = tab->id;
        util::postToMainThread([tabId, origin, kind, left, top, height] {
            BrowserWindow* window = nullptr;
            Tab* tab = nullptr;
            if (!findTab(tabId, window, tab) || window->activeTabId() != tabId || tabOrigin(tab) != origin)
                return;
            std::string rectJSON = "{\"left\":" + std::to_string(left) + ",\"top\":" + std::to_string(top) + ",\"height\":" + std::to_string(height) + "}";
            json::Value rect;
            json::parse(rectJSON, rect);
            showFieldMenu(window, tab, origin, kind, rect);
        });
        return;
    }
    vault::wipe(text);
}

bool offerFor(int tabId, Offer& out)
{
    auto found = g_pending.find(tabId);
    if (found == g_pending.end() || !found->second.shown)
        return false;
    out.site = wide(displaySite(found->second.origin));
    out.username = wide(found->second.username);
    out.update = !found->second.existingId.empty();
    return true;
}

void answerOffer(int tabId, const std::wstring& answer, HWND hwnd)
{
    auto found = g_pending.find(tabId);
    if (found == g_pending.end())
        return;
    Pending& pending = found->second;
    BrowserWindow* window = BrowserWindow::fromHWND(hwnd);
    if (answer == L"allow") {
        if (vault::state() == vault::State::Unlocked) {
            std::wstring message = savePending(pending);
            erasePending(tabId);
            offerChanged(tabId);
            if (window)
                window->showToast(message);
            return;
        }
        // Set up or unlocked on prism://passwords first; saved after that.
        pending.saveAfterUnlock = true;
        pending.shown = false;
        offerChanged(tabId);
        if (window)
            window->openInternalPage(L"passwords");
        return;
    }
    if (answer == L"block") {
        if (vault::state() == vault::State::Unlocked)
            vault::setNeverSave(pending.origin, true);
        else
            g_neverThisSession.insert(pending.origin);
        if (window)
            window->showToast(L"Prism won't offer to save passwords for " + wide(displaySite(pending.origin)));
    }
    erasePending(tabId);
    offerChanged(tabId);
}

void tabNavigated(int tabId, const std::wstring& url)
{
    g_lockedMenuShown.erase(tabId);
    auto found = g_pending.find(tabId);
    if (found == g_pending.end() || found->second.saveAfterUnlock)
        return;
    // Logging in usually loads another page of the same site; keep the offer
    // for that (and one more page, for sign-in redirects).
    Pending& pending = found->second;
    pending.commits++;
    std::string origin = vault::originOf(util::toUTF8(url));
    bool sameSite = !origin.empty() && vault::siteOf(origin) == vault::siteOf(pending.origin);
    if ((!sameSite && pending.commits > 2) || pending.commits > 4) {
        erasePending(tabId);
        offerChanged(tabId);
    }
}

void tabClosed(int tabId)
{
    g_lockedMenuShown.erase(tabId);
    auto found = g_pending.find(tabId);
    if (found != g_pending.end() && !found->second.saveAfterUnlock)
        erasePending(tabId);
}

std::string handleCommand(const std::wstring& command, const std::wstring& argument, HWND hwnd)
{
    json::Value args;
    std::string argumentUTF8 = util::toUTF8(argument);
    json::parse(argumentUTF8.empty() ? std::string("{}") : argumentUTF8, args);
    vault::wipe(argumentUTF8);
    auto master = [&]() { return args["master"].asString(); };

    if (command == L"state")
        return stateJSON();

    if (command == L"create") {
        std::string error;
        std::string password = master();
        bool ok = vault::create(password, args["windowsUnlock"].asBool(true), error);
        vault::wipe(password);
        if (!ok)
            return "{\"error\":" + json::quote(error) + "}";
        g_verifiedAt = GetTickCount64();
        used();
        std::wstring saved = finishWaiting();
        std::string json = stateJSON();
        if (!saved.empty())
            json.insert(json.size() - 1, ",\"message\":" + json::quote(util::toUTF8(saved)));
        return json;
    }

    if (command == L"unlock") {
        std::string error;
        std::string password = master();
        bool ok = vault::unlock(password, error);
        vault::wipe(password);
        if (!ok)
            return "{\"error\":" + json::quote(error) + "}";
        g_verifiedAt = GetTickCount64();
        used();
        std::wstring saved = finishWaiting();
        std::string json = stateJSON();
        if (!saved.empty())
            json.insert(json.size() - 1, ",\"message\":" + json::quote(util::toUTF8(saved)));
        return json;
    }

    if (command == L"verify") {
        std::string password = master();
        bool ok = vault::verify(password);
        vault::wipe(password);
        if (ok)
            g_verifiedAt = GetTickCount64();
        return ok ? stateJSON() : "{\"error\":\"That master password isn't right.\"}";
    }

    if (command == L"lock") {
        vault::lock();
        g_verifiedAt = 0;
        return stateJSON();
    }

    if (command == L"generate")
        return json::quote(vault::randomPassword(static_cast<int>(args["length"].asNumber(20))));

    if (vault::state() != vault::State::Unlocked)
        return "{\"error\":\"Unlock Prism Passwords first.\",\"locked\":true}";
    used();

    if (command == L"list")
        return listJSON();

    if (command == L"reveal" || command == L"copy") {
        const vault::Login* login = vault::find(args["id"].asString());
        if (!login)
            return errorJSON("That login isn't there any more.");
        bool wantsPassword = command == L"reveal" || args["what"].asString() == "password";
        if (wantsPassword && !recentlyVerified())
            return "{\"needMaster\":true}";
        if (command == L"reveal")
            return "{\"password\":" + json::quote(login->password) + "}";
        bool ok = copyText(hwnd, wantsPassword ? login->password : login->username, wantsPassword);
        return ok ? "{\"copied\":true}" : errorJSON("Couldn't copy to the clipboard.");
    }

    if (command == L"save") {
        vault::Login login;
        std::string id = args["id"].asString();
        if (!id.empty()) {
            const vault::Login* existing = vault::find(id);
            if (!existing)
                return errorJSON("That login isn't there any more.");
            login = *existing;
        }
        std::string address = args["url"].asString();
        if (!address.empty() && address.find("://") == std::string::npos)
            address = "https://" + address;
        std::string origin = vault::originOf(address);
        if (origin.empty())
            return errorJSON("Enter the site's address, like example.com.");
        login.origin = origin;
        login.username = args["username"].asString();
        if (args.has("password")) {
            vault::wipe(login.password);
            login.password = args["password"].asString();
        }
        if (login.password.empty())
            return errorJSON("Enter the password.");
        login.note = args["note"].asString();
        vault::put(login);
        vault::wipe(login.password);
        return listJSON();
    }

    if (command == L"delete") {
        vault::remove(args["id"].asString());
        return listJSON();
    }

    if (command == L"allowSaving") {
        vault::setNeverSave(args["origin"].asString(), false);
        return listJSON();
    }

    if (command == L"settings") {
        if (args.has("windowsUnlock")) {
            std::string error;
            if (!vault::setWindowsUnlock(args["windowsUnlock"].asBool(), error))
                return "{\"error\":" + json::quote(error) + "}";
        }
        if (args.has("autoLock")) {
            int minutes = std::max(0, std::min(1440, static_cast<int>(args["autoLock"].asNumber(15))));
            util::writeSetting(L"PasswordsAutoLock", std::to_wstring(minutes));
        }
        return stateJSON();
    }

    if (command == L"changeMaster") {
        std::string error;
        std::string current = args["current"].asString(), replacement = args["replacement"].asString();
        bool ok = vault::changeMaster(current, replacement, error);
        vault::wipe(current);
        vault::wipe(replacement);
        return ok ? stateJSON() : "{\"error\":" + json::quote(error) + "}";
    }

    if (command == L"import") {
        auto file = dialogs::openFile(hwnd, L"Import passwords", L"Password exports (*.csv)", L"*.csv");
        if (!file)
            return "{\"cancelled\":true}";
        std::string text = util::readFile(*file, 64ull * 1024 * 1024);
        std::string result = importCSV(text);
        vault::wipe(text);
        return result;
    }

    if (command == L"export") {
        if (!recentlyVerified())
            return "{\"needMaster\":true}";
        auto file = dialogs::saveFile(hwnd, L"Export passwords", L"Prism passwords.csv", L"CSV file (*.csv)", L"*.csv");
        if (!file)
            return "{\"cancelled\":true}";
        std::string csv = "name,url,username,password,note\r\n";
        for (auto& login : vault::logins())
            csv += csvField(displaySite(login.origin)) + "," + csvField(login.origin) + "," + csvField(login.username) + ","
                + csvField(login.password) + "," + csvField(login.note) + "\r\n";
        bool ok = util::writeFileAtomically(*file, csv);
        vault::wipe(csv);
        return ok ? "{\"exported\":" + std::to_string(vault::logins().size()) + "}" : errorJSON("Couldn't write that file.");
    }

    if (command == L"erase") {
        std::string password = master();
        bool ok = vault::verify(password);
        vault::wipe(password);
        if (!ok)
            return errorJSON("That master password isn't right.");
        vault::erase();
        g_verifiedAt = 0;
        return stateJSON();
    }

    return "null";
}

} // namespace passwords
