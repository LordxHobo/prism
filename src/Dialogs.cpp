// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Dialogs.h"

namespace dialogs {

namespace {

constexpr int kEditId = 100;

struct PromptState {
    HWND edit { };
    bool done { false };
    bool accepted { false };
    std::wstring result;
    HFONT font { };
};

LRESULT CALLBACK promptProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        break;
    }
    case WM_COMMAND:
        if (!state)
            break;
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            state->accepted = LOWORD(wParam) == IDOK;
            if (state->accepted) {
                int length = GetWindowTextLengthW(state->edit);
                state->result.assign(length, L'\0');
                GetWindowTextW(state->edit, state->result.data(), length + 1);
            }
            state->done = true;
            return 0;
        }
        break;
    case WM_CLOSE:
        if (state) {
            state->accepted = false;
            state->done = true;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

std::optional<std::wstring> prompt(HWND owner, const std::wstring& title, const std::wstring& message, const std::wstring& defaultValue)
{
    static bool registered = false;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = promptProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = L"PrismPromptDialog";
        RegisterClassExW(&wc);
        registered = true;
    }

    UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };

    PromptState state;
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    state.font = CreateFontIndirectW(&metrics.lfMessageFont);

    RECT ownerRect = { 0, 0, px(800), px(600) };
    if (owner)
        GetWindowRect(owner, &ownerRect);
    int width = px(420), height = px(190);
    int x = ownerRect.left + (ownerRect.right - ownerRect.left - width) / 2;
    int y = ownerRect.top + (ownerRect.bottom - ownerRect.top - height) / 3;

    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"PrismPromptDialog", title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner, nullptr, instance, &state);
    if (!dialog) {
        DeleteObject(state.font);
        return std::nullopt;
    }

    RECT client;
    GetClientRect(dialog, &client);
    int margin = px(12);
    int innerWidth = client.right - 2 * margin;
    int buttonWidth = px(80), buttonHeight = px(26);

    HWND label = CreateWindowExW(0, L"STATIC", message.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        margin, margin, innerWidth, px(54), dialog, nullptr, instance, nullptr);
    state.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", defaultValue.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        margin, margin + px(60), innerWidth, px(24), dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)), instance, nullptr);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        client.right - margin - 2 * buttonWidth - px(8), client.bottom - margin - buttonHeight, buttonWidth, buttonHeight,
        dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), instance, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        client.right - margin - buttonWidth, client.bottom - margin - buttonHeight, buttonWidth, buttonHeight,
        dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), instance, nullptr);
    for (HWND child : { label, state.edit, ok, cancel })
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);

    if (owner)
        EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetFocus(state.edit);
    SendMessageW(state.edit, EM_SETSEL, 0, -1);

    MSG msg;
    while (!state.done) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            // The app is quitting: hand WM_QUIT back to the main loop.
            if (!got)
                PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (IsDialogMessageW(dialog, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    DestroyWindow(dialog);
    DeleteObject(state.font);

    if (!state.accepted)
        return std::nullopt;
    return state.result;
}

} // namespace dialogs
