// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Translate.h"

#include "BrowserWindow.h"
#include "Engine.h"
#include "ExtensionServer.h"
#include "Extensions.h"
#include "Http.h"
#include "Json.h"
#include "Perf.h"
#include "Tabs.h"
#include "Util.h"

#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

using namespace wk;
namespace fs = std::filesystem;

namespace translate {

namespace {

// Mozilla's Remote Settings, where Firefox gets the same files.
const wchar_t* kServer = L"https://firefox.settings.services.mozilla.com/v1/";
const char* kDefaultAttachments = "https://firefox-settings-attachments.cdn.mozilla.net/";
// The versions that work with the Bergamot JavaScript in ui/translate
// (Firefox's bergamot-translator.js v0.6.0): engine 4.x with models 3.x
// (Firefox now), or engine 3.x with models up to 2.x (before).
constexpr int kEngineMajor = 4;
constexpr int kModelMajor = 3;
constexpr ULONGLONG kCatalogMaxAge = 24ull * 60 * 60 * 1000;
constexpr ULONGLONG kCallTimeout = 4ull * 60 * 1000;
constexpr ULONGLONG kTranslatorIdle = 3ull * 60 * 1000; // then its memory is freed
constexpr ULONGLONG kEngineIdle = 10ull * 60 * 1000;    // then the hidden page closes
constexpr const char* kSiteId = "prism-translate";

// ---- Languages ------------------------------------------------------------------------

const std::map<std::string, const wchar_t*>& languageNames()
{
    static const std::map<std::string, const wchar_t*> names = {
        { "af", L"Afrikaans" }, { "am", L"Amharic" }, { "ar", L"Arabic" }, { "az", L"Azerbaijani" }, { "be", L"Belarusian" },
        { "bg", L"Bulgarian" }, { "bn", L"Bengali" }, { "bs", L"Bosnian" }, { "ca", L"Catalan" }, { "cs", L"Czech" },
        { "cy", L"Welsh" }, { "da", L"Danish" }, { "de", L"German" }, { "el", L"Greek" }, { "en", L"English" },
        { "eo", L"Esperanto" }, { "es", L"Spanish" }, { "et", L"Estonian" }, { "eu", L"Basque" }, { "fa", L"Persian" },
        { "fi", L"Finnish" }, { "fil", L"Filipino" }, { "fr", L"French" }, { "ga", L"Irish" }, { "gl", L"Galician" },
        { "gu", L"Gujarati" }, { "he", L"Hebrew" }, { "hi", L"Hindi" }, { "hr", L"Croatian" }, { "hu", L"Hungarian" },
        { "hy", L"Armenian" }, { "id", L"Indonesian" }, { "is", L"Icelandic" }, { "it", L"Italian" }, { "ja", L"Japanese" },
        { "ka", L"Georgian" }, { "kk", L"Kazakh" }, { "km", L"Khmer" }, { "kn", L"Kannada" }, { "ko", L"Korean" },
        { "lt", L"Lithuanian" }, { "lv", L"Latvian" }, { "mk", L"Macedonian" }, { "ml", L"Malayalam" }, { "mn", L"Mongolian" },
        { "mr", L"Marathi" }, { "ms", L"Malay" }, { "mt", L"Maltese" }, { "my", L"Burmese" }, { "nb", L"Norwegian Bokmål" },
        { "ne", L"Nepali" }, { "nl", L"Dutch" }, { "nn", L"Norwegian Nynorsk" }, { "pa", L"Punjabi" }, { "pl", L"Polish" },
        { "pt", L"Portuguese" }, { "ro", L"Romanian" }, { "ru", L"Russian" }, { "si", L"Sinhala" }, { "sk", L"Slovak" },
        { "sl", L"Slovenian" }, { "sq", L"Albanian" }, { "sr", L"Serbian" }, { "sv", L"Swedish" }, { "sw", L"Swahili" },
        { "ta", L"Tamil" }, { "te", L"Telugu" }, { "th", L"Thai" }, { "tr", L"Turkish" }, { "uk", L"Ukrainian" },
        { "ur", L"Urdu" }, { "uz", L"Uzbek" }, { "vi", L"Vietnamese" }, { "zh", L"Chinese (Simplified)" },
        { "zh-Hans", L"Chinese (Simplified)" }, { "zh-Hant", L"Chinese (Traditional)" },
    };
    return names;
}

std::wstring languageName(const std::string& code)
{
    auto found = languageNames().find(code);
    return found != languageNames().end() ? found->second : util::fromUTF8(code);
}

std::string lowerASCII(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return text;
}

// "nl-BE" -> "nl"; CLD2's old codes -> today's; Chinese keeps its script.
std::string normalizeLanguage(std::string code)
{
    std::replace(code.begin(), code.end(), '_', '-');
    std::string lower = lowerASCII(code);
    if (lower == "zh-hant" || lower == "zh-tw" || lower == "zh-hk" || lower == "zh-mo")
        return "zh-Hant";
    std::string primary = lower.substr(0, lower.find('-'));
    if (primary == "iw") return "he";
    if (primary == "in") return "id";
    if (primary == "ji") return "yi";
    if (primary == "jw") return "jv";
    if (primary == "tl") return "fil";
    if (primary == "no") return "nb";
    return primary;
}

// ---- Files and versions -----------------------------------------------------------------

struct File {
    std::string type; // model, lex, vocab, srcvocab, trgvocab, wasm
    std::string name;
    std::string hash; // SHA-256 of the file as downloaded
    std::string location;
    uint64_t size { 0 };
};

struct Pair {
    std::string from, to, version;
    std::map<std::string, File> files;
};

struct Catalog {
    bool ready { false };
    std::string base = kDefaultAttachments;
    File engine;
    std::map<std::string, Pair> pairs; // "fr-en"
};

Catalog g_catalog;
bool g_catalogLoading = false;
std::string g_catalogError;
std::vector<std::function<void()>> g_catalogWaiting;
ULONGLONG g_catalogFresh = 0; // when the list was last known to be current

std::vector<int> versionParts(const std::string& version)
{
    std::vector<int> parts;
    int value = 0;
    bool digits = false;
    for (char c : version) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            digits = true;
        } else if (c == '.') {
            parts.push_back(value);
            value = 0;
            digits = false;
        } else {
            break; // "1.0a1": pre-releases count as the version they lead to
        }
    }
    if (digits || parts.empty())
        parts.push_back(value);
    return parts;
}

int compareVersions(const std::string& a, const std::string& b)
{
    auto x = versionParts(a), y = versionParts(b);
    size_t count = std::max(x.size(), y.size());
    for (size_t i = 0; i < count; i++) {
        int left = i < x.size() ? x[i] : 0, right = i < y.size() ? y[i] : 0;
        if (left != right)
            return left < right ? -1 : 1;
    }
    return 0;
}

int majorOf(const std::string& version)
{
    return versionParts(version).front();
}

// A record's filter_expression, for Prism as a desktop release Firefox
// would read it. Firefox's lists use env.<name> == / != '<value>' joined by
// && and ||; version checks pass (Prism uses the newest files); anything
// else doesn't match, so an unknown rule never picks a file.
bool filterMatches(const std::string& expression)
{
    if (expression.find_first_not_of(" \t") == std::string::npos)
        return true;
    auto trim = [](std::string text) {
        size_t start = text.find_first_not_of(" \t()"), end = text.find_last_not_of(" \t()");
        return start == std::string::npos ? std::string() : text.substr(start, end - start + 1);
    };
    auto split = [](const std::string& text, const std::string& by) {
        std::vector<std::string> parts;
        size_t start = 0;
        for (;;) {
            size_t at = text.find(by, start);
            parts.push_back(text.substr(start, at == std::string::npos ? std::string::npos : at - start));
            if (at == std::string::npos)
                return parts;
            start = at + by.size();
        }
    };
    const std::map<std::string, std::string> env = { { "appinfo.OS", "WINNT" }, { "channel", "release" } };
    for (auto& clause : split(expression, "||")) {
        bool all = true;
        for (auto& rawAtom : split(clause, "&&")) {
            std::string atom = trim(rawAtom);
            if (atom.find("versionCompare") != std::string::npos)
                continue;
            bool equals = atom.find("==") != std::string::npos;
            size_t op = atom.find(equals ? "==" : "!=");
            if (atom.rfind("env.", 0) != 0 || op == std::string::npos) {
                all = false;
                break;
            }
            std::string name = trim(atom.substr(4, op - 4));
            std::string value = trim(atom.substr(op + 2));
            if (value.size() < 2 || (value.front() != '\'' && value.front() != '"') || value.back() != value.front()) {
                all = false;
                break;
            }
            value = value.substr(1, value.size() - 2);
            auto found = env.find(name);
            if (found == env.end() || (found->second == value) != equals) {
                all = false;
                break;
            }
        }
        if (all)
            return true;
    }
    return false;
}

bool forUs(const json::Value& record)
{
    return filterMatches(record["filter_expression"].asString()) && record["variant"].asString().empty();
}

File fileOf(const json::Value& record, const std::string& type)
{
    File file;
    file.type = type;
    file.name = record["name"].asString();
    const json::Value& attachment = record["attachment"];
    file.hash = lowerASCII(attachment["hash"].asString());
    file.location = attachment["location"].asString();
    file.size = static_cast<uint64_t>(attachment["size"].asNumber());
    return file;
}

bool validFile(const File& file)
{
    return file.hash.size() == 64 && file.hash.find_first_not_of("0123456789abcdef") == std::string::npos && !file.location.empty()
        && file.location.find("..") == std::string::npos && file.size > 0;
}

// One engine version and the model versions made for it.
bool buildCatalogFor(const std::string& base, const json::Value& engineList, const json::Value& modelList, int engineMajor, int minModelMajor, int maxModelMajor, Catalog& out)
{
    Catalog catalog;
    if (!base.empty())
        catalog.base = base.back() == '/' ? base : base + "/";
    std::string best;
    for (auto& record : engineList.items()) {
        std::string version = record["version"].asString();
        if (record["name"].asString() != "bergamot-translator" || majorOf(version) != engineMajor || !forUs(record))
            continue;
        if (!best.empty() && compareVersions(version, best) <= 0)
            continue;
        File file = fileOf(record, "wasm");
        if (!validFile(file))
            continue;
        catalog.engine = file;
        best = version;
    }
    if (best.empty())
        return false;

    // Per pair, the newest version of its "model" file, and the files of
    // exactly that version (what Firefox does).
    struct Candidate { std::string version; json::Value record; };
    std::map<std::string, std::vector<const json::Value*>> byPair;
    for (auto& record : modelList.items()) {
        std::string from = record.has("sourceLanguage") ? record["sourceLanguage"].asString() : record["fromLang"].asString();
        std::string to = record.has("targetLanguage") ? record["targetLanguage"].asString() : record["toLang"].asString();
        int major = majorOf(record["version"].asString());
        if (from.empty() || to.empty() || major < minModelMajor || major > maxModelMajor || !forUs(record))
            continue;
        byPair[from + "-" + to].push_back(&record);
    }
    for (auto& [key, records] : byPair) {
        std::string version;
        for (auto* record : records) {
            if (record->operator[]("fileType").asString() == "model" && (version.empty() || compareVersions(record->operator[]("version").asString(), version) > 0))
                version = record->operator[]("version").asString();
        }
        if (version.empty())
            continue;
        Pair pair;
        pair.version = version;
        for (auto* record : records) {
            const json::Value& r = *record;
            std::string type = r["fileType"].asString();
            // The lexical shortlist makes it faster but a little less
            // accurate; Prism goes for accuracy.
            if (compareVersions(r["version"].asString(), version) != 0 || type == "lex" || pair.files.count(type))
                continue;
            if (type != "model" && type != "vocab" && type != "srcvocab" && type != "trgvocab")
                continue;
            File file = fileOf(r, type);
            if (!validFile(file))
                continue;
            pair.from = r.has("sourceLanguage") ? r["sourceLanguage"].asString() : r["fromLang"].asString();
            pair.to = r.has("targetLanguage") ? r["targetLanguage"].asString() : r["toLang"].asString();
            pair.files[type] = file;
        }
        bool vocab = pair.files.count("vocab") || (pair.files.count("srcvocab") && pair.files.count("trgvocab"));
        if (pair.files.count("model") && vocab)
            catalog.pairs[key] = std::move(pair);
    }
    if (catalog.pairs.empty())
        return false;
    catalog.ready = true;
    out = std::move(catalog);
    return true;
}

bool buildCatalog(const std::string& base, const std::string& engineRecords, const std::string& modelRecords, Catalog& out)
{
    json::Value engines, models;
    if (!json::parse(engineRecords, engines) || !json::parse(modelRecords, models))
        return false;
    const json::Value& engineList = engines.isObject() ? engines["data"] : engines;
    const json::Value& modelList = models.isObject() ? models["data"] : models;
    // What Firefox uses now (engine 4.x with zstd models 3.x); otherwise the
    // same engine's earlier release (3.x) with models 1.x and 2.x.
    return buildCatalogFor(base, engineList, modelList, kEngineMajor, kModelMajor, kModelMajor, out)
        || buildCatalogFor(base, engineList, modelList, kEngineMajor - 1, 1, kModelMajor - 1, out);
}

std::wstring dataDir()
{
    return util::dataDirectory() + L"\\translations";
}

std::wstring filesDir()
{
    return dataDir() + L"\\files";
}

std::wstring pathOf(const File& file)
{
    return filesDir() + L"\\" + util::fromUTF8(file.hash) + L".zst";
}

bool have(const File& file)
{
    std::error_code error;
    return fs::exists(pathOf(file), error) && fs::file_size(pathOf(file), error) == file.size;
}

std::string sha256Hex(const std::string& data)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return { };
    unsigned char hash[32] = { };
    NTSTATUS status = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), hash, sizeof(hash));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0)
        return { };
    static const char hex[] = "0123456789abcdef";
    std::string text;
    for (unsigned char byte : hash) {
        text += hex[byte >> 4];
        text += hex[byte & 15];
    }
    return text;
}

// ---- The list of files (Mozilla's records) ----------------------------------------------

// All records of a collection ({"data": [...]}, following Next-Page).
bool fetchRecords(const std::wstring& collection, std::string& out, std::wstring& error)
{
    std::wstring next = std::wstring(kServer) + L"buckets/main/collections/" + collection + L"/records";
    std::string items;
    for (int page = 0; page < 20 && !next.empty(); page++) {
        http::Request request;
        request.url = next;
        http::Response response;
        if (!http::request(request, response, error, 32 * 1024 * 1024))
            return false;
        if (response.status != 200) {
            error = L"Mozilla's server answered " + std::to_wstring(response.status) + L".";
            return false;
        }
        json::Value parsed;
        if (!json::parse(response.body, parsed) || !parsed["data"].isArray()) {
            error = L"Mozilla's server sent something unexpected.";
            return false;
        }
        const json::Value& data = parsed["data"];
        std::string inner = response.body.substr(data.start(), data.end() - data.start());
        // Strip the brackets, keep the records as they are.
        size_t open = inner.find('['), close = inner.rfind(']');
        if (open != std::string::npos && close != std::string::npos && close > open) {
            std::string body = inner.substr(open + 1, close - open - 1);
            if (body.find_first_not_of(" \t\r\n") != std::string::npos)
                items += (items.empty() ? "" : ",") + body;
        }
        next.clear();
        for (auto& [name, value] : response.headers) {
            std::wstring lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            if (lower == L"next-page" && value.rfind(kServer, 0) == 0)
                next = value;
        }
    }
    out = "{\"data\":[" + items + "]}";
    return true;
}

void finishCatalogLoad()
{
    g_catalogLoading = false;
    auto waiting = std::move(g_catalogWaiting);
    g_catalogWaiting.clear();
    for (auto& callback : waiting)
        callback();
}

bool loadCachedCatalog(ULONGLONG* ageMs)
{
    std::wstring dir = dataDir();
    std::string base = util::readFile(dir + L"\\attachments.txt", 4096);
    std::string engines = util::readFile(dir + L"\\engine-records.json");
    std::string models = util::readFile(dir + L"\\model-records.json");
    if (engines.empty() || models.empty())
        return false;
    Catalog catalog;
    if (!buildCatalog(base, engines, models, catalog))
        return false;
    g_catalog = std::move(catalog);
    if (ageMs) {
        std::error_code error;
        auto written = fs::last_write_time(dir + L"\\model-records.json", error);
        auto now = fs::file_time_type::clock::now();
        *ageMs = error ? kCatalogMaxAge : static_cast<ULONGLONG>(std::chrono::duration_cast<std::chrono::milliseconds>(now - written).count());
    }
    return true;
}

void refreshCatalog()
{
    if (g_catalogLoading)
        return;
    g_catalogLoading = true;
    std::thread([] {
        std::wstring error;
        std::string base, engines, models;
        http::Request request;
        request.url = kServer;
        http::Response response;
        if (http::request(request, response, error, 1024 * 1024) && response.status == 200) {
            json::Value root;
            if (json::parse(response.body, root))
                base = root["capabilities"]["attachments"]["base_url"].asString();
        }
        error.clear();
        bool ok = fetchRecords(L"translations-wasm", engines, error) && fetchRecords(L"translations-models", models, error);
        util::postToMainThread([ok, error, base, engines, models] {
            Catalog catalog;
            std::string baseURL = base.rfind("https://", 0) == 0 ? base : kDefaultAttachments;
            if (ok && buildCatalog(baseURL, engines, models, catalog)) {
                g_catalog = std::move(catalog);
                g_catalogError.clear();
                g_catalogFresh = GetTickCount64();
                util::ensureDirectory(dataDir());
                util::writeFileAtomically(dataDir() + L"\\attachments.txt", baseURL);
                util::writeFileAtomically(dataDir() + L"\\engine-records.json", engines);
                util::writeFileAtomically(dataDir() + L"\\model-records.json", models);
            } else if (!g_catalog.ready) {
                g_catalogError = ok ? "Mozilla's list of languages couldn't be read." : util::toUTF8(error.empty() ? L"Couldn't reach Mozilla's server." : error);
                perf::note("Translator: " + g_catalogError);
            }
            finishCatalogLoad();
        });
    }).detach();
}

// Runs `then` once the list is there (or couldn't be had).
void withCatalog(std::function<void()> then)
{
    static bool triedCache = false;
    static ULONGLONG lastAttempt = 0;
    ULONGLONG now = GetTickCount64();
    if (!triedCache) {
        triedCache = true;
        ULONGLONG age = kCatalogMaxAge;
        if (loadCachedCatalog(&age) && age < kCatalogMaxAge)
            g_catalogFresh = now;
    }
    bool stale = !g_catalogFresh || now - g_catalogFresh > kCatalogMaxAge;
    // Offline: try again now and then, not on every page.
    ULONGLONG retry = g_catalog.ready ? 60ull * 60 * 1000 : 5ull * 60 * 1000;
    if (stale && !g_catalogLoading && (!lastAttempt || now - lastAttempt > retry)) {
        lastAttempt = now;
        refreshCatalog();
    }
    if (g_catalog.ready || !g_catalogLoading) {
        then();
        return;
    }
    g_catalogWaiting.push_back(std::move(then));
}

// ---- Downloads --------------------------------------------------------------------------

struct Job {
    std::string key;
    std::string label; // "Dutch"
    std::vector<File> files;
    std::vector<std::function<void(bool, const std::string&)>> done;
};

std::deque<Job> g_jobs;
bool g_downloading = false;
std::string g_downloadLabel;
uint64_t g_downloadReceived = 0, g_downloadTotal = 0;

// The toolbars, at most about three times a second.
bool g_refreshScheduled = false;
ULONGLONG g_lastRefresh = 0;

void CALLBACK refreshTimer(HWND, UINT, UINT_PTR id, DWORD)
{
    KillTimer(nullptr, id);
    g_refreshScheduled = false;
    g_lastRefresh = GetTickCount64();
    BrowserWindow::refreshToolbars();
}

void refreshSoon()
{
    if (g_refreshScheduled)
        return;
    ULONGLONG now = GetTickCount64();
    if (now - g_lastRefresh >= 300) {
        g_lastRefresh = now;
        BrowserWindow::refreshToolbars();
        return;
    }
    g_refreshScheduled = SetTimer(nullptr, 0, static_cast<UINT>(300 - (now - g_lastRefresh)), refreshTimer) != 0;
    if (!g_refreshScheduled)
        BrowserWindow::refreshToolbars();
}

void runNextJob();

void finishJob(bool ok, const std::string& error)
{
    Job job = std::move(g_jobs.front());
    g_jobs.pop_front();
    g_downloading = false;
    g_downloadLabel.clear();
    g_downloadReceived = g_downloadTotal = 0;
    for (auto& callback : job.done)
        callback(ok, error);
    refreshSoon();
    runNextJob();
}

void runNextJob()
{
    if (g_downloading || g_jobs.empty())
        return;
    Job& job = g_jobs.front();
    std::vector<File> missing;
    uint64_t total = 0;
    for (auto& file : job.files) {
        if (!have(file)) {
            missing.push_back(file);
            total += file.size;
        }
    }
    if (missing.empty()) {
        g_downloading = true;
        finishJob(true, { });
        return;
    }
    g_downloading = true;
    g_downloadLabel = job.label;
    g_downloadReceived = 0;
    g_downloadTotal = total;
    refreshSoon();
    std::string base = g_catalog.base;
    std::thread([missing, base] {
        util::ensureDirectory(util::dataDirectory() + L"\\translations");
        util::ensureDirectory(filesDir());
        uint64_t before = 0;
        std::string failure;
        for (auto& file : missing) {
            std::string body;
            std::wstring error;
            uint64_t start = before;
            auto lastPosted = std::make_shared<uint64_t>(0);
            auto progress = [start, lastPosted](uint64_t received, uint64_t) {
                if (received - *lastPosted < 512 * 1024)
                    return;
                *lastPosted = received;
                uint64_t now = start + received;
                util::postToMainThread([now] {
                    g_downloadReceived = now;
                    refreshSoon();
                });
            };
            bool ok = false;
            for (int attempt = 0; attempt < 2 && !ok; attempt++) {
                body.clear();
                ok = http::get(util::fromUTF8(base + file.location), body, error, 512ull * 1024 * 1024, progress);
                if (ok && sha256Hex(body) != file.hash) {
                    ok = false;
                    error = L"A downloaded file didn't match its checksum.";
                }
            }
            if (!ok || !util::writeFileAtomically(pathOf(file), body)) {
                failure = ok ? "The language files couldn't be saved." : util::toUTF8(error.empty() ? L"The download failed." : error);
                break;
            }
            before += file.size;
        }
        util::postToMainThread([failure] {
            if (!failure.empty())
                perf::note("Translator download: " + failure);
            finishJob(failure.empty(), failure);
        });
    }).detach();
}

void download(const std::vector<File>& files, const std::string& label, std::function<void(bool, const std::string&)> done)
{
    std::vector<std::string> hashes;
    for (auto& file : files)
        hashes.push_back(file.hash);
    std::sort(hashes.begin(), hashes.end());
    std::string key;
    for (auto& hash : hashes)
        key += hash.substr(0, 16);
    for (auto& job : g_jobs) {
        if (job.key == key) {
            job.done.push_back(std::move(done));
            return;
        }
    }
    Job job;
    job.key = key;
    job.label = label;
    job.files = files;
    job.done.push_back(std::move(done));
    g_jobs.push_back(std::move(job));
    runNextJob();
}

// The models from `from` to `to`: direct, or through English.
std::vector<const Pair*> chainFor(const std::string& from, const std::string& to)
{
    std::vector<const Pair*> chain;
    if (from == to)
        return chain;
    auto direct = g_catalog.pairs.find(from + "-" + to);
    if (direct != g_catalog.pairs.end())
        return { &direct->second };
    auto first = g_catalog.pairs.find(from + "-en");
    auto second = g_catalog.pairs.find("en-" + to);
    if (from == "en" || to == "en" || first == g_catalog.pairs.end() || second == g_catalog.pairs.end())
        return chain;
    return { &first->second, &second->second };
}

std::vector<File> filesFor(const std::vector<const Pair*>& chain)
{
    std::vector<File> files { g_catalog.engine };
    for (auto* pair : chain) {
        for (auto& [type, file] : pair->files)
            files.push_back(file);
    }
    return files;
}

std::string languageOfChain(const std::vector<const Pair*>& chain)
{
    std::set<std::string> names;
    for (auto* pair : chain) {
        if (pair->from != "en")
            names.insert(util::toUTF8(languageName(pair->from)));
        if (pair->to != "en")
            names.insert(util::toUTF8(languageName(pair->to)));
    }
    std::string label;
    for (auto& name : names)
        label += (label.empty() ? "" : " and ") + name;
    return label.empty() ? "English" : label;
}

// ---- The hidden translation page ------------------------------------------------------

struct Call {
    std::function<void(const json::Value&)> done;
    ULONGLONG started { 0 };
    bool translating { false };
};

struct EngineHost {
    HWND hwnd { };
    Ref<WKViewRef> view;
    WKPageRef page { };
    bool ready { false };
    std::vector<std::string> queued;
    ULONGLONG createdAt { 0 };
};

std::unique_ptr<EngineHost> g_engine;
std::map<int, Call> g_calls;
int g_nextCall = 1;
ULONGLONG g_lastUse = 0;
ULONGLONG g_lastTranslate = 0;
bool g_translatorLoaded = false;
constexpr UINT_PTR kTickTimer = 1;

void closeEngine(const std::string& why);

void failCalls(const std::string& why)
{
    auto calls = std::move(g_calls);
    g_calls.clear();
    std::string text = "{\"ok\":false,\"error\":" + json::quote(why) + "}";
    json::Value answer;
    json::parse(text, answer);
    for (auto& [id, call] : calls)
        call.done(answer);
}

void evaluate(const std::string& script)
{
    if (!g_engine)
        return;
    if (!g_engine->ready) {
        g_engine->queued.push_back(script);
        return;
    }
    WKPageEvaluateJavaScriptInMainFrame(g_engine->page, stringUTF8(script).get(), nullptr, nullptr);
}

void engineAnswered(int id, const std::string& text)
{
    if (id == 0) {
        if (g_engine && !g_engine->ready) {
            g_engine->ready = true;
            auto queued = std::move(g_engine->queued);
            g_engine->queued.clear();
            for (auto& script : queued)
                evaluate(script);
        }
        return;
    }
    auto found = g_calls.find(id);
    if (found == g_calls.end())
        return;
    Call call = std::move(found->second);
    g_calls.erase(found);
    json::Value answer;
    if (!json::parse(text, answer))
        json::parse("{\"ok\":false,\"error\":\"The translator sent something unreadable.\"}", answer);
    call.done(answer);
}

void enginePrompt(WKPageRef, WKStringRef message, WKStringRef, WKFrameRef frame, WKSecurityOriginRef, WKPageRunJavaScriptPromptResultListenerRef listener, const void*)
{
    std::string text = util::toUTF8(toWide(message));
    WKPageRunJavaScriptPromptResultListenerCall(listener, stringUTF8("").get());
    if (!frame || !WKFrameIsMainFrame(frame) || text.rfind("prism-translate:", 0) != 0)
        return;
    size_t newline = text.find('\n');
    if (newline == std::string::npos)
        return;
    int id = atoi(text.substr(16, newline - 16).c_str());
    std::string body = text.substr(newline + 1);
    // Not inside WebKit's callback: answers may start more work.
    util::postToMainThread([id, body] { engineAnswered(id, body); });
}

void engineTerminated(WKPageRef, uint32_t, const void*)
{
    util::postToMainThread([] { closeEngine("The translator stopped (it may have run out of memory)."); });
}

void engineFailed(WKPageRef, WKNavigationRef, WKErrorRef, WKTypeRef, const void*)
{
    util::postToMainThread([] { closeEngine("The translator couldn't start."); });
}

void tick()
{
    ULONGLONG now = GetTickCount64();
    std::vector<int> late;
    for (auto& [id, call] : g_calls) {
        if (now - call.started > kCallTimeout)
            late.push_back(id);
    }
    for (int id : late)
        engineAnswered(id, "{\"ok\":false,\"error\":\"The translator took too long.\"}");
    if (!g_engine)
        return;
    if (!g_engine->ready && now - g_engine->createdAt > 60000) {
        closeEngine("The translator didn't start.");
        return;
    }
    if (g_calls.empty()) {
        if (g_translatorLoaded && now - g_lastTranslate > kTranslatorIdle) {
            // Frees the models' memory; they load again when needed.
            g_translatorLoaded = false;
            int id = g_nextCall++;
            g_calls[id] = { [](const json::Value&) { }, now, false };
            evaluate("prismEngine.handle(" + std::to_string(id) + ",{\"op\":\"unload\"})");
        } else if (now - g_lastUse > kEngineIdle) {
            closeEngine({ });
        }
    }
}

LRESULT CALLBACK engineWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_TIMER && wParam == kTickTimer) {
        tick();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void closeEngine(const std::string& why)
{
    std::unique_ptr<EngineHost> engine = std::move(g_engine);
    g_translatorLoaded = false;
    if (!why.empty())
        perf::note("Translator: " + why);
    failCalls(why.empty() ? std::string("The translator was closed.") : why);
    if (!engine)
        return;
    KillTimer(engine->hwnd, kTickTimer);
    WKPageUIClientV6 ui = { };
    ui.base.version = 6;
    WKPageSetPageUIClient(engine->page, &ui.base);
    WKPageNavigationClientV3 navigation = { };
    navigation.base.version = 3;
    WKPageSetPageNavigationClient(engine->page, &navigation.base);
    WKPageClose(engine->page);
    engine->view.reset();
    DestroyWindow(engine->hwnd);
}

std::string engineBaseURL()
{
    uint16_t port = extensions::server::start();
    if (!port)
        return { };
    return "http://" + std::string(kSiteId) + ".localhost:" + std::to_string(port) + "/";
}

bool ensureEngine()
{
    if (g_engine)
        return true;
    std::string base = engineBaseURL();
    WKPageConfigurationRef configuration = engine::extensionPageConfiguration();
    if (base.empty() || !configuration)
        return false;
    extensions::server::Site site;
    site.id = kSiteId;
    site.dir = fs::path(util::exeDirectory() + L"\\ui\\translate");
    site.mounts.push_back({ "files", fs::path(filesDir()) });
    extensions::server::setPrismSite(site);

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW windowClass = { sizeof(windowClass) };
        windowClass.lpfnWndProc = engineWindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"PrismTranslator";
        RegisterClassExW(&windowClass);
        registered = true;
    }
    auto host = std::make_unique<EngineHost>();
    host->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"PrismTranslator", L"", WS_POPUP, 0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!host->hwnd)
        return false;
    RECT rect = { 0, 0, 800, 600 };
    host->view = Ref<WKViewRef>::adopt(WKViewCreate(rect, configuration, host->hwnd));
    if (!host->view) {
        DestroyWindow(host->hwnd);
        return false;
    }
    WKViewSetIsInWindow(host->view.get(), true);
    host->page = WKViewGetPage(host->view.get());
    host->createdAt = GetTickCount64();

    WKPageUIClientV6 ui = { };
    ui.base.version = 6;
    ui.runJavaScriptPrompt = enginePrompt;
    WKPageSetPageUIClient(host->page, &ui.base);
    WKPageNavigationClientV3 navigation = { };
    navigation.base.version = 3;
    navigation.webProcessDidTerminate = engineTerminated;
    navigation.didFailProvisionalNavigation = engineFailed;
    WKPageSetPageNavigationClient(host->page, &navigation.base);
    SetTimer(host->hwnd, kTickTimer, 5000, nullptr);

    WKPageLoadURL(host->page, url(util::fromUTF8(base + "engine.html")).get());
    g_engine = std::move(host);
    return true;
}

void callEngine(const std::string& request, bool translating, std::function<void(const json::Value&)> done)
{
    g_lastUse = GetTickCount64();
    if (translating) {
        g_lastTranslate = g_lastUse;
        g_translatorLoaded = true;
    }
    if (!ensureEngine()) {
        json::Value answer;
        json::parse("{\"ok\":false,\"error\":\"The translator couldn't start.\"}", answer);
        done(answer);
        return;
    }
    int id = g_nextCall++;
    g_calls[id] = { std::move(done), GetTickCount64(), translating };
    evaluate("prismEngine.handle(" + std::to_string(id) + "," + request + ")");
}

// ---- Settings and choices -------------------------------------------------------------

std::string g_pageSource; // ui/translate-page.js

std::string mode()
{
    std::string value = util::toUTF8(util::readSetting(L"TranslateMode", L"auto"));
    return value == "offer" || value == "off" ? value : "auto";
}

std::string targetLanguage()
{
    std::string value = normalizeLanguage(util::toUTF8(util::readSetting(L"TranslateTo", L"en")));
    if (util::toUTF8(util::readSetting(L"TranslateTo", L"en")) == "zh-Hant")
        value = "zh-Hant";
    return value.empty() ? "en" : value;
}

struct Prefs {
    std::set<std::string> neverLanguages, alwaysLanguages, neverSites;
};
Prefs g_prefs;
std::set<std::string> g_privateNeverSites; // private windows: only until Prism closes
bool g_prefsLoaded = false;

std::wstring prefsPath()
{
    return util::dataDirectory() + L"\\translate.json";
}

void loadPrefs()
{
    if (g_prefsLoaded)
        return;
    g_prefsLoaded = true;
    json::Value root;
    if (!json::parse(util::readFile(prefsPath()), root) || !root.isObject())
        return;
    auto read = [&](const char* key, std::set<std::string>& out) {
        for (auto& item : root[key].items()) {
            if (item.isString() && !item.asString().empty() && item.asString().size() < 256)
                out.insert(item.asString());
        }
    };
    read("neverLanguages", g_prefs.neverLanguages);
    read("alwaysLanguages", g_prefs.alwaysLanguages);
    read("neverSites", g_prefs.neverSites);
}

std::string listJSON(const std::set<std::string>& items)
{
    std::string json = "[";
    for (auto& item : items)
        json += (json.size() > 1 ? "," : "") + json::quote(item);
    return json + "]";
}

void savePrefs()
{
    util::writeFileAtomically(prefsPath(), "{\"neverLanguages\":" + listJSON(g_prefs.neverLanguages) + ",\"alwaysLanguages\":" + listJSON(g_prefs.alwaysLanguages)
        + ",\"neverSites\":" + listJSON(g_prefs.neverSites) + "}\n");
}

std::string siteOf(const std::wstring& address)
{
    std::string text = util::toUTF8(address);
    size_t start = text.find("://");
    if (start == std::string::npos)
        return { };
    std::string host = text.substr(start + 3);
    host = host.substr(0, host.find_first_of("/?#"));
    size_t at = host.rfind('@');
    if (at != std::string::npos)
        host.erase(0, at + 1);
    size_t port = host.rfind(':');
    if (port != std::string::npos && host.find(']') == std::string::npos)
        host.erase(port);
    host = lowerASCII(host);
    if (host.rfind("www.", 0) == 0)
        host.erase(0, 4);
    return host;
}

bool isNeverSite(const std::string& site, bool privatePage)
{
    return g_prefs.neverSites.count(site) || (privatePage && g_privateNeverSites.count(site));
}

// Can text in `from` be translated into `to`?
bool canTranslate(const std::string& from, const std::string& to)
{
    return from != to && !chainFor(from, to).empty();
}

// What the page is in, as a language with models (or "").
std::string supportedSource(const std::string& code)
{
    std::string normalized = normalizeLanguage(code);
    if (normalized == "en")
        return "en";
    if (g_catalog.pairs.count(normalized + "-en"))
        return normalized;
    if (normalized == "zh-Hant" && g_catalog.pairs.count("zh-en"))
        return "zh";
    if (normalized == "nn" && g_catalog.pairs.count("nb-en"))
        return "nb";
    return { };
}

std::vector<std::string> targets()
{
    std::set<std::string> codes { "en" };
    for (auto& [key, pair] : g_catalog.pairs) {
        if (pair.from == "en")
            codes.insert(pair.to);
    }
    std::vector<std::string> list(codes.begin(), codes.end());
    std::sort(list.begin(), list.end(), [](const std::string& a, const std::string& b) { return languageName(a) < languageName(b); });
    return list;
}

std::vector<std::string> sources()
{
    std::set<std::string> codes { "en" };
    for (auto& [key, pair] : g_catalog.pairs) {
        if (pair.to == "en")
            codes.insert(pair.from);
    }
    std::vector<std::string> list(codes.begin(), codes.end());
    std::sort(list.begin(), list.end(), [](const std::string& a, const std::string& b) { return languageName(a) < languageName(b); });
    return list;
}

// ---- Pages ------------------------------------------------------------------------------

enum class NextLoad { Normal, Original, Force };

struct PageState {
    int generation { 0 };
    std::string language; // what the page is in ("" = not known or same as yours)
    std::string from, to;
    bool translating { false };  // asked to translate this document
    int pending { 0 };           // translations on their way
    int waitingForFiles { 0 };   // of those, waiting for a download
    int done { 0 };              // pieces translated
    std::string error;
    bool privatePage { false };
    NextLoad next { NextLoad::Normal };
    std::string nextFrom, nextTo;
};

std::map<WKPageRef, PageState> g_pages;

PageState& stateFor(WKPageRef page)
{
    return g_pages[page];
}

void startOnPage(WKPageRef page, const std::string& from, const std::string& to)
{
    PageState& state = stateFor(page);
    state.from = from;
    state.to = to;
    state.translating = true;
    state.error.clear();
    if (WKPagePostMessageToInjectedBundle) {
        std::string script = "window.__prismTranslate ? window.__prismTranslate.start(" + json::quote(from) + "," + json::quote(to) + ") : 'missing'";
        WKPagePostMessageToInjectedBundle(page, stringUTF8("prism:exec").get(), stringUTF8(std::string("0\t") + kSiteId + "\t" + script).get());
    }
    refreshSoon();
}

void reloadPage(WKPageRef page, NextLoad next, const std::string& from = { }, const std::string& to = { })
{
    PageState& state = stateFor(page);
    state.next = next;
    state.nextFrom = from;
    state.nextTo = to;
    WKPageReload(page);
}

std::string answerJSON(const std::string& action, const std::string& from = { }, const std::string& to = { })
{
    std::string json = "{\"action\":" + json::quote(action);
    if (!from.empty())
        json += ",\"from\":" + json::quote(from) + ",\"to\":" + json::quote(to);
    return json + "}";
}

void decide(WKPageRef page, int generation, const json::Value& detected, const std::string& declared, std::function<void(const std::string&)> reply)
{
    auto found = g_pages.find(page);
    if (found == g_pages.end() || found->second.generation != generation) {
        reply(answerJSON("none"));
        return;
    }
    PageState& state = found->second;
    std::string to = targetLanguage();
    std::string language;
    std::string guess = detected["language"].asString();
    bool confident = detected["confident"].asBool();
    std::string fromDeclared = declared.empty() ? std::string() : supportedSource(declared);
    if (confident && guess != "un" && !guess.empty())
        language = supportedSource(guess);
    // Not sure (short pages, mixed languages): what the page says it's in,
    // when that's at least among the detector's guesses.
    if (language.empty() && !fromDeclared.empty()) {
        bool plausible = normalizeLanguage(guess) == normalizeLanguage(declared);
        for (auto& entry : detected["languages"].items())
            plausible = plausible || normalizeLanguage(entry["languageCode"].asString()) == normalizeLanguage(declared);
        if (plausible || guess.empty() || guess == "un")
            language = fromDeclared;
    }
    NextLoad next = state.next;
    std::string nextFrom = state.nextFrom, nextTo = state.nextTo;
    state.next = NextLoad::Normal;
    if (next == NextLoad::Force && canTranslate(nextFrom, nextTo)) {
        state.language = nextFrom;
        state.from = nextFrom;
        state.to = nextTo;
        state.translating = true;
        refreshSoon();
        reply(answerJSON("translate", nextFrom, nextTo));
        return;
    }
    if (language.empty() || normalizeLanguage(language) == normalizeLanguage(to) || !canTranslate(language, to)) {
        state.language.clear();
        refreshSoon();
        reply(answerJSON("none"));
        return;
    }
    state.language = language;
    state.from = language;
    state.to = to;
    tabs::Info tab;
    std::string site = tabs::findByPageAnyWindow(page, tab) ? siteOf(tab.url) : std::string();
    bool automatic = (mode() == "auto" || g_prefs.alwaysLanguages.count(language)) && !g_prefs.neverLanguages.count(language)
        && !isNeverSite(site, state.privatePage) && next != NextLoad::Original;
    state.translating = automatic;
    refreshSoon();
    reply(automatic ? answerJSON("translate", language, to) : answerJSON("none"));
}

std::string modelsJSON(const std::vector<const Pair*>& chain)
{
    std::string json = "[";
    for (auto* pair : chain) {
        json += std::string(json.size() > 1 ? "," : "") + "{\"key\":" + json::quote(pair->from + "-" + pair->to + "@" + pair->version)
            + ",\"from\":" + json::quote(pair->from) + ",\"to\":" + json::quote(pair->to) + ",\"files\":{";
        bool first = true;
        for (auto& [type, file] : pair->files) {
            json += std::string(first ? "" : ",") + json::quote(type) + ":{\"url\":" + json::quote("files/" + file.hash + ".zst") + ",\"name\":" + json::quote(file.name) + "}";
            first = false;
        }
        json += "}}";
    }
    return json + "]";
}

void translateItems(WKPageRef page, int generation, const std::string& from, const std::string& to, const json::Value& items, std::function<void(const std::string&)> reply)
{
    std::vector<const Pair*> chain = chainFor(from, to);
    if (chain.empty()) {
        reply("{\"error\":\"These languages can't be translated.\"}");
        return;
    }
    // Only what the page script sends: {h, t} pieces of text.
    std::string itemsJSON = "[";
    size_t total = 0;
    for (auto& item : items.items()) {
        std::string text = item["t"].asString();
        total += text.size();
        itemsJSON += std::string(itemsJSON.size() > 1 ? "," : "") + "{\"h\":" + (item["h"].asBool() ? "true" : "false") + ",\"t\":" + json::quote(text) + "}";
    }
    itemsJSON += "]";
    if (items.size() > 64 || total > 64 * 1024) {
        reply("{\"error\":\"Too much at once.\"}");
        return;
    }
    PageState& state = stateFor(page);
    state.pending++;
    std::vector<File> files = filesFor(chain);
    bool missing = std::any_of(files.begin(), files.end(), [](const File& file) { return !have(file); });
    if (missing)
        state.waitingForFiles++;
    refreshSoon();
    std::string request = "{\"op\":\"translate\",\"wasm\":" + json::quote("files/" + g_catalog.engine.hash + ".zst") + ",\"models\":" + modelsJSON(chain) + ",\"items\":" + itemsJSON + "}";
    std::string label = languageOfChain(chain);
    // Changes this page's counts, if it's still the same document.
    auto update = [page, generation](std::function<void(PageState&)> change) {
        auto found = g_pages.find(page);
        if (found != g_pages.end() && found->second.generation == generation) {
            change(found->second);
            refreshSoon();
        }
    };
    int count = static_cast<int>(items.size());
    download(files, label, [request, reply, update, missing, count](bool ok, const std::string& error) {
        if (!ok) {
            update([missing, error](PageState& state) {
                state.pending = std::max(0, state.pending - 1);
                if (missing)
                    state.waitingForFiles = std::max(0, state.waitingForFiles - 1);
                state.error = error;
            });
            reply("{\"error\":" + json::quote(error) + "}");
            return;
        }
        if (missing)
            update([](PageState& state) { state.waitingForFiles = std::max(0, state.waitingForFiles - 1); });
        callEngine(request, true, [reply, update, count](const json::Value& answer) {
            bool ok = answer["ok"].asBool() && answer["results"].isArray();
            std::string error = ok ? std::string() : answer["error"].asString();
            update([ok, error, count](PageState& state) {
                state.pending = std::max(0, state.pending - 1);
                if (ok)
                    state.done += count;
                else
                    state.error = error.empty() ? "The translation failed." : error;
            });
            if (!ok) {
                reply("{\"error\":" + json::quote(error) + "}");
                return;
            }
            std::string raw = "[";
            for (auto& result : answer["results"].items())
                raw += std::string(raw.size() > 1 ? "," : "") + (result.isString() ? json::quote(result.asString()) : std::string("null"));
            reply("{\"results\":" + raw + "]}");
        });
    });
}

} // namespace

// ---- Public -----------------------------------------------------------------------------

void initialize()
{
    g_pageSource = util::readFile(util::exeDirectory() + L"\\ui\\translate-page.js");
    loadPrefs();
}

void shutdown()
{
    closeEngine({ });
}

std::string withTranslator(const std::string& payload)
{
    if (g_pageSource.empty() || mode() == "off")
        return payload;
    size_t newline = payload.find('\n');
    int count = newline == std::string::npos ? 0 : atoi(payload.substr(0, newline).c_str());
    std::string rest = newline == std::string::npos ? std::string() : payload.substr(newline + 1);
    return std::to_string(count + 1) + "\n" + kSiteId + "\t" + std::to_string(g_pageSource.size()) + "\n" + g_pageSource + rest;
}

void handleMessage(WKPageRef page, bool mainFrame, const std::string& text, std::function<void(const std::string&)> reply)
{
    json::Value message;
    if (!mainFrame || text.size() > 256 * 1024 || !json::parse(text, message) || !message.isObject()) {
        reply("null");
        return;
    }
    std::string op = message["op"].asString();
    PageState& state = stateFor(page);
    int generation = state.generation;
    if (op == "detect") {
        if (mode() == "off" && state.next != NextLoad::Force) {
            reply(answerJSON("none"));
            return;
        }
        tabs::Info tab;
        if (tabs::findByPageAnyWindow(page, tab))
            state.privatePage = tab.isPrivate;
        std::string declared = message["lang"].asString().substr(0, 35);
        std::string request = "{\"op\":\"detect\",\"text\":" + json::quote(message["text"].asString().substr(0, 8000)) + "}";
        withCatalog([page, generation, request, declared, reply] {
            if (!g_catalog.ready) {
                reply(answerJSON("none"));
                return;
            }
            callEngine(request, false, [page, generation, declared, reply](const json::Value& answer) {
                if (!answer["ok"].asBool()) {
                    reply(answerJSON("none"));
                    return;
                }
                decide(page, generation, answer["result"], declared, reply);
            });
        });
        return;
    }
    if (op == "translate") {
        std::string from = message["from"].asString(), to = message["to"].asString();
        if (!message["items"].isArray() || from.empty() || to.empty()) {
            reply("{\"error\":\"Nothing to translate.\"}");
            return;
        }
        json::Value items = message["items"];
        withCatalog([page, generation, from, to, items, reply] {
            if (!g_catalog.ready) {
                reply("{\"error\":" + json::quote(g_catalogError.empty() ? std::string("The list of languages isn't available.") : g_catalogError) + "}");
                return;
            }
            translateItems(page, generation, from, to, items, reply);
        });
        return;
    }
    reply("null");
}

std::string stateJSON(WKPageRef page)
{
    auto found = g_pages.find(page);
    if (found == g_pages.end() || found->second.language.empty() || mode() == "off")
        return "null";
    const PageState& state = found->second;
    std::string status;
    double progress = -1;
    if (state.waitingForFiles > 0 && g_downloading) {
        status = "downloading";
        if (g_downloadTotal)
            progress = static_cast<double>(g_downloadReceived) / static_cast<double>(g_downloadTotal);
    } else if (state.pending > 0) {
        status = "translating";
    } else if (state.translating && state.done > 0) {
        status = "translated";
    } else if (state.translating && !state.error.empty()) {
        status = "failed";
    } else if (state.translating) {
        status = "translating";
    } else {
        status = "offer";
    }
    char number[32];
    snprintf(number, sizeof(number), "%.3f", progress);
    return "{\"from\":" + json::quote(state.from) + ",\"fromName\":" + util::jsonString(languageName(state.from))
        + ",\"to\":" + json::quote(state.to) + ",\"toName\":" + util::jsonString(languageName(state.to))
        + ",\"status\":\"" + status + "\",\"progress\":" + number + ",\"error\":" + json::quote(state.error) + "}";
}

void pageCommitted(WKPageRef page)
{
    PageState& state = stateFor(page);
    NextLoad next = state.next;
    std::string nextFrom = state.nextFrom, nextTo = state.nextTo;
    int generation = state.generation + 1;
    bool privatePage = state.privatePage;
    state = PageState();
    state.generation = generation;
    state.next = next;
    state.nextFrom = nextFrom;
    state.nextTo = nextTo;
    state.privatePage = privatePage;
}

void pageClosed(WKPageRef page)
{
    g_pages.erase(page);
}

bool showMenu(WKPageRef page, HWND owner, POINT point)
{
    auto found = g_pages.find(page);
    if (found == g_pages.end() || found->second.language.empty())
        return false;
    PageState& state = found->second;
    tabs::Info tab;
    std::string site = tabs::findByPageAnyWindow(page, tab) ? siteOf(tab.url) : std::string();
    std::string language = state.from;
    std::wstring name = languageName(language);
    bool automatic = mode() == "auto";
    enum { Translate = 1, Original, NeverLanguage, AlwaysLanguage, NeverSite, Settings, Target = 100 };

    HMENU menu = CreatePopupMenu();
    std::wstring heading = state.translating ? L"Translated from " + name + L" to " + languageName(state.to) : L"This page is in " + name;
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, heading.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (state.translating)
        AppendMenuW(menu, MF_STRING, Original, L"Show original");
    else
        AppendMenuW(menu, MF_STRING, Translate, (L"Translate to " + languageName(targetLanguage())).c_str());
    HMENU targetsMenu = CreatePopupMenu();
    std::vector<std::string> targetList = targets();
    for (size_t i = 0; i < targetList.size(); i++) {
        const std::string& code = targetList[i];
        if (code == language || !canTranslate(language, code))
            continue;
        UINT flags = MF_STRING | (state.translating && code == state.to ? MF_CHECKED : 0);
        AppendMenuW(targetsMenu, flags, Target + i, languageName(code).c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(targetsMenu), L"Translate to another language");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (automatic)
        AppendMenuW(menu, MF_STRING | (g_prefs.neverLanguages.count(language) ? MF_CHECKED : 0), NeverLanguage, (L"Never translate " + name).c_str());
    else
        AppendMenuW(menu, MF_STRING | (g_prefs.alwaysLanguages.count(language) ? MF_CHECKED : 0), AlwaysLanguage, (L"Always translate " + name).c_str());
    if (!site.empty())
        AppendMenuW(menu, MF_STRING | (isNeverSite(site, state.privatePage) ? MF_CHECKED : 0), NeverSite, (L"Never translate " + util::fromUTF8(site)).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Settings, L"Translation settings");

    SetForegroundWindow(owner);
    int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, point.x, point.y, 0, owner, nullptr);
    DestroyMenu(menu); // and its submenu

    // The page may have closed or moved on while the menu was open.
    found = g_pages.find(page);
    if (!command || found == g_pages.end())
        return false;
    PageState& now = found->second;
    switch (command) {
    case Translate:
        startOnPage(page, language, targetLanguage());
        return false;
    case Original:
        reloadPage(page, NextLoad::Original);
        return false;
    case NeverLanguage:
        if (!g_prefs.neverLanguages.erase(language)) {
            g_prefs.neverLanguages.insert(language);
            if (now.translating)
                reloadPage(page, NextLoad::Original);
        }
        savePrefs();
        return false;
    case AlwaysLanguage:
        if (!g_prefs.alwaysLanguages.erase(language)) {
            g_prefs.alwaysLanguages.insert(language);
            if (!now.translating)
                startOnPage(page, language, targetLanguage());
        }
        savePrefs();
        return false;
    case NeverSite: {
        bool privatePage = now.privatePage;
        std::set<std::string>& sites = privatePage ? g_privateNeverSites : g_prefs.neverSites;
        if (g_prefs.neverSites.erase(site) || (privatePage && g_privateNeverSites.erase(site))) {
            if (!privatePage)
                savePrefs();
        } else {
            sites.insert(site);
            if (!privatePage)
                savePrefs();
            if (now.translating)
                reloadPage(page, NextLoad::Original);
        }
        return false;
    }
    case Settings:
        return true;
    default:
        if (command >= Target && static_cast<size_t>(command - Target) < targetList.size()) {
            std::string to = targetList[command - Target];
            if (now.translating)
                reloadPage(page, NextLoad::Force, language, to); // from the original text
            else
                startOnPage(page, language, to);
        }
        return false;
    }
}

// ---- prism://settings -------------------------------------------------------------------

namespace {

uint64_t sizeOf(const std::vector<const Pair*>& pairs)
{
    uint64_t size = 0;
    for (auto* pair : pairs) {
        for (auto& [type, file] : pair->files)
            size += file.size;
    }
    return size;
}

std::vector<const Pair*> pairsOf(const std::string& code)
{
    std::vector<const Pair*> pairs;
    for (auto* key : { "-en", "en-" }) {
        std::string name = std::string(key) == "-en" ? code + "-en" : "en-" + code;
        auto found = g_catalog.pairs.find(name);
        if (found != g_catalog.pairs.end())
            pairs.push_back(&found->second);
    }
    return pairs;
}

bool downloaded(const std::vector<const Pair*>& pairs)
{
    bool any = false;
    for (auto* pair : pairs) {
        for (auto& [type, file] : pair->files) {
            if (!have(file))
                return false;
            any = true;
        }
    }
    return any;
}

std::string languagesJSON(const std::set<std::string>& codes)
{
    std::string json = "[";
    for (auto& code : codes)
        json += std::string(json.size() > 1 ? "," : "") + "{\"code\":" + json::quote(code) + ",\"name\":" + util::jsonString(languageName(code)) + "}";
    return json + "]";
}

std::string settingsJSON()
{
    std::string json = "{\"mode\":" + json::quote(mode()) + ",\"to\":" + json::quote(targetLanguage()) + ",\"toName\":" + util::jsonString(languageName(targetLanguage()))
        + ",\"available\":" + (extensions::isolatedContentScripts() ? "true" : "false")
        + ",\"catalog\":" + json::quote(g_catalog.ready ? "ready" : g_catalogLoading ? "loading" : g_catalogError.empty() ? "none" : "error")
        + ",\"error\":" + json::quote(g_catalogError);
    json += ",\"targets\":[";
    bool first = true;
    for (auto& code : targets()) {
        json += std::string(first ? "" : ",") + "{\"code\":" + json::quote(code) + ",\"name\":" + util::jsonString(languageName(code)) + "}";
        first = false;
    }
    json += "],\"languages\":[";
    first = true;
    uint64_t used = 0;
    for (auto& code : sources()) {
        if (code == "en")
            continue;
        auto pairs = pairsOf(code);
        bool have = downloaded(pairs);
        if (have)
            used += sizeOf(pairs);
        json += std::string(first ? "" : ",") + "{\"code\":" + json::quote(code) + ",\"name\":" + util::jsonString(languageName(code))
            + ",\"size\":" + std::to_string(sizeOf(pairs)) + ",\"downloaded\":" + (have ? "true" : "false") + "}";
        first = false;
    }
    json += "],\"used\":" + std::to_string(used);
    json += ",\"neverLanguages\":" + languagesJSON(g_prefs.neverLanguages) + ",\"alwaysLanguages\":" + languagesJSON(g_prefs.alwaysLanguages)
        + ",\"neverSites\":" + listJSON(g_prefs.neverSites);
    if (g_downloading)
        json += ",\"downloading\":{\"name\":" + json::quote(g_downloadLabel) + ",\"received\":" + std::to_string(g_downloadReceived) + ",\"total\":" + std::to_string(g_downloadTotal) + "}";
    return json + "}";
}

void removeFiles(const std::vector<const Pair*>& pairs)
{
    // Files other downloaded languages still use stay.
    std::set<std::string> keep { g_catalog.engine.hash };
    std::set<const Pair*> removing(pairs.begin(), pairs.end());
    for (auto& [key, pair] : g_catalog.pairs) {
        if (removing.count(&pair))
            continue;
        for (auto& [type, file] : pair.files) {
            if (have(file))
                keep.insert(file.hash);
        }
    }
    for (auto* pair : pairs) {
        for (auto& [type, file] : pair->files) {
            if (!keep.count(file.hash))
                DeleteFileW(pathOf(file).c_str());
        }
    }
}

} // namespace

std::string handleCommand(const std::wstring& command, const std::wstring& argument)
{
    loadPrefs();
    std::string arg = util::toUTF8(argument);
    if (command == L"set") {
        size_t newline = arg.find('\n');
        std::string key = arg.substr(0, newline), value = newline == std::string::npos ? std::string() : arg.substr(newline + 1);
        if (key == "mode" && (value == "auto" || value == "offer" || value == "off")) {
            bool changed = value != mode();
            util::writeSetting(L"TranslateMode", util::fromUTF8(value));
            if (changed)
                engine::sendContentScripts(); // pages loaded from now on
            BrowserWindow::refreshToolbars();
        } else if (key == "to" && !value.empty() && value.size() < 16) {
            util::writeSetting(L"TranslateTo", util::fromUTF8(value));
        }
    } else if (command == L"removeNeverSite") {
        g_prefs.neverSites.erase(arg);
        savePrefs();
    } else if (command == L"removeNeverLanguage") {
        g_prefs.neverLanguages.erase(arg);
        savePrefs();
    } else if (command == L"removeAlwaysLanguage") {
        g_prefs.alwaysLanguages.erase(arg);
        savePrefs();
    } else if (command == L"download" && g_catalog.ready) {
        auto pairs = pairsOf(arg);
        if (!pairs.empty())
            download(filesFor(pairs), util::toUTF8(languageName(arg)), [](bool, const std::string&) { });
    } else if (command == L"remove" && g_catalog.ready) {
        removeFiles(pairsOf(arg));
    } else if (command == L"removeAll") {
        closeEngine({ });
        std::error_code error;
        fs::remove_all(filesDir(), error);
    } else if (command == L"refresh") {
        refreshCatalog();
    }
    // translate.get (and every change answers the new state)
    if (!g_catalog.ready && !g_catalogLoading)
        withCatalog([] { });
    return settingsJSON();
}

} // namespace translate
