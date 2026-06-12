#include "app/goto_dialog.h"
#include <string>

namespace fastpad {

// Control ID for the EDIT. Must stay clear of the button command IDs
// (IDOK=1, IDCANCEL=2): an EDIT sends EN_SETFOCUS / EN_CHANGE notifications to
// its parent as WM_COMMAND, and dispatch keys only on LOWORD (the control id).
// If the edit's id were IDOK, focusing it would look like "Go" and close the
// dialog instantly.
static constexpr int IDC_GOTO_EDIT = 1000;

struct GotoState {
    uint64_t value = 0;
    bool isLine = true;
    bool ok = false;
    bool done = false;
    HWND edit = nullptr;
};

static LRESULT CALLBACK gotoProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    auto* st = (GotoState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_CREATE: {
        st = (GotoState*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
        CreateWindowExW(0, L"STATIC", L"Line number, or :byte-offset", WS_CHILD | WS_VISIBLE,
            10, 10, 260, 18, h, nullptr, nullptr, nullptr);
        st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            10, 32, 260, 22, h, (HMENU)(INT_PTR)IDC_GOTO_EDIT, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Go", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            115, 62, 70, 24, h, (HMENU)IDOK, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            195, 62, 70, 24, h, (HMENU)IDCANCEL, nullptr, nullptr);
        SetFocus(st->edit);
        return 0;
    }
    case WM_COMMAND:
        if (st && LOWORD(wp) == IDOK) {
            wchar_t buf[64] = L"";
            GetWindowTextW(st->edit, buf, 64);
            std::wstring s = buf;
            st->isLine = true;
            size_t start = 0;
            if (!s.empty() && s[0] == L':') { st->isLine = false; start = 1; }
            st->value = wcstoull(s.c_str() + start, nullptr, 0);
            st->ok = true;
            DestroyWindow(h);
        } else if (st && LOWORD(wp) == IDCANCEL) DestroyWindow(h);
        return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: if (st) st->done = true; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

bool show_goto_dialog(HWND owner, uint64_t* value, bool* isLine) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = gotoProc;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"FastPadGoto";
        RegisterClassW(&wc);
        registered = true;
    }
    GotoState st;
    RECT orc; GetWindowRect(owner, &orc);
    EnableWindow(owner, FALSE);
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"FastPadGoto", L"Go to",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, orc.left + 80, orc.top + 80, 296, 130,
        owner, nullptr, nullptr, &st);
    ShowWindow(h, SW_SHOW);
    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { DestroyWindow(h); continue; }
        if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (st.ok) { *value = st.value; *isLine = st.isLine; }
    return st.ok;
}

} // namespace fastpad
