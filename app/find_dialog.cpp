#include "app/find_dialog.h"
#include <string>

namespace fastpad {

// Button IDs: IDOK = Find Next, IDRETRY = Find Prev, IDCANCEL = Cancel.
// (IDRETRY is a standard Windows button id = 4, chosen here for Find Prev so
// the dialog can return the direction without a custom id.)
static constexpr int IDC_FINDPREV = IDRETRY;   // 4

// Control IDs for the EDIT and checkbox. These MUST stay clear of the button
// command IDs (IDOK=1, IDCANCEL=2, IDRETRY=4): an EDIT sends EN_SETFOCUS /
// EN_CHANGE notifications to its parent as WM_COMMAND, and WM_COMMAND dispatch
// keys only on LOWORD (the control id). If the edit's id were IDOK, focusing
// it would look like "Find Next" and close the dialog instantly.
static constexpr int IDC_FIND_EDIT = 1000;
static constexpr int IDC_FIND_CASE = 1001;

struct FindState {
    FindResult result{};
    bool done = false;
    HWND edit = nullptr;
    HWND chkCase = nullptr;
};

static LRESULT CALLBACK findProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    auto* st = (FindState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_CREATE: {
        st = (FindState*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
        // Label
        CreateWindowExW(0, L"STATIC", L"Find:", WS_CHILD | WS_VISIBLE,
            10, 10, 40, 18, h, nullptr, nullptr, nullptr);
        // Edit field
        st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            st->result.needle.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            54, 8, 226, 22, h, (HMENU)(INT_PTR)IDC_FIND_EDIT, nullptr, nullptr);
        // Match case checkbox
        st->chkCase = CreateWindowExW(0, L"BUTTON", L"Match &case",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            10, 38, 120, 18, h, (HMENU)(INT_PTR)IDC_FIND_CASE, nullptr, nullptr);
        SendMessageW(st->chkCase, BM_SETCHECK,
            st->result.matchCase ? BST_CHECKED : BST_UNCHECKED, 0);
        // Find Next button (default)
        CreateWindowExW(0, L"BUTTON", L"Find &Next",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            10, 64, 90, 24, h, (HMENU)(INT_PTR)IDOK, nullptr, nullptr);
        // Find Prev button
        CreateWindowExW(0, L"BUTTON", L"Find &Prev",
            WS_CHILD | WS_VISIBLE,
            108, 64, 90, 24, h, (HMENU)(INT_PTR)IDC_FINDPREV, nullptr, nullptr);
        // Cancel button
        CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE,
            206, 64, 74, 24, h, (HMENU)(INT_PTR)IDCANCEL, nullptr, nullptr);
        SetFocus(st->edit);
        // Move caret to end of pre-filled text
        SendMessageW(st->edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        return 0;
    }
    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        if (st && (id == IDOK || id == IDC_FINDPREV)) {
            wchar_t buf[1024] = L"";
            GetWindowTextW(st->edit, buf, 1024);
            st->result.needle = buf;
            st->result.matchCase =
                (SendMessageW(st->chkCase, BM_GETCHECK, 0, 0) == BST_CHECKED);
            st->result.forward = (id == IDOK);
            st->result.ok = true;
            DestroyWindow(h);
        } else if (st && id == IDCANCEL) {
            DestroyWindow(h);
        }
        return 0;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: if (st) st->done = true; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

FindResult show_find_dialog(HWND owner, const std::wstring& initialNeedle,
                            bool initialMatchCase) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = findProc;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"FastPadFind";
        RegisterClassW(&wc);
        registered = true;
    }

    FindState st;
    st.result.needle = initialNeedle;
    st.result.matchCase = initialMatchCase;

    RECT orc; GetWindowRect(owner, &orc);
    EnableWindow(owner, FALSE);
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"FastPadFind", L"Find",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        orc.left + 80, orc.top + 80, 300, 130,
        owner, nullptr, nullptr, &st);
    ShowWindow(h, SW_SHOW);

    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            DestroyWindow(h);
            continue;
        }
        if (!IsDialogMessageW(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return st.result;
}

} // namespace fastpad
