// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <vector>

// Prism Passwords' vault: saved logins, encrypted on disk.
//
// %LOCALAPPDATA%\Prism\passwords.vault is JSON with the encrypted contents.
// A random 256-bit vault key encrypts the logins (AES-256-GCM, Windows CNG).
// The vault key itself is stored twice, encrypted:
//   - with a key made from the master password (PBKDF2-SHA256, 600,000
//     rounds, random salt), so only the master password opens it;
//   - optionally with Windows' DPAPI for the signed-in Windows account
//     ("Stay unlocked while signed in to Windows"), so Prism can open it at
//     start without asking.
// Everything else (sites, user names, notes, the never-save list) is inside
// the encrypted part. Nothing is sent anywhere.
namespace vault {

enum class State { None, Locked, Unlocked }; // None: not set up yet

struct Login {
    std::string id;
    std::string origin;   // "https://example.com" (scheme, host, port)
    std::string username;
    std::string password;
    std::string note;
    double created { 0 };  // ms since 1970
    double updated { 0 };
    double used { 0 };
};

// The vault file (tests use their own; Prism uses the default).
void setPath(const std::wstring& path);

State state();
bool windowsUnlockEnabled();

// Sets up a new vault (State::None only).
bool create(const std::string& master, bool windowsUnlock, std::string& error);
bool unlock(const std::string& master, std::string& error);
// At start: opens the vault with DPAPI if that's turned on.
bool unlockWithWindows();
// Checks the master password without changing anything.
bool verify(const std::string& master);
void lock();
// Unlocked only.
bool changeMaster(const std::string& current, const std::string& replacement, std::string& error);
bool setWindowsUnlock(bool on, std::string& error);
// Deletes the vault file and everything in it.
void erase();

// Unlocked only (empty otherwise).
const std::vector<Login>& logins();
const Login* find(const std::string& id);
// Logins for a page: the same origin first, then other sites of the same
// domain (accounts.example.com for www.example.com).
std::vector<const Login*> matching(const std::string& origin);
// Adds, or replaces the login with the same id. Fills id/created/updated.
// Returns the id ("" when locked).
std::string put(Login login);
bool remove(const std::string& id);
void markUsed(const std::string& id);
bool neverSave(const std::string& origin);
void setNeverSave(const std::string& origin, bool never);
std::vector<std::string> neverSaveList();

// Helpers shared with the browser side.
std::string originOf(const std::string& url); // "" for non-http(s)
std::string siteOf(const std::string& origin); // last two host labels
std::string randomPassword(int length = 20);
double nowMs();
// Overwrites a string's characters before clearing it.
void wipe(std::string& text);

} // namespace vault
