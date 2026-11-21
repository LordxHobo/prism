// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "PasswordVault.h"

#include "Json.h"
#include "Util.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstring>

namespace vault {

namespace {

constexpr int kIterations = 600000;
constexpr size_t kKeySize = 32;
constexpr size_t kNonceSize = 12;
constexpr size_t kTagSize = 16;
const char kKeyPurpose[] = "prism-vault-v1:key";
const char kDataPurpose[] = "prism-vault-v1:data";

std::wstring g_path;
State g_state = State::None;
bool g_loaded = false;

// The file's parts (kept while locked so it can be opened again).
std::string g_salt;          // raw bytes
int g_iterations = kIterations;
std::string g_wrappedKey;    // nonce + ciphertext + tag
std::string g_windowsKey;    // DPAPI blob, or ""
std::string g_data;          // nonce + ciphertext + tag

// While unlocked.
std::string g_key;
std::vector<Login> g_logins;
std::vector<std::string> g_never;

std::wstring path()
{
    if (g_path.empty())
        g_path = util::dataDirectory() + L"\\passwords.vault";
    return g_path;
}

// ---- Bytes and encodings --------------------------------------------------

bool randomBytes(std::string& out, size_t count)
{
    out.assign(count, '\0');
    return BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()), static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::string decodeBase64(const std::string& text)
{
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int value = 0, bits = -8;
    for (char c : text) {
        size_t index = alphabet.find(c);
        if (index == std::string::npos)
            continue;
        value = (value << 6) + static_cast<int>(index);
        bits += 6;
        if (bits >= 0) {
            out += static_cast<char>((value >> bits) & 0xff);
            bits -= 8;
        }
    }
    return out;
}

// ---- Crypto (Windows CNG) ---------------------------------------------------

bool deriveKey(const std::string& master, const std::string& salt, int iterations, std::string& key)
{
    BCRYPT_ALG_HANDLE prf = nullptr;
    if (BCryptOpenAlgorithmProvider(&prf, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
        return false;
    key.assign(kKeySize, '\0');
    NTSTATUS status = BCryptDeriveKeyPBKDF2(prf,
        reinterpret_cast<PUCHAR>(const_cast<char*>(master.data())), static_cast<ULONG>(master.size()),
        reinterpret_cast<PUCHAR>(const_cast<char*>(salt.data())), static_cast<ULONG>(salt.size()),
        static_cast<ULONGLONG>(iterations), reinterpret_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0);
    BCryptCloseAlgorithmProvider(prf, 0);
    return status == 0;
}

// AES-256-GCM. Output: nonce (12) + ciphertext + tag (16). `purpose` is
// authenticated data, so a blob can't be swapped for another kind.
bool seal(const std::string& key, const std::string& plain, const char* purpose, std::string& out)
{
    BCRYPT_ALG_HANDLE aes = nullptr;
    if (BCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
        return false;
    bool ok = false;
    BCRYPT_KEY_HANDLE handle = nullptr;
    std::string nonce;
    if (BCryptSetProperty(aes, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)), sizeof(BCRYPT_CHAIN_MODE_GCM), 0) == 0
        && BCryptGenerateSymmetricKey(aes, &handle, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())), static_cast<ULONG>(key.size()), 0) == 0
        && randomBytes(nonce, kNonceSize)) {
        std::string tag(kTagSize, '\0');
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = reinterpret_cast<PUCHAR>(nonce.data());
        info.cbNonce = static_cast<ULONG>(nonce.size());
        info.pbAuthData = reinterpret_cast<PUCHAR>(const_cast<char*>(purpose));
        info.cbAuthData = static_cast<ULONG>(strlen(purpose));
        info.pbTag = reinterpret_cast<PUCHAR>(tag.data());
        info.cbTag = static_cast<ULONG>(tag.size());
        std::string cipher(plain.size(), '\0');
        ULONG written = 0;
        if (BCryptEncrypt(handle, reinterpret_cast<PUCHAR>(const_cast<char*>(plain.data())), static_cast<ULONG>(plain.size()), &info, nullptr, 0,
                reinterpret_cast<PUCHAR>(cipher.data()), static_cast<ULONG>(cipher.size()), &written, 0) == 0) {
            out = nonce + cipher.substr(0, written) + tag;
            ok = true;
        }
    }
    if (handle)
        BCryptDestroyKey(handle);
    BCryptCloseAlgorithmProvider(aes, 0);
    return ok;
}

bool open(const std::string& key, const std::string& blob, const char* purpose, std::string& plain)
{
    if (blob.size() < kNonceSize + kTagSize)
        return false;
    BCRYPT_ALG_HANDLE aes = nullptr;
    if (BCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
        return false;
    bool ok = false;
    BCRYPT_KEY_HANDLE handle = nullptr;
    if (BCryptSetProperty(aes, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)), sizeof(BCRYPT_CHAIN_MODE_GCM), 0) == 0
        && BCryptGenerateSymmetricKey(aes, &handle, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())), static_cast<ULONG>(key.size()), 0) == 0) {
        std::string nonce = blob.substr(0, kNonceSize);
        std::string tag = blob.substr(blob.size() - kTagSize);
        std::string cipher = blob.substr(kNonceSize, blob.size() - kNonceSize - kTagSize);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = reinterpret_cast<PUCHAR>(nonce.data());
        info.cbNonce = static_cast<ULONG>(nonce.size());
        info.pbAuthData = reinterpret_cast<PUCHAR>(const_cast<char*>(purpose));
        info.cbAuthData = static_cast<ULONG>(strlen(purpose));
        info.pbTag = reinterpret_cast<PUCHAR>(tag.data());
        info.cbTag = static_cast<ULONG>(tag.size());
        std::string out(cipher.size(), '\0');
        ULONG written = 0;
        if (BCryptDecrypt(handle, reinterpret_cast<PUCHAR>(cipher.data()), static_cast<ULONG>(cipher.size()), &info, nullptr, 0,
                reinterpret_cast<PUCHAR>(out.data()), static_cast<ULONG>(out.size()), &written, 0) == 0) {
            out.resize(written);
            plain = std::move(out);
            ok = true;
        } else
            wipe(out);
    }
    if (handle)
        BCryptDestroyKey(handle);
    BCryptCloseAlgorithmProvider(aes, 0);
    return ok;
}

bool protectForWindows(const std::string& key, std::string& blob)
{
    DATA_BLOB in = { static_cast<DWORD>(key.size()), reinterpret_cast<BYTE*>(const_cast<char*>(key.data())) };
    DATA_BLOB out = { };
    if (!CryptProtectData(&in, L"Prism Passwords", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    blob.assign(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool unprotectForWindows(const std::string& blob, std::string& key)
{
    DATA_BLOB in = { static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE*>(const_cast<char*>(blob.data())) };
    DATA_BLOB out = { };
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    key.assign(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return key.size() == kKeySize;
}

// ---- File -----------------------------------------------------------------

void load()
{
    if (g_loaded)
        return;
    g_loaded = true;
    std::string text = util::readFile(path(), 256ull * 1024 * 1024);
    json::Value root;
    if (text.empty() || !json::parse(text, root) || !root.isObject() || root["version"].asNumber() != 1) {
        g_state = State::None;
        return;
    }
    g_salt = decodeBase64(root["kdf"]["salt"].asString());
    g_iterations = static_cast<int>(root["kdf"]["iterations"].asNumber(kIterations));
    g_wrappedKey = decodeBase64(root["key"].asString());
    g_windowsKey = decodeBase64(root["windowsKey"].asString());
    g_data = decodeBase64(root["data"].asString());
    g_state = g_salt.empty() || g_wrappedKey.empty() || g_data.empty() ? State::None : State::Locked;
}

bool writeFile()
{
    std::string text = "{\"version\":1,\"kdf\":{\"name\":\"PBKDF2-SHA256\",\"iterations\":" + std::to_string(g_iterations)
        + ",\"salt\":\"" + util::base64(g_salt) + "\"},\"key\":\"" + util::base64(g_wrappedKey) + "\"";
    if (!g_windowsKey.empty())
        text += ",\"windowsKey\":\"" + util::base64(g_windowsKey) + "\"";
    text += ",\"data\":\"" + util::base64(g_data) + "\"}\n";
    return util::writeFileAtomically(path(), text);
}

std::string number(double value)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.0f", value);
    return buffer;
}

std::string serializeContents()
{
    std::string out = "{\"logins\":[";
    for (size_t i = 0; i < g_logins.size(); i++) {
        const Login& login = g_logins[i];
        if (i)
            out += ',';
        out += "{\"id\":" + json::quote(login.id) + ",\"origin\":" + json::quote(login.origin) + ",\"username\":" + json::quote(login.username)
            + ",\"password\":" + json::quote(login.password) + ",\"note\":" + json::quote(login.note) + ",\"created\":" + number(login.created)
            + ",\"updated\":" + number(login.updated) + ",\"used\":" + number(login.used) + "}";
    }
    out += "],\"never\":[";
    for (size_t i = 0; i < g_never.size(); i++)
        out += (i ? "," : "") + json::quote(g_never[i]);
    return out + "]}";
}

bool readContents(const std::string& plain)
{
    json::Value root;
    if (!json::parse(plain, root) || !root.isObject())
        return false;
    g_logins.clear();
    g_never.clear();
    for (auto& item : root["logins"].items()) {
        Login login;
        login.id = item["id"].asString();
        login.origin = item["origin"].asString();
        login.username = item["username"].asString();
        login.password = item["password"].asString();
        login.note = item["note"].asString();
        login.created = item["created"].asNumber();
        login.updated = item["updated"].asNumber();
        login.used = item["used"].asNumber();
        if (!login.id.empty())
            g_logins.push_back(std::move(login));
    }
    for (auto& item : root["never"].items())
        if (item.isString())
            g_never.push_back(item.asString());
    return true;
}

bool save()
{
    if (g_state != State::Unlocked)
        return false;
    std::string plain = serializeContents();
    std::string sealed;
    bool ok = seal(g_key, plain, kDataPurpose, sealed);
    wipe(plain);
    if (!ok)
        return false;
    g_data = std::move(sealed);
    return writeFile();
}

// Opens the vault with a vault key from either route.
bool openWithKey(std::string key)
{
    std::string plain;
    if (!open(key, g_data, kDataPurpose, plain) || !readContents(plain)) {
        wipe(key);
        wipe(plain);
        return false;
    }
    wipe(plain);
    g_key = std::move(key);
    g_state = State::Unlocked;
    return true;
}

bool unwrapWithMaster(const std::string& master, std::string& key)
{
    std::string derived;
    if (!deriveKey(master, g_salt, g_iterations, derived))
        return false;
    bool ok = open(derived, g_wrappedKey, kKeyPurpose, key);
    wipe(derived);
    return ok;
}

bool wrapWithMaster(const std::string& master, std::string& salt, std::string& wrapped)
{
    std::string derived;
    if (!randomBytes(salt, 16) || !deriveKey(master, salt, kIterations, derived))
        return false;
    bool ok = seal(derived, g_key, kKeyPurpose, wrapped);
    wipe(derived);
    return ok;
}

std::string lowerHost(const std::string& origin)
{
    size_t scheme = origin.find("://");
    std::string host = scheme == std::string::npos ? origin : origin.substr(scheme + 3);
    size_t port = host.rfind(':');
    if (port != std::string::npos && host.find(']') == std::string::npos)
        host.erase(port);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return host;
}

const std::vector<Login> kNoLogins;

} // namespace

void wipe(std::string& text)
{
    if (!text.empty())
        SecureZeroMemory(text.data(), text.size());
    text.clear();
}

double nowMs()
{
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    ULONGLONG ticks = (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    return static_cast<double>((ticks - 116444736000000000ull) / 10000);
}

void setPath(const std::wstring& file)
{
    lock();
    g_path = file;
    g_loaded = false;
    g_state = State::None;
}

State state()
{
    load();
    return g_state;
}

bool windowsUnlockEnabled()
{
    load();
    return !g_windowsKey.empty();
}

bool create(const std::string& master, bool windowsUnlock, std::string& error)
{
    load();
    if (g_state != State::None) {
        error = "Prism Passwords is already set up.";
        return false;
    }
    if (master.size() < 8) {
        error = "Use at least 8 characters for the master password.";
        return false;
    }
    if (!randomBytes(g_key, kKeySize) || !wrapWithMaster(master, g_salt, g_wrappedKey)) {
        wipe(g_key);
        error = "Windows couldn't create the encryption keys.";
        return false;
    }
    g_iterations = kIterations;
    g_windowsKey.clear();
    if (windowsUnlock)
        protectForWindows(g_key, g_windowsKey);
    g_logins.clear();
    g_never.clear();
    g_state = State::Unlocked;
    if (!save()) {
        lock();
        g_state = State::None;
        error = "Couldn't save the password vault.";
        return false;
    }
    return true;
}

bool unlock(const std::string& master, std::string& error)
{
    load();
    if (g_state == State::Unlocked)
        return true;
    if (g_state == State::None) {
        error = "Prism Passwords isn't set up yet.";
        return false;
    }
    std::string key;
    if (!unwrapWithMaster(master, key)) {
        error = "That master password isn't right.";
        return false;
    }
    if (!openWithKey(std::move(key))) {
        error = "The password vault couldn't be read. It may be damaged.";
        return false;
    }
    return true;
}

bool unlockWithWindows()
{
    load();
    if (g_state != State::Locked || g_windowsKey.empty())
        return g_state == State::Unlocked;
    std::string key;
    if (!unprotectForWindows(g_windowsKey, key))
        return false;
    return openWithKey(std::move(key));
}

bool verify(const std::string& master)
{
    load();
    if (g_state == State::None)
        return false;
    std::string key;
    bool ok = unwrapWithMaster(master, key);
    wipe(key);
    return ok;
}

void lock()
{
    wipe(g_key);
    for (auto& login : g_logins) {
        wipe(login.password);
        wipe(login.username);
        wipe(login.note);
    }
    g_logins.clear();
    g_never.clear();
    if (g_state == State::Unlocked)
        g_state = State::Locked;
}

bool changeMaster(const std::string& current, const std::string& replacement, std::string& error)
{
    if (state() != State::Unlocked) {
        error = "Unlock Prism Passwords first.";
        return false;
    }
    if (!verify(current)) {
        error = "The current master password isn't right.";
        return false;
    }
    if (replacement.size() < 8) {
        error = "Use at least 8 characters for the master password.";
        return false;
    }
    std::string salt, wrapped;
    if (!wrapWithMaster(replacement, salt, wrapped)) {
        error = "Windows couldn't create the encryption keys.";
        return false;
    }
    g_salt = salt;
    g_wrappedKey = wrapped;
    g_iterations = kIterations;
    if (!save()) {
        error = "Couldn't save the password vault.";
        return false;
    }
    return true;
}

bool setWindowsUnlock(bool on, std::string& error)
{
    if (state() != State::Unlocked) {
        error = "Unlock Prism Passwords first.";
        return false;
    }
    if (on) {
        std::string blob;
        if (!protectForWindows(g_key, blob)) {
            error = "Windows couldn't protect the key for your account.";
            return false;
        }
        g_windowsKey = blob;
    } else
        g_windowsKey.clear();
    return writeFile();
}

void erase()
{
    lock();
    DeleteFileW(path().c_str());
    g_salt.clear();
    g_wrappedKey.clear();
    g_windowsKey.clear();
    g_data.clear();
    g_state = State::None;
    g_loaded = true;
}

const std::vector<Login>& logins()
{
    return state() == State::Unlocked ? g_logins : kNoLogins;
}

const Login* find(const std::string& id)
{
    for (auto& login : logins())
        if (login.id == id)
            return &login;
    return nullptr;
}

std::vector<const Login*> matching(const std::string& origin)
{
    std::vector<const Login*> exact, related;
    std::string site = siteOf(origin);
    bool secure = origin.rfind("https://", 0) == 0;
    for (auto& login : logins()) {
        if (login.origin == origin)
            exact.push_back(&login);
        else if (!site.empty() && siteOf(login.origin) == site && (secure || login.origin.rfind("http://", 0) == 0))
            related.push_back(&login); // an https login is never offered to an http page
    }
    auto recent = [](const Login* a, const Login* b) { return std::max(a->used, a->updated) > std::max(b->used, b->updated); };
    std::sort(exact.begin(), exact.end(), recent);
    std::sort(related.begin(), related.end(), recent);
    exact.insert(exact.end(), related.begin(), related.end());
    return exact;
}

std::string put(Login login)
{
    if (state() != State::Unlocked)
        return { };
    double now = nowMs();
    if (login.id.empty()) {
        std::string bytes;
        randomBytes(bytes, 8);
        static const char hex[] = "0123456789abcdef";
        for (unsigned char c : bytes) {
            login.id += hex[c >> 4];
            login.id += hex[c & 15];
        }
        login.created = now;
    }
    login.updated = now;
    std::string id = login.id;
    auto existing = std::find_if(g_logins.begin(), g_logins.end(), [&](const Login& item) { return item.id == id; });
    if (existing != g_logins.end()) {
        login.created = existing->created ? existing->created : now;
        login.used = std::max(login.used, existing->used);
        wipe(existing->password);
        *existing = std::move(login);
    } else
        g_logins.push_back(std::move(login));
    save();
    return id;
}

bool remove(const std::string& id)
{
    if (state() != State::Unlocked)
        return false;
    auto found = std::find_if(g_logins.begin(), g_logins.end(), [&](const Login& item) { return item.id == id; });
    if (found == g_logins.end())
        return false;
    wipe(found->password);
    g_logins.erase(found);
    return save();
}

void markUsed(const std::string& id)
{
    for (auto& login : g_logins) {
        if (login.id == id) {
            login.used = nowMs();
            save();
            return;
        }
    }
}

bool neverSave(const std::string& origin)
{
    return std::find(g_never.begin(), g_never.end(), origin) != g_never.end();
}

void setNeverSave(const std::string& origin, bool never)
{
    if (state() != State::Unlocked)
        return;
    auto found = std::find(g_never.begin(), g_never.end(), origin);
    if (never && found == g_never.end())
        g_never.push_back(origin);
    else if (!never && found != g_never.end())
        g_never.erase(found);
    else
        return;
    save();
}

std::vector<std::string> neverSaveList()
{
    return state() == State::Unlocked ? g_never : std::vector<std::string>();
}

std::string originOf(const std::string& url)
{
    std::string lower = url.substr(0, 8);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    size_t start;
    if (lower.rfind("https://", 0) == 0)
        start = 8;
    else if (lower.rfind("http://", 0) == 0)
        start = 7;
    else
        return { };
    size_t end = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    size_t at = authority.rfind('@');
    if (at != std::string::npos)
        authority.erase(0, at + 1);
    std::transform(authority.begin(), authority.end(), authority.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    // Default ports aren't part of the origin.
    std::string scheme = start == 8 ? "https" : "http";
    if ((scheme == "https" && authority.size() > 4 && authority.compare(authority.size() - 4, 4, ":443") == 0)
        || (scheme == "http" && authority.size() > 3 && authority.compare(authority.size() - 3, 3, ":80") == 0))
        authority.erase(authority.rfind(':'));
    if (authority.empty())
        return { };
    return scheme + "://" + authority;
}

std::string siteOf(const std::string& origin)
{
    std::string host = lowerHost(origin);
    if (host.empty() || host.find_first_not_of("0123456789.") == std::string::npos || host[0] == '[')
        return host; // IP addresses stand alone
    size_t last = host.rfind('.');
    if (last == std::string::npos || last == 0)
        return host;
    size_t second = host.rfind('.', last - 1);
    std::string site = second == std::string::npos ? host : host.substr(second + 1);
    // co.uk, com.au and the like: keep one more label.
    std::string label = second == std::string::npos ? "" : host.substr(second + 1, last - second - 1);
    if (second != std::string::npos && (label == "co" || label == "com" || label == "net" || label == "org" || label == "gov" || label == "ac" || label == "edu")
        && host.size() - last - 1 == 2) {
        size_t third = host.rfind('.', second - 1);
        site = third == std::string::npos || second == 0 ? host : host.substr(third + 1);
    }
    return site;
}

std::string randomPassword(int length)
{
    // No look-alikes (0/O, 1/l/I); at least one of each kind.
    static const std::string lower = "abcdefghijkmnopqrstuvwxyz";
    static const std::string upper = "ABCDEFGHJKLMNPQRSTUVWXYZ";
    static const std::string digits = "23456789";
    static const std::string symbols = "-_.!@#$%*+=?";
    const std::string all = lower + upper + digits + symbols;
    length = std::max(12, std::min(64, length));
    for (;;) {
        std::string bytes;
        if (!randomBytes(bytes, static_cast<size_t>(length) * 2))
            return { };
        std::string out;
        for (size_t i = 0; i + 1 < bytes.size() && out.size() < static_cast<size_t>(length); i += 2) {
            unsigned value = (static_cast<unsigned char>(bytes[i]) << 8) | static_cast<unsigned char>(bytes[i + 1]);
            unsigned limit = 65536 - 65536 % all.size(); // no modulo bias
            if (value < limit)
                out += all[value % all.size()];
        }
        if (out.size() < static_cast<size_t>(length))
            continue;
        auto has = [&](const std::string& set) { return out.find_first_of(set) != std::string::npos; };
        if (has(lower) && has(upper) && has(digits) && has(symbols))
            return out;
    }
}

} // namespace vault
