// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <windows.h>
#include <functional>
#include <string>

namespace util {

// Prism's icon (the Ocean logo); `smallIcon` is the title bar size.
HICON appIcon(bool smallIcon);

std::string toUTF8(const std::wstring&);
std::wstring fromUTF8(const std::string&);

// Folder that contains Prism.exe (no trailing backslash).
std::wstring exeDirectory();

// %LOCALAPPDATA%\Prism (created if needed). All of Prism's data lives here.
std::wstring dataDirectory();

// The user's Downloads folder.
std::wstring downloadsDirectory();

// Creates a directory and all missing parents.
void ensureDirectory(const std::wstring&);

// Converts C:\path\file.html to file:///C:/path/file.html
std::wstring fileURLFromPath(const std::wstring&);

std::wstring formatWindowsError(DWORD code);

// Percent-encodes text (as UTF-8) for use in a URL query value.
std::wstring urlEncode(const std::wstring&);

// Reverses urlEncode (%XX sequences, as UTF-8; '+' stays '+').
std::wstring urlDecode(const std::wstring&);

// Escapes text for use inside a JSON string literal; output is UTF-8.
std::string jsonString(const std::wstring&);

// Makes a file name safe for the file system and unique in `folder`.
std::wstring uniqueFilePath(const std::wstring& folder, std::wstring name);

// Writes a file atomically (write to a temporary file, then replace).
bool writeFileAtomically(const std::wstring& path, const std::string& contents);
// Reads a whole file. Files larger than maxBytes read as empty.
std::string readFile(const std::wstring& path, unsigned long long maxBytes = 16ull * 1024 * 1024);

// Simple key/value settings stored in %LOCALAPPDATA%\Prism\prism.ini.
std::wstring readSetting(const wchar_t* key, const wchar_t* fallback = L"");
void writeSetting(const wchar_t* key, const std::wstring& value);

// Runs `task` on the main (UI) thread. Safe to call from any thread.
// initMainThread() must be called once at startup, on the main thread.
void initMainThread();
void postToMainThread(std::function<void()> task);

// Encodes bytes as base64 (for data: URLs).
std::string base64(const std::string& bytes);

} // namespace util
