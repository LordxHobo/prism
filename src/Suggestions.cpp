// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Suggestions.h"

#include "Bookmarks.h"
#include "History.h"
#include "Util.h"

#include <algorithm>
#include <dwmapi.h>
#include <windowsx.h>
#include <set>

namespace suggestions {

namespace {

constexpr int kRowHeight = 34;    // CSS pixels
constexpr int kPadding = 6;
constexpr size_t kMaxItems = 8;
constexpr wchar_t kClassName[] = L"PrismSuggestions";

HWND g_window = nullptr;
std::vector<Item> g_items;
int g_selected = -1;
int g_hovered = -1;
Style g_style;
std::function<void(const Item&)> g_pick;
HFONT g_font = nullptr;
HFONT g_iconFont = nullptr;
double g_fontScale = 0;

int px(double css)
{
    return static_cast<int>(css * g_style.scale + 0.5);
}

COLORREF mix(COLORREF a, COLORREF b, double amount)
{
    auto channel = [&](int x, int y) { return static_cast<int>(x + (y - x) * amount + 0.5); };
    return RGB(channel(GetRValue(a), GetRValue(b)), channel(GetGValue(a), GetGValue(b)), channel(GetBValue(a), GetBValue(b)));
}

void ensureFonts()
{
    if (g_font && g_fontScale == g_style.scale)
        return;
    if (g_font)
        DeleteObject(g_font);
    if (g_iconFont)
        DeleteObject(g_iconFont);
    g_fontScale = g_style.scale;
    g_font = CreateFontW(-px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    // Windows' own icon font (Windows 10 and 11).
    g_iconFont = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
}

const wchar_t* iconFor(const std::wstring& kind)
{
    if (kind == L"search") return L"\xE721";   // magnifier
    if (kind == L"bookmark") return L"\xE734"; // star
    if (kind == L"history") return L"\xE81C";  // clock
    return L"\xE774";                           // globe
}

int rowAt(int y)
{
    int row = (y - px(kPadding)) / px(kRowHeight);
    return y >= px(kPadding) && row >= 0 && row < static_cast<int>(g_items.size()) ? row : -1;
}

void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC target = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

    COLORREF background = g_style.dark ? RGB(0x26, 0x26, 0x2c) : RGB(0xff, 0xff, 0xff);
    COLORREF text = g_style.dark ? RGB(0xec, 0xec, 0xf1) : RGB(0x1a, 0x1a, 0x22);
    COLORREF dim = g_style.dark ? RGB(0x9a, 0x9a, 0xa8) : RGB(0x5d, 0x5d, 0x6b);
    COLORREF hover = g_style.dark ? RGB(0x32, 0x32, 0x3a) : RGB(0xf0, 0xf0, 0xf4);
    COLORREF selected = mix(background, g_style.accent, g_style.dark ? 0.35 : 0.18);

    HBRUSH fill = CreateSolidBrush(background);
    FillRect(dc, &client, fill);
    DeleteObject(fill);
    ensureFonts();
    SetBkMode(dc, TRANSPARENT);

    for (size_t i = 0; i < g_items.size(); ++i) {
        const Item& item = g_items[i];
        RECT row = { px(4), px(kPadding) + static_cast<int>(i) * px(kRowHeight), client.right - px(4), px(kPadding) + static_cast<int>(i + 1) * px(kRowHeight) };
        int index = static_cast<int>(i);
        if (index == g_selected || index == g_hovered) {
            HBRUSH rowBrush = CreateSolidBrush(index == g_selected ? selected : hover);
            HGDIOBJ oldBrush = SelectObject(dc, rowBrush);
            HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, row.left, row.top, row.right + 1, row.bottom + 1, px(12), px(12));
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(rowBrush);
        }
        RECT icon = { row.left + px(8), row.top, row.left + px(32), row.bottom };
        SelectObject(dc, g_iconFont);
        SetTextColor(dc, item.kind == L"bookmark" ? g_style.accent : dim);
        DrawTextW(dc, iconFor(item.kind), -1, &icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        SelectObject(dc, g_font);
        RECT label = { row.left + px(40), row.top, row.right - px(10), row.bottom };
        // The title takes up to 60% of the row; the address gets the rest.
        SIZE titleSize = { };
        GetTextExtentPoint32W(dc, item.title.c_str(), static_cast<int>(item.title.size()), &titleSize);
        int titleWidth = std::min<int>(titleSize.cx, (label.right - label.left) * (item.detail.empty() ? 100 : 60) / 100);
        RECT titleRect = { label.left, label.top, label.left + titleWidth, label.bottom };
        SetTextColor(dc, text);
        DrawTextW(dc, item.title.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (!item.detail.empty()) {
            RECT detailRect = { titleRect.right + px(10), label.top, label.right, label.bottom };
            std::wstring detail = L"\x2014  " + item.detail;
            SetTextColor(dc, dim);
            DrawTextW(dc, detail.c_str(), -1, &detailRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }

    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_MOUSEACTIVATE:
        // Clicking the list must not take the focus from the address bar.
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        int row = rowAt(GET_Y_LPARAM(lParam));
        if (row != g_hovered) {
            g_hovered = row;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hovered = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int row = rowAt(GET_Y_LPARAM(lParam));
        if (row >= 0 && g_pick) {
            Item item = g_items[static_cast<size_t>(row)];
            auto pick = g_pick;
            hide();
            pick(item);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void ensureWindow(HWND owner)
{
    if (g_window && GetWindow(g_window, GW_OWNER) == owner)
        return;
    if (g_window)
        DestroyWindow(g_window);
    static bool registered = false;
    if (!registered) {
        WNDCLASSW windowClass = { };
        windowClass.style = CS_DROPSHADOW;
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kClassName;
        RegisterClassW(&windowClass);
        registered = true;
    }
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"", WS_POPUP,
        0, 0, 0, 0, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    // Rounded corners on Windows 11 (ignored on Windows 10).
    DWORD corners = 3; // DWMWCP_ROUNDSMALL
    DwmSetWindowAttribute(g_window, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));
}

std::wstring withoutScheme(const std::wstring& url)
{
    size_t scheme = url.find(L"://");
    std::wstring text = scheme == std::wstring::npos ? url : url.substr(scheme + 3);
    if (text.rfind(L"www.", 0) == 0)
        text = text.substr(4);
    if (!text.empty() && text.back() == L'/' && std::count(text.begin(), text.end(), L'/') == 1)
        text.pop_back();
    return text;
}

} // namespace

std::vector<Item> build(const std::wstring& text, const std::wstring& go, bool isSearch, const std::wstring& searchName)
{
    std::vector<Item> items;
    if (text.empty() || go.empty())
        return items;
    if (isSearch)
        items.push_back({ L"search", text, go, L"Search with " + searchName });
    else
        items.push_back({ L"url", withoutScheme(go), go, L"Go to this address" });

    std::set<std::wstring> seen { go };
    for (auto& match : bookmarks::search(text, 3)) {
        if (!seen.insert(match.url).second)
            continue;
        items.push_back({ L"bookmark", match.title.empty() ? withoutScheme(match.url) : match.title, match.url, withoutScheme(match.url) });
    }
    for (auto& match : history::search(text, kMaxItems)) {
        if (items.size() >= kMaxItems)
            break;
        if (!seen.insert(match.url).second)
            continue;
        items.push_back({ L"history", match.title.empty() ? withoutScheme(match.url) : match.title, match.url, withoutScheme(match.url) });
    }
    return items;
}

void show(HWND owner, const RECT& anchor, const Style& style, std::vector<Item> items, std::function<void(const Item&)> pick)
{
    // Just the "search for"/"go to" line isn't worth a list.
    if (items.size() < 2) {
        hide();
        return;
    }
    ensureWindow(owner);
    g_style = style;
    g_items = std::move(items);
    g_pick = std::move(pick);
    g_selected = -1;
    g_hovered = -1;
    int width = anchor.right - anchor.left;
    int height = px(kPadding * 2) + static_cast<int>(g_items.size()) * px(kRowHeight);
    SetWindowPos(g_window, HWND_TOP, anchor.left, anchor.bottom, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_window, nullptr, FALSE);
}

void select(int index)
{
    if (!isVisible())
        return;
    g_selected = index >= 0 && index < static_cast<int>(g_items.size()) ? index : -1;
    InvalidateRect(g_window, nullptr, FALSE);
}

void hide()
{
    if (g_window)
        ShowWindow(g_window, SW_HIDE);
    g_items.clear();
    g_pick = nullptr;
}

bool isVisible()
{
    return g_window && IsWindowVisible(g_window);
}

std::string itemsJSON(const std::vector<Item>& items)
{
    std::string json = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        json += (i ? "," : "") + std::string("{\"kind\":") + util::jsonString(items[i].kind) + ",\"title\":" + util::jsonString(items[i].title)
            + ",\"url\":" + util::jsonString(items[i].url) + "}";
    }
    return json + "]";
}

} // namespace suggestions
