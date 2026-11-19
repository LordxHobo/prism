// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Bookmarks.h"

#include "Dialogs.h"
#include "Icons.h"
#include "Json.h"
#include "Util.h"

#include <algorithm>
#include <cwctype>
#include <shlobj.h>

namespace bookmarks {

namespace {

// bookmarks.json:
// {"version":1,"nextId":N,"bar":[node...],"other":[node...]}
// node: {"id":n,"title":"...","added":ms,"url":"..."} or, for folders,
// {"id":n,"title":"...","added":ms,"children":[node...]}

Node g_bar;
Node g_other;
long long g_nextId = 3;
bool g_loaded = false;
std::function<void()> g_listener;

std::wstring bookmarksFile()
{
    return util::dataDirectory() + L"\\bookmarks.json";
}

long long nowMilliseconds()
{
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return static_cast<long long>(value.QuadPart / 10000ULL) - 11644473600000LL;
}

std::wstring lower(std::wstring text)
{
    for (auto& c : text)
        c = static_cast<wchar_t>(towlower(c));
    return text;
}

// Only web and file addresses can be bookmarked (no javascript: and such).
bool isAllowedURL(const std::wstring& url)
{
    std::wstring scheme = lower(url.substr(0, url.find(L':')));
    return (scheme == L"http" || scheme == L"https" || scheme == L"file" || scheme == L"ftp")
        && url.find(L"://") != std::wstring::npos && url.size() < 8192;
}

std::wstring cleanTitle(std::wstring title)
{
    for (auto& c : title) {
        if (c < 0x20)
            c = L' ';
    }
    if (title.size() > 500)
        title.resize(500);
    return title;
}

Node* findIn(Node& node, long long id)
{
    if (node.id == id)
        return &node;
    for (auto& child : node.children) {
        if (Node* found = findIn(*child, id))
            return found;
    }
    return nullptr;
}

Node* findMutable(long long id)
{
    if (Node* found = findIn(g_bar, id))
        return found;
    return findIn(g_other, id);
}

const Node* findURLIn(const Node& node, const std::wstring& url)
{
    for (auto& child : node.children) {
        if (!child->folder && child->url == url)
            return child.get();
        if (child->folder) {
            if (const Node* found = findURLIn(*child, url))
                return found;
        }
    }
    return nullptr;
}

void readChildren(Node& parent, const json::Value& list, int depth)
{
    if (depth > 30)
        return;
    for (auto& item : list.items()) {
        auto node = std::make_unique<Node>();
        node->id = static_cast<long long>(item["id"].asNumber(0));
        if (node->id <= 2 || findMutable(node->id))
            node->id = g_nextId++;
        g_nextId = std::max(g_nextId, node->id + 1);
        node->title = util::fromUTF8(item["title"].asString());
        node->added = static_cast<long long>(item["added"].asNumber(0));
        node->parent = &parent;
        if (item.has("children")) {
            node->folder = true;
            parent.children.push_back(std::move(node));
            readChildren(*parent.children.back(), item["children"], depth + 1);
            continue;
        }
        node->url = util::fromUTF8(item["url"].asString());
        if (!isAllowedURL(node->url))
            continue;
        parent.children.push_back(std::move(node));
    }
}

std::string nodeJSON(const Node& node)
{
    std::string json = "{\"id\":" + std::to_string(node.id) + ",\"title\":" + util::jsonString(node.title)
        + ",\"added\":" + std::to_string(node.added);
    if (node.folder) {
        json += ",\"children\":[";
        for (size_t i = 0; i < node.children.size(); ++i)
            json += (i ? "," : "") + nodeJSON(*node.children[i]);
        json += "]";
    } else
        json += ",\"url\":" + util::jsonString(node.url) + ",\"icon\":" + util::jsonString(icons::fileURLFor(node.url));
    return json + "}";
}

std::string childrenJSON(const Node& node)
{
    std::string json = "[";
    for (size_t i = 0; i < node.children.size(); ++i)
        json += (i ? "," : "") + nodeJSON(*node.children[i]);
    return json + "]";
}

void save()
{
    std::string json = "{\"version\":1,\"nextId\":" + std::to_string(g_nextId)
        + ",\"bar\":" + childrenJSON(g_bar) + ",\"other\":" + childrenJSON(g_other) + "}";
    util::writeFileAtomically(bookmarksFile(), json);
}

void changed()
{
    save();
    if (g_listener)
        g_listener();
}

Node* addNode(Node& parent, bool folder, const std::wstring& title, const std::wstring& url, long long added = 0, int index = -1)
{
    auto node = std::make_unique<Node>();
    node->id = g_nextId++;
    node->folder = folder;
    node->title = cleanTitle(title);
    node->url = url;
    node->added = added ? added : nowMilliseconds();
    node->parent = &parent;
    Node* raw = node.get();
    if (index < 0 || index > static_cast<int>(parent.children.size()))
        parent.children.push_back(std::move(node));
    else
        parent.children.insert(parent.children.begin() + index, std::move(node));
    return raw;
}

bool isInside(const Node* node, const Node* folder)
{
    for (const Node* current = node; current; current = current->parent) {
        if (current == folder)
            return true;
    }
    return false;
}

size_t countBookmarks(const Node& node)
{
    size_t count = 0;
    for (auto& child : node.children)
        count += child->folder ? countBookmarks(*child) : 1;
    return count;
}

// ---- Importing ---------------------------------------------------------------

// Chrome-style "Bookmarks" files (Chrome, Edge, Brave, Vivaldi, Opera).
struct Browser {
    const char* id;
    const wchar_t* name;
    bool roaming; // under %APPDATA% instead of %LOCALAPPDATA%
    const wchar_t* path;
};

const Browser kBrowsers[] = {
    { "chrome", L"Google Chrome", false, L"Google\\Chrome\\User Data\\Default\\Bookmarks" },
    { "edge", L"Microsoft Edge", false, L"Microsoft\\Edge\\User Data\\Default\\Bookmarks" },
    { "brave", L"Brave", false, L"BraveSoftware\\Brave-Browser\\User Data\\Default\\Bookmarks" },
    { "vivaldi", L"Vivaldi", false, L"Vivaldi\\User Data\\Default\\Bookmarks" },
    { "opera", L"Opera", true, L"Opera Software\\Opera Stable\\Bookmarks" },
};

std::wstring knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)))
        result = path;
    CoTaskMemFree(path);
    return result;
}

std::wstring browserFile(const Browser& browser)
{
    return knownFolder(browser.roaming ? FOLDERID_RoamingAppData : FOLDERID_LocalAppData) + L"\\" + browser.path;
}

bool fileExists(const std::wstring& path)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Chrome stores times in microseconds since 1601.
long long chromeTime(const std::string& text)
{
    long long value = _atoi64(text.c_str());
    return value > 0 ? value / 1000 - 11644473600000LL : 0;
}

size_t importChromeNodes(Node& parent, const json::Value& list, int depth)
{
    if (depth > 30)
        return 0;
    size_t count = 0;
    for (auto& item : list.items()) {
        std::wstring title = util::fromUTF8(item["name"].asString());
        if (item["type"].asString() == "folder") {
            Node* folder = addNode(parent, true, title, L"", chromeTime(item["date_added"].asString()));
            count += importChromeNodes(*folder, item["children"], depth + 1);
        } else {
            std::wstring url = util::fromUTF8(item["url"].asString());
            if (!isAllowedURL(url))
                continue;
            addNode(parent, false, title, url, chromeTime(item["date_added"].asString()));
            ++count;
        }
    }
    return count;
}

std::string importBrowser(const std::string& id)
{
    for (auto& browser : kBrowsers) {
        if (id != browser.id)
            continue;
        json::Value root;
        if (!json::parse(util::readFile(browserFile(browser)), root) || !root["roots"].isObject())
            return "{\"error\":" + util::jsonString(std::wstring(L"Couldn't read the bookmarks of ") + browser.name + L".") + "}";
        auto& roots = root["roots"];
        size_t count = 0;
        // Their bookmarks bar becomes Prism's if Prism's is still empty;
        // everything else goes into a folder in Other bookmarks.
        Node* folder = nullptr;
        auto ensureFolder = [&]() -> Node& {
            if (!folder)
                folder = addNode(g_other, true, std::wstring(L"Imported from ") + browser.name, L"");
            return *folder;
        };
        if (g_bar.children.empty())
            count += importChromeNodes(g_bar, roots["bookmark_bar"]["children"], 0);
        else if (roots["bookmark_bar"]["children"].size()) {
            Node* bar = addNode(ensureFolder(), true, L"Bookmarks bar", L"");
            count += importChromeNodes(*bar, roots["bookmark_bar"]["children"], 0);
        }
        for (const char* key : { "other", "synced" }) {
            if (roots[key]["children"].size())
                count += importChromeNodes(ensureFolder(), roots[key]["children"], 0);
        }
        changed();
        return "{\"count\":" + std::to_string(count) + "}";
    }
    return "{\"error\":\"Unknown browser.\"}";
}

std::wstring decodeEntities(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out += text[i];
            continue;
        }
        size_t semi = text.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += '&';
            continue;
        }
        std::string name = text.substr(i + 1, semi - i - 1);
        unsigned long code = 0;
        if (name == "amp") code = '&';
        else if (name == "lt") code = '<';
        else if (name == "gt") code = '>';
        else if (name == "quot") code = '"';
        else if (name == "apos") code = '\'';
        else if (name == "nbsp") code = 0xA0;
        else if (!name.empty() && name[0] == '#')
            code = name.size() > 1 && (name[1] == 'x' || name[1] == 'X') ? strtoul(name.c_str() + 2, nullptr, 16) : strtoul(name.c_str() + 1, nullptr, 10);
        if (!code || code > 0x10FFFF) {
            out += '&';
            continue;
        }
        // Encode as UTF-8.
        if (code < 0x80) out += static_cast<char>(code);
        else if (code < 0x800) { out += static_cast<char>(0xC0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 0x3F)); }
        else if (code < 0x10000) { out += static_cast<char>(0xE0 | (code >> 12)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3F)); out += static_cast<char>(0x80 | (code & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (code >> 18)); out += static_cast<char>(0x80 | ((code >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3F)); out += static_cast<char>(0x80 | (code & 0x3F)); }
        i = semi;
    }
    return util::fromUTF8(out);
}

std::string lowerASCII(std::string text)
{
    for (auto& c : text)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string attribute(const std::string& tag, const std::string& name)
{
    std::string lowered = lowerASCII(tag);
    size_t at = 0;
    while ((at = lowered.find(name, at)) != std::string::npos) {
        // A whole attribute name, followed by "=".
        bool startOK = at > 0 && isspace(static_cast<unsigned char>(lowered[at - 1]));
        size_t eq = at + name.size();
        while (eq < lowered.size() && isspace(static_cast<unsigned char>(lowered[eq])))
            ++eq;
        if (!startOK || eq >= lowered.size() || lowered[eq] != '=') {
            at += name.size();
            continue;
        }
        size_t value = eq + 1;
        while (value < tag.size() && isspace(static_cast<unsigned char>(tag[value])))
            ++value;
        if (value >= tag.size())
            return { };
        char quote = tag[value];
        if (quote == '"' || quote == '\'') {
            size_t end = tag.find(quote, value + 1);
            return tag.substr(value + 1, end == std::string::npos ? std::string::npos : end - value - 1);
        }
        size_t end = tag.find_first_of(" \t\r\n>", value);
        return tag.substr(value, end == std::string::npos ? std::string::npos : end - value);
    }
    return { };
}

// The "Netscape bookmark file" every browser can export (and Prism exports).
size_t importHTML(const std::string& html, Node& into)
{
    std::string lowered = lowerASCII(html);
    std::vector<Node*> stack { &into };
    std::wstring pendingFolder;
    bool hasPendingFolder = false;
    bool pendingIsToolbar = false;
    bool barWasEmpty = g_bar.children.empty();
    size_t count = 0;
    size_t at = 0;
    while ((at = lowered.find('<', at)) != std::string::npos) {
        size_t close = lowered.find('>', at);
        if (close == std::string::npos)
            break;
        std::string tag = html.substr(at, close - at + 1);
        std::string name = lowered.substr(at + 1, std::min<size_t>(4, close - at - 1));
        if (name.rfind("h3", 0) == 0) {
            size_t end = lowered.find("</h3", close);
            if (end == std::string::npos)
                break;
            pendingFolder = decodeEntities(html.substr(close + 1, end - close - 1));
            hasPendingFolder = true;
            pendingIsToolbar = lowerASCII(attribute(tag, "personal_toolbar_folder")) == "true";
            at = end;
            continue;
        }
        if (name.rfind("dl", 0) == 0) {
            if (hasPendingFolder) {
                Node* parent = stack.back();
                Node* folder;
                // Firefox/Prism's toolbar folder fills an empty bookmarks bar.
                if (pendingIsToolbar && barWasEmpty && stack.size() <= 2) {
                    folder = &g_bar;
                    barWasEmpty = false;
                } else
                    folder = addNode(*parent, true, pendingFolder, L"");
                stack.push_back(folder);
                hasPendingFolder = false;
            } else if (stack.size() == 1)
                stack.push_back(&into); // the file's outer list
            else
                stack.push_back(stack.back());
        } else if (name.rfind("/dl", 0) == 0) {
            if (stack.size() > 1)
                stack.pop_back();
        } else if (name.rfind("a", 0) == 0 && (name.size() < 2 || isspace(static_cast<unsigned char>(name[1])))) {
            size_t end = lowered.find("</a", close);
            if (end == std::string::npos)
                break;
            std::wstring url = decodeEntities(attribute(tag, "href"));
            std::wstring title = decodeEntities(html.substr(close + 1, end - close - 1));
            long long added = _atoi64(attribute(tag, "add_date").c_str()) * 1000;
            if (isAllowedURL(url)) {
                addNode(*stack.back(), false, title.empty() ? url : title, url, added);
                ++count;
            }
            at = end;
            continue;
        }
        at = close;
    }
    return count;
}

std::string escapeHTML(const std::wstring& text)
{
    std::string out;
    for (char c : util::toUTF8(text)) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

void exportNodes(const Node& folder, std::string& out, int depth)
{
    std::string indent(static_cast<size_t>(depth) * 4, ' ');
    for (auto& child : folder.children) {
        std::string added = std::to_string(child->added / 1000);
        if (child->folder) {
            out += indent + "<DT><H3 ADD_DATE=\"" + added + "\">" + escapeHTML(child->title) + "</H3>\n";
            out += indent + "<DL><p>\n";
            exportNodes(*child, out, depth + 1);
            out += indent + "</DL><p>\n";
        } else
            out += indent + "<DT><A HREF=\"" + escapeHTML(child->url) + "\" ADD_DATE=\"" + added + "\">" + escapeHTML(child->title) + "</A>\n";
    }
}

std::string exportHTML()
{
    std::string out = "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
        "<!-- Exported by Prism. -->\n"
        "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
        "<TITLE>Bookmarks</TITLE>\n<H1>Bookmarks</H1>\n<DL><p>\n";
    out += "    <DT><H3 PERSONAL_TOOLBAR_FOLDER=\"true\">Bookmarks bar</H3>\n    <DL><p>\n";
    exportNodes(g_bar, out, 2);
    out += "    </DL><p>\n";
    exportNodes(g_other, out, 1);
    out += "</DL><p>\n";
    return out;
}

void searchIn(const Node& node, const std::vector<std::wstring>& terms, std::vector<Match>& out, size_t limit)
{
    for (auto& child : node.children) {
        if (out.size() >= limit)
            return;
        if (child->folder) {
            searchIn(*child, terms, out, limit);
            continue;
        }
        std::wstring haystack = lower(child->url) + L" " + lower(child->title);
        bool all = true;
        for (auto& term : terms)
            all = all && haystack.find(term) != std::wstring::npos;
        if (all)
            out.push_back({ child->url, child->title });
    }
}

} // namespace

void load()
{
    if (g_loaded)
        return;
    g_loaded = true;
    g_bar.id = kBarId;
    g_bar.folder = true;
    g_bar.title = L"Bookmarks bar";
    g_other.id = kOtherId;
    g_other.folder = true;
    g_other.title = L"Other bookmarks";
    json::Value root;
    if (!json::parse(util::readFile(bookmarksFile()), root))
        return;
    g_nextId = std::max<long long>(3, static_cast<long long>(root["nextId"].asNumber(3)));
    readChildren(g_bar, root["bar"], 0);
    readChildren(g_other, root["other"], 0);
}

void setChangeListener(std::function<void()> listener)
{
    g_listener = std::move(listener);
}

const Node* find(long long id)
{
    load();
    return findMutable(id);
}

const Node* findURL(const std::wstring& url)
{
    load();
    if (url.empty())
        return nullptr;
    if (const Node* found = findURLIn(g_bar, url))
        return found;
    return findURLIn(g_other, url);
}

long long add(const std::wstring& url, const std::wstring& title, long long parentId)
{
    load();
    if (!isAllowedURL(url))
        return 0;
    Node* parent = findMutable(parentId);
    if (!parent || !parent->folder)
        parent = &g_bar;
    Node* node = addNode(*parent, false, title.empty() ? url : title, url);
    changed();
    return node->id;
}

void remove(long long id)
{
    load();
    Node* node = findMutable(id);
    if (!node || !node->parent)
        return; // the two top folders stay
    auto& siblings = node->parent->children;
    std::erase_if(siblings, [&](const std::unique_ptr<Node>& child) { return child.get() == node; });
    changed();
}

void rename(long long id, const std::wstring& title)
{
    load();
    Node* node = findMutable(id);
    if (!node || !node->parent || title.empty())
        return;
    node->title = cleanTitle(title);
    changed();
}

std::string barJSON()
{
    load();
    std::string json = "[";
    for (size_t i = 0; i < g_bar.children.size(); ++i) {
        auto& child = g_bar.children[i];
        json += (i ? "," : "") + std::string("{\"id\":") + std::to_string(child->id) + ",\"title\":" + util::jsonString(child->title)
            + ",\"url\":" + util::jsonString(child->url) + ",\"icon\":" + util::jsonString(child->folder ? L"" : icons::fileURLFor(child->url))
            + ",\"folder\":" + (child->folder ? "true" : "false") + "}";
    }
    return json + "]";
}

std::vector<Match> search(const std::wstring& text, size_t limit)
{
    load();
    std::vector<std::wstring> terms;
    std::wstring current;
    for (wchar_t c : lower(text)) {
        if (iswspace(c)) {
            if (!current.empty())
                terms.push_back(current);
            current.clear();
        } else
            current += c;
    }
    if (!current.empty())
        terms.push_back(current);
    std::vector<Match> out;
    if (terms.empty())
        return out;
    searchIn(g_bar, terms, out, limit);
    searchIn(g_other, terms, out, limit);
    return out;
}

std::string handleCommand(const std::wstring& command, const std::wstring& argument, HWND owner)
{
    load();
    json::Value request;
    json::parse(util::toUTF8(argument), request);

    if (command == L"tree")
        return "{\"bar\":" + nodeJSON(g_bar) + ",\"other\":" + nodeJSON(g_other) + "}";

    if (command == L"add") {
        // {"parent": id, "title": "...", "url": "..." (none for a folder), "index": n}
        Node* parent = findMutable(static_cast<long long>(request["parent"].asNumber(kOtherId)));
        if (!parent || !parent->folder)
            return "0";
        std::wstring url = util::fromUTF8(request["url"].asString());
        bool folder = !request.has("url");
        if (!folder && !isAllowedURL(url))
            return "0";
        std::wstring title = util::fromUTF8(request["title"].asString());
        Node* node = addNode(*parent, folder, title.empty() ? (folder ? L"New folder" : url) : title, url, 0,
            static_cast<int>(request["index"].asNumber(-1)));
        changed();
        return std::to_string(node->id);
    }

    if (command == L"update") {
        // {"id": n, "title": "...", "url": "..."}
        Node* node = findMutable(static_cast<long long>(request["id"].asNumber(0)));
        if (!node || !node->parent)
            return "false";
        if (request.has("title"))
            node->title = cleanTitle(util::fromUTF8(request["title"].asString()));
        if (request.has("url") && !node->folder) {
            std::wstring url = util::fromUTF8(request["url"].asString());
            if (!isAllowedURL(url))
                return "false";
            node->url = url;
        }
        changed();
        return "true";
    }

    if (command == L"remove") {
        remove(_wtoi64(argument.c_str()));
        return "true";
    }

    if (command == L"move") {
        // {"id": n, "parent": id, "index": n}
        Node* node = findMutable(static_cast<long long>(request["id"].asNumber(0)));
        Node* parent = findMutable(static_cast<long long>(request["parent"].asNumber(0)));
        if (!node || !node->parent || !parent || !parent->folder || isInside(parent, node))
            return "false";
        int index = static_cast<int>(request["index"].asNumber(-1));
        auto& from = node->parent->children;
        auto it = std::find_if(from.begin(), from.end(), [&](const std::unique_ptr<Node>& child) { return child.get() == node; });
        // Moving later in the same folder: the index counts the item itself.
        int oldIndex = static_cast<int>(it - from.begin());
        std::unique_ptr<Node> owned = std::move(*it);
        from.erase(it);
        if (parent == node->parent && index > oldIndex)
            --index;
        owned->parent = parent;
        if (index < 0 || index > static_cast<int>(parent->children.size()))
            parent->children.push_back(std::move(owned));
        else
            parent->children.insert(parent->children.begin() + index, std::move(owned));
        changed();
        return "true";
    }

    if (command == L"browsers") {
        std::string json = "[";
        bool first = true;
        for (auto& browser : kBrowsers) {
            if (!fileExists(browserFile(browser)))
                continue;
            json += (first ? "" : ",") + std::string("{\"id\":\"") + browser.id + "\",\"name\":" + util::jsonString(browser.name) + "}";
            first = false;
        }
        return json + "]";
    }

    if (command == L"importBrowser")
        return importBrowser(util::toUTF8(argument));

    if (command == L"importFile") {
        auto path = dialogs::openFile(owner, L"Import bookmarks", L"Bookmark files (*.html)", L"*.html;*.htm");
        if (!path)
            return "{\"cancelled\":true}";
        std::string html = util::readFile(*path);
        Node* folder = addNode(g_other, true, L"Imported", L"");
        size_t count = importHTML(html, *folder);
        if (folder->children.empty())
            remove(folder->id);
        changed();
        return "{\"count\":" + std::to_string(count) + "}";
    }

    if (command == L"exportFile") {
        auto path = dialogs::saveFile(owner, L"Export bookmarks", L"bookmarks.html", L"Bookmark files (*.html)", L"*.html");
        if (!path)
            return "{\"cancelled\":true}";
        bool ok = util::writeFileAtomically(*path, exportHTML());
        return ok ? "{\"count\":" + std::to_string(countBookmarks(g_bar) + countBookmarks(g_other)) + "}" : "{\"error\":\"Couldn't save the file.\"}";
    }

    return "null";
}

} // namespace bookmarks
