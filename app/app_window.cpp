#include "app/app_window.h"
#include "app/commands.h"
#include "app/find_dialog.h"
#include "app/goto_dialog.h"
#include "app/registration.h"
#include "app/renderer.h"
#include "app/resource.h"
#include "app/settings.h"
#include "core/encoding.h"
#include "core/save.h"
#include "core/search.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <commctrl.h>
#include <commdlg.h>
#include <string>
#include <vector>
#include <windowsx.h>

namespace fastpad {

// Define destructor in this TU where Renderer is complete.
AppWindow::~AppWindow() = default;

// Copy refuses selections larger than this rather than ballooning RAM with a
// decoded gigabyte: clipboard-sized data should go through Save As instead.
static constexpr uint64_t kMaxClipboardBytes = 256ull << 20;   // 256 MB

static std::vector<std::pair<UINT, std::wstring>> g_codepages;

// View > Font size entries; menu IDs are IDM_VIEW_FONTSIZE_BASE + index.
static constexpr int kFontSizes[] = { 8, 10, 12, 14, 16, 18, 20, 24, 28, 36 };

AppWindow* AppWindow::create(HINSTANCE inst, int showCmd, const wchar_t* fileArg) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    // EX variant so the class carries the app icon (title bar + Alt-Tab).
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // fully covered by the view + status children
    wc.lpszClassName = L"FastPadMain";
    RegisterClassExW(&wc);

    // The text view lives in a CHILD window so its scrollbars sit on the view's
    // edges - ABOVE the status bar - instead of on the top-level window frame,
    // where Windows would draw the horizontal bar BELOW the status bar.
    WNDCLASSW vc{};
    vc.lpfnWndProc = viewProc;
    vc.hInstance = inst;
    vc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
    vc.hbrBackground = nullptr;   // Direct2D paints the entire view client area
    vc.lpszClassName = L"FastPadView";
    RegisterClassW(&vc);

    auto* self = new AppWindow();
    self->inst_ = inst;
    // WS_CLIPCHILDREN: without it every main-window paint covers the full
    // client rect INCLUDING the area under the children, which then repaint
    // on top of the white -> visible blink on every frame.
    HWND h = CreateWindowExW(0, L"FastPadMain", L"FastPad",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 750, nullptr, nullptr, inst, self);
    if (!h) { delete self; return nullptr; }
    ShowWindow(h, showCmd);
    if (fileArg && *fileArg) self->openPath(fileArg);
    return self;
}

LRESULT CALLBACK AppWindow::wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    AppWindow* self;
    if (m == WM_NCCREATE) {
        self = (AppWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (AppWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    }
    return self ? self->handle(m, wp, lp) : DefWindowProcW(h, m, wp, lp);
}

LRESULT CALLBACK AppWindow::viewProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    AppWindow* self;
    if (m == WM_NCCREATE) {
        self = (AppWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
        self->view_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (AppWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    }
    return self ? self->handleView(m, wp, lp) : DefWindowProcW(h, m, wp, lp);
}

void AppWindow::buildMenu() {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_SAVE, L"&Save\tCtrl+S");
    AppendMenuW(file, MF_STRING, IDM_SAVEAS, L"Save &As...\tCtrl+Shift+S");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, IDM_UNDO, L"&Undo\tCtrl+Z");
    AppendMenuW(edit, MF_STRING, IDM_REDO, L"&Redo\tCtrl+Y");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, IDM_CUT, L"Cu&t\tCtrl+X");
    AppendMenuW(edit, MF_STRING, IDM_COPY, L"&Copy\tCtrl+C");
    AppendMenuW(edit, MF_STRING, IDM_PASTE, L"&Paste\tCtrl+V");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, IDM_SELECTALL, L"Select &All\tCtrl+A");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, IDM_FIND, L"&Find...\tCtrl+F");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, IDM_GOTO, L"&Go to...\tCtrl+G");
    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING | (settings_.wordWrap ? MF_CHECKED : MF_UNCHECKED),
        IDM_VIEW_WRAP, L"&Word wrap");
    HMENU fontSize = CreatePopupMenu();
    for (size_t i = 0; i < _countof(kFontSizes); ++i) {
        wchar_t label[8];
        swprintf_s(label, L"%d", kFontSizes[i]);
        AppendMenuW(fontSize, MF_STRING, IDM_VIEW_FONTSIZE_BASE + (UINT)i, label);
    }
    AppendMenuW(view, MF_POPUP, (UINT_PTR)fontSize, L"Font size");
    HMENU enc = CreatePopupMenu();
    buildEncodingMenu(enc);
    HMENU help = CreatePopupMenu();
    // Both registration items always exist; WM_INITMENUPOPUP grays the one
    // that does not apply to the current registration state.
    AppendMenuW(help, MF_STRING, IDM_REGISTER, L"&Register in Open With");
    AppendMenuW(help, MF_STRING, IDM_UNREGISTER, L"&Unregister from Open With");
    AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About FastPad");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"&Edit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&View");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)enc, L"E&ncoding");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
    SetMenu(hwnd_, bar);
}

void AppWindow::buildEncodingMenu(HMENU enc) {
    AppendMenuW(enc, MF_STRING, IDM_ENC_UTF8, L"Reopen as UTF-8");
    AppendMenuW(enc, MF_STRING, IDM_ENC_UTF16LE, L"Reopen as UTF-16 LE");
    AppendMenuW(enc, MF_STRING, IDM_ENC_UTF16BE, L"Reopen as UTF-16 BE");
    AppendMenuW(enc, MF_SEPARATOR, 0, nullptr);
    HMENU cps = CreatePopupMenu();
    g_codepages = list_codepages();
    for (size_t i = 0; i < g_codepages.size(); ++i)
        AppendMenuW(cps, MF_STRING, IDM_ENC_CP_BASE + (UINT)i, g_codepages[i].second.c_str());
    AppendMenuW(enc, MF_POPUP, (UINT_PTR)cps, L"Reopen with codepage");
    AppendMenuW(enc, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(enc, MF_STRING, IDM_SAVEAS_UTF8, L"Save As UTF-8...");
    AppendMenuW(enc, MF_STRING, IDM_SAVEAS_UTF16LE, L"Save As UTF-16 LE...");
    AppendMenuW(enc, MF_STRING, IDM_SAVEAS_LF, L"Save with &LF endings...");
    AppendMenuW(enc, MF_STRING, IDM_SAVEAS_CRLF, L"Save with C&RLF endings...");
}

void AppWindow::onOpenDialog() {
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"All files\0*.*\0Text files\0*.txt;*.log;*.csv;*.md;*.json;*.xml\0";
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) openPath(buf);
}

void AppWindow::openPath(const wchar_t* path) {
    LARGE_INTEGER f0, t0, t1;
    QueryPerformanceFrequency(&f0);
    QueryPerformanceCounter(&t0);
    auto d = std::make_unique<Document>();
    std::wstring err;
    bool ok = d->open(path, &err);
    QueryPerformanceCounter(&t1);
    double openMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f0.QuadPart;
    wchar_t dbg[160];
    swprintf_s(dbg, L"FastPad: open took %.2f ms\n", openMs);
    OutputDebugStringW(dbg);
    if (!ok) {
        MessageBoxW(hwnd_, (L"Cannot open file:\n" + err).c_str(), L"FastPad", MB_ICONERROR);
        return;
    }
    doc_ = std::move(d);
    // Rebind the renderer immediately: never leave it pointing at the freed
    // previous Document (M2-BACKLOG item 1).
    if (renderer_) renderer_->setDocument(doc_.get());
    wchar_t msStr[32];
    swprintf_s(msStr, L"%.1f", openMs);
    titleBase_ = std::wstring(path) + L" - FastPad (opened in " + msStr + L" ms)";
    SetWindowTextW(hwnd_, titleBase_.c_str());
    if (indexTimer_) KillTimer(hwnd_, indexTimer_);
    indexTimer_ = SetTimer(hwnd_, 1, 250, nullptr);
    updateStatusBar();
    InvalidateRect(view_, nullptr, FALSE);   // never bErase: GDI white-flash, D2D repaints fully
}

// ---- save flow --------------------------------------------------------------

// Documents at or below this run save_document inline with no progress UI;
// raw streaming at this size finishes well under a second.
static constexpr uint64_t kInlineSaveLimit = 64ull << 20;   // 64 MB

// Modal progress popup, modeled on goto_dialog's owner-disabled pattern. The
// save engine is synchronous: the popup exists for the whole save_document
// call and the progress callback both updates the bar and pumps messages so
// the Cancel button (-> the atomic) stays live.
struct SaveProgressState {
    std::atomic<bool> cancel{ false };
    HWND label = nullptr;
    HWND bar = nullptr;
};

static LRESULT CALLBACK saveProgressProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    auto* st = (SaveProgressState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_CREATE: {
        st = (SaveProgressState*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
        st->label = CreateWindowExW(0, L"STATIC", L"Saving... 0%", WS_CHILD | WS_VISIBLE,
            10, 10, 260, 18, h, nullptr, nullptr, nullptr);
        st->bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE,
            10, 32, 260, 18, h, nullptr, nullptr, nullptr);
        SendMessageW(st->bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            195, 60, 75, 24, h, (HMENU)IDCANCEL, nullptr, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (st && LOWORD(wp) == IDCANCEL) st->cancel.store(true);
        return 0;
    case WM_CLOSE:                       // titlebar X acts as Cancel; saveFlow destroys the window
        if (st) st->cancel.store(true);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

bool AppWindow::saveFlow(const std::wstring& targetPath, std::optional<EncodingInfo> transcode,
                         std::optional<std::wstring> forceEol) {
    if (!doc_ || saving_) return false;
    saving_ = true;
    struct SavingGuard { bool& f; ~SavingGuard() { f = false; } } guard{ saving_ };

    SaveOptions opts;
    opts.targetPath = targetPath;
    opts.transcodeTo = transcode;
    opts.forceEol = std::move(forceEol);
    std::wstring err;
    bool ok;
    SaveProgressState st;

    if (doc_->size() <= kInlineSaveLimit) {
        ok = save_document(*doc_, opts, nullptr, st.cancel, &err);
    } else {
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = saveProgressProc;
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = L"FastPadSaveProgress";
            RegisterClassW(&wc);
            registered = true;
        }
        RECT orc; GetWindowRect(hwnd_, &orc);
        EnableWindow(hwnd_, FALSE);      // owner disabled for the modal save
        HWND pop = CreateWindowExW(WS_EX_DLGMODALFRAME, L"FastPadSaveProgress", L"FastPad",
            WS_POPUP | WS_CAPTION | WS_SYSMENU, orc.left + 80, orc.top + 80, 296, 130,
            hwnd_, nullptr, nullptr, &st);
        ShowWindow(pop, SW_SHOW);
        auto onProgress = [&st](uint64_t done, uint64_t total) {
            const int pct = total ? (int)(done * 100 / total) : 100;
            SendMessageW(st.bar, PBM_SETPOS, (WPARAM)pct, 0);
            wchar_t txt[48];
            swprintf_s(txt, L"Saving... %d%%", pct);
            SetWindowTextW(st.label, txt);
            MSG m;                       // keep Cancel (and paint) live during the synchronous save
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) { st.cancel.store(true); continue; }
                TranslateMessage(&m); DispatchMessageW(&m);
            }
        };
        ok = save_document(*doc_, opts, onProgress, st.cancel, &err);
        DestroyWindow(pop);
        EnableWindow(hwnd_, TRUE);
        SetForegroundWindow(hwnd_);
    }

    if (!ok) {
        if (!st.cancel.load())           // cancel is the user's own action - no error box
            MessageBoxW(hwnd_, (L"Save failed:\n" + err).c_str(), L"FastPad", MB_ICONERROR);
        return false;
    }

    // Reopen the saved file as the new pristine original (encoding re-detected,
    // so a transcode save lands with its new encoding naturally). The renderer
    // rebind resets the caret to the document start - accepted M2 limitation.
    const std::wstring saved = targetPath.empty() ? doc_->path() : targetPath;
    if (!doc_->reopenAfterSave(saved.c_str(), &err)) {
        MessageBoxW(hwnd_, (L"Saved, but reopening the file failed:\n" + err).c_str(),
                    L"FastPad", MB_ICONERROR);
        doc_.reset();
        if (renderer_) renderer_->setDocument(nullptr);
        if (indexTimer_) { KillTimer(hwnd_, indexTimer_); indexTimer_ = 0; }
        titleBase_ = L"FastPad";
        SetWindowTextW(hwnd_, titleBase_.c_str());
        updateStatusBar();
        InvalidateRect(view_, nullptr, FALSE);
        return false;                    // the file on disk is good, but we lost the view
    }
    if (renderer_) renderer_->setDocument(doc_.get());
    if (indexTimer_) KillTimer(hwnd_, indexTimer_);
    indexTimer_ = SetTimer(hwnd_, 1, 250, nullptr);
    titleBase_ = saved + L" - FastPad";
    SetWindowTextW(hwnd_, titleBase_.c_str());
    updateTitleDirty();                  // clean - drops the '*'
    updateStatusBar();
    InvalidateRect(view_, nullptr, FALSE);
    return true;
}

void AppWindow::onSaveAsDialog(std::optional<EncodingInfo> transcode,
                               std::optional<std::wstring> forceEol) {
    if (!doc_ || saving_) return;
    wchar_t buf[MAX_PATH] = L"";
    wcsncpy_s(buf, doc_->path().c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"All files\0*.*\0Text files\0*.txt;*.log;*.csv;*.md;*.json;*.xml\0";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameW(&ofn)) saveFlow(buf, transcode, std::move(forceEol));
}

// ---- find ---------------------------------------------------------------

// Documents at or below this limit run the search inline.
static constexpr uint64_t kInlineSearchLimit = 64ull << 20;   // 64 MB

void AppWindow::doFind(bool forward) {
    if (!doc_ || !renderer_) return;

    // Empty needle: open the dialog instead of searching
    if (findNeedle_.empty()) {
        FindResult fr = show_find_dialog(hwnd_, findNeedle_, findMatchCase_);
        if (!fr.ok) return;
        findNeedle_ = fr.needle;
        findMatchCase_ = fr.matchCase;
        forward = fr.forward;
        if (findNeedle_.empty()) return;
    }

    // Encode needle to compute match byte length
    const std::vector<uint8_t> needleBytes =
        encode_text(doc_->encoding(), findNeedle_);
    if (needleBytes.empty()) return;
    const uint64_t matchByteLen = (uint64_t)needleBytes.size();

    // Determine start offset
    uint64_t startOff;
    if (forward) {
        startOff = renderer_->hasSelection() ? renderer_->selEnd() : renderer_->caret();
    } else {
        startOff = renderer_->hasSelection() ? renderer_->selBegin() : renderer_->caret();
    }

    SearchOptions opts;
    opts.caseSensitive = findMatchCase_;
    opts.forward = forward;

    // Search function: inline or background. Sets `cancelled` when the user
    // aborts the background progress popup; a cancelled scan must not be
    // confused with a genuine miss (no wrap retry, no "Cannot find" box).
    auto runSearch = [&](uint64_t from, bool& cancelled) -> std::optional<uint64_t> {
        cancelled = false;
        if (doc_->size() <= kInlineSearchLimit) {
            std::atomic<bool> noCancel{ false };
            return search_document(*doc_, findNeedle_, from, opts, nullptr, noCancel);
        }
        // Background search with progress popup
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = saveProgressProc;
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = L"FastPadSearchProgress";
            RegisterClassW(&wc);
            registered = true;
        }
        SaveProgressState st;
        RECT orc; GetWindowRect(hwnd_, &orc);
        EnableWindow(hwnd_, FALSE);
        HWND pop = CreateWindowExW(WS_EX_DLGMODALFRAME, L"FastPadSearchProgress",
            L"FastPad",
            WS_POPUP | WS_CAPTION | WS_SYSMENU,
            orc.left + 80, orc.top + 80, 296, 130,
            hwnd_, nullptr, nullptr, &st);
        ShowWindow(pop, SW_SHOW);
        auto onProgress = [&st](uint64_t done, uint64_t total) {
            const int pct = total ? (int)(done * 100 / total) : 100;
            SendMessageW(st.bar, PBM_SETPOS, (WPARAM)pct, 0);
            wchar_t txt[48];
            swprintf_s(txt, L"Searching... %d%%", pct);
            SetWindowTextW(st.label, txt);
            MSG mm;
            while (PeekMessageW(&mm, nullptr, 0, 0, PM_REMOVE)) {
                if (mm.message == WM_KEYDOWN && mm.wParam == VK_ESCAPE) {
                    st.cancel.store(true); continue;
                }
                TranslateMessage(&mm); DispatchMessageW(&mm);
            }
        };
        std::optional<uint64_t> result =
            search_document(*doc_, findNeedle_, from, opts, onProgress, st.cancel);
        DestroyWindow(pop);
        EnableWindow(hwnd_, TRUE);
        SetForegroundWindow(hwnd_);
        if (st.cancel.load()) { cancelled = true; return std::nullopt; }
        return result;
    };

    // First attempt
    bool cancelled = false;
    std::optional<uint64_t> hit = runSearch(startOff, cancelled);
    if (cancelled) return;   // user aborted: no wrap retry, no "Cannot find"

    // Wrap once on miss, unless the wrap start equals the original start
    // (the first scan already covered the whole document).
    if (!hit.has_value()) {
        uint64_t wrapFrom;
        if (forward) {
            // Forward wrap: restart from BOM end (beginning of text)
            wrapFrom = (uint64_t)doc_->encoding().bomBytes;
        } else {
            // Backward wrap: restart from end of document
            wrapFrom = doc_->size();
        }
        if (wrapFrom != startOff) {
            hit = runSearch(wrapFrom, cancelled);
            if (cancelled) return;
        }
    }

    if (!hit.has_value()) {
        std::wstring msg = L"Cannot find \"" + findNeedle_ + L"\"";
        MessageBoxW(hwnd_, msg.c_str(), L"FastPad", MB_ICONINFORMATION);
        return;
    }

    // Select the match
    renderer_->setCaret(*hit, false);
    renderer_->setCaret(*hit + matchByteLen, true);
    renderer_->ensureCaretVisible();

    // Status: show match offset
    wchar_t sb[64];
    swprintf_s(sb, L"Found at offset %llu", (unsigned long long)*hit);
    setStatusPart(0, sb);
}

void AppWindow::updateStatusBar() {
    if (!status_) return;
    std::wstring enc = doc_ ? encoding_name(doc_->encoding()) : L"";
    wchar_t size[64] = L"", idx[64] = L"";
    if (doc_) {
        swprintf_s(size, L"%llu bytes", (unsigned long long)doc_->size());
        if (doc_->indexComplete())
            swprintf_s(idx, L"%llu lines", (unsigned long long)doc_->lineCount());
        else if (doc_->size())
            swprintf_s(idx, L"indexing %u%%", (unsigned)(doc_->indexedBytes() * 100 / doc_->size()));
    }
    wchar_t lncol[96] = L"";
    if (doc_ && renderer_) {
        uint64_t off = renderer_->topOffset();
        if (doc_->dirty()) {
            // Dirty doc: line index reflects the saved file, not live content.
            // Use lineBreaksBefore for the live offset when available.
            auto ln = doc_->lineBreaksBefore(off);
            if (ln.has_value())
                swprintf_s(lncol, L"Ln %llu   Off %llu", (unsigned long long)(*ln + 1), (unsigned long long)off);
            else
                swprintf_s(lncol, L"Off %llu", (unsigned long long)off);
        } else {
            if (doc_->indexedBytes() >= off)
                swprintf_s(lncol, L"Ln %llu   Off %llu", (unsigned long long)(doc_->lineOfOffset(off) + 1), (unsigned long long)off);
            else
                swprintf_s(lncol, L"Off %llu", (unsigned long long)off);
        }
    }
    setStatusPart(0, lncol);
    setStatusPart(1, enc.c_str());
    setStatusPart(2, size);
    setStatusPart(3, idx);
}

// Coalescing one-shot timer: replaces synchronous updateStatusBar() in
// scroll-driven paths so that fast scrolling produces at most one repaint
// per 80 ms. Calling SetTimer with an existing id simply resets the interval.
void AppWindow::scheduleStatusUpdate() {
    SetTimer(hwnd_, kStatusTimer, 80, nullptr);
}

// SB_SETTEXT repaints its part even when the text is identical - on a 250 ms
// timer plus every wheel tick that reads as status-strip flicker. Send only
// real changes.
void AppWindow::setStatusPart(int part, const wchar_t* text) {
    if (statusText_[part] == text) return;
    statusText_[part] = text;
    SendMessageW(status_, SB_SETTEXTW, (WPARAM)part, (LPARAM)text);
}

void AppWindow::updateTitleDirty() {
    if (titleBase_.empty()) return;
    const bool dirty = doc_ && doc_->dirty();
    SetWindowTextW(hwnd_, (dirty ? L"*" + titleBase_ : titleBase_).c_str());
}

// ---- editing helpers --------------------------------------------------------

void AppWindow::afterEdit(uint64_t newCaret) {
    renderer_->setCaret(newCaret, false);
    renderer_->ensureCaretVisible();
    renderer_->noteDocumentChanged();    // content changed: layout cache is stale
    updateStatusBar();
    updateTitleDirty();
}

uint64_t AppWindow::eraseSelectionIfAny() {
    uint64_t caret = renderer_->caret();
    if (renderer_->hasSelection()) {
        caret = renderer_->selBegin();
        doc_->eraseRange(caret, renderer_->selEnd() - caret);
        renderer_->clearSelection();
    }
    return caret;
}

// End-of-line caret position for the line containing `off`: findNextBreak
// includes the break bytes, so step back over up to two break chars (LF then
// CR for a CRLF pair). Encoding-agnostic via charStepBackward + decodeAt.
uint64_t AppWindow::lineEndOffset(uint64_t off) {
    uint64_t e = doc_->findNextBreak(off);
    for (int k = 0; k < 2 && e > off; ++k) {
        const uint64_t p = doc_->charStepBackward(e);
        std::wstring ch;
        doc_->decodeAt(p, (size_t)(e - p), ch);
        if (ch == L"\n" || ch == L"\r") e = p; else break;
    }
    return e;
}

// Up/Down caret movement. M2 approximation: the column is the BYTE delta from
// the line start, carried to the target line and clamped to its end - cheap
// and stable, but multi-byte chars make the visual column drift (M3 will use
// a real preferred-x column). The final offset is snapped onto a char
// boundary so the caret never lands inside a multi-byte sequence.
uint64_t AppWindow::caretLineVertical(int dir) {
    const uint64_t caret = renderer_->caret();
    const uint64_t lineStart = doc_->findPrevBreak(caret);
    const uint64_t column = caret - lineStart;
    uint64_t targetStart;
    if (dir < 0) {
        // Up at the first line moves to the line start (notepad behavior).
        if (lineStart <= (uint64_t)doc_->encoding().bomBytes) return lineStart;
        targetStart = doc_->findPrevBreak(lineStart);  // start of the previous line
    } else {
        // findNextBreak == size() when caret sits on the last line: target
        // collapses to the document end below.
        targetStart = doc_->findNextBreak(caret);      // start of the next line
    }
    const uint64_t end = lineEndOffset(targetStart);
    uint64_t target = targetStart + column;
    if (target >= end) return end;
    // Snap into the char grid: forward to the next boundary, then back to the
    // start of the char containing `target` (O(1) per char, not O(column)).
    return doc_->charStepBackward(doc_->charStepForward(target));
}

bool AppWindow::copySelection() {
    if (!renderer_->hasSelection()) return false;
    const uint64_t sb = renderer_->selBegin();
    const uint64_t len = renderer_->selEnd() - sb;
    if (len > kMaxClipboardBytes) {
        MessageBoxW(hwnd_, L"Selection is larger than 256 MB - too large for the clipboard.\nUse Save As for data that size.",
                    L"FastPad", MB_ICONINFORMATION);
        return false;
    }
    std::wstring text;
    doc_->decodeAt(sb, (size_t)len, text);
    if (!OpenClipboard(hwnd_)) return false;
    bool ok = false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(h)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
            ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        }
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}

void AppWindow::pasteClipboard() {
    std::wstring text;
    if (!OpenClipboard(hwnd_)) return;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) {
            text = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (text.empty()) return;
    const uint64_t caret = eraseSelectionIfAny();
    const std::vector<uint8_t> bytes = normalize_paste(doc_->encoding(), text, doc_->eolBytes());
    doc_->insertBytes(caret, bytes.data(), bytes.size());
    afterEdit(caret + bytes.size());
}

void AppWindow::onChar(wchar_t wch) {
    if (wch < 0x20) {
        // Ctrl chords (Ctrl+H/I/M...) arrive as control codes - never type them.
        if (GetKeyState(VK_CONTROL) < 0) return;
        if (wch != L'\r' && wch != L'\t' && wch != 0x08) return;
    }
    if (wch == 0x08) {                                 // Backspace
        uint64_t caret;
        if (renderer_->hasSelection()) {
            caret = eraseSelectionIfAny();
        } else {
            caret = renderer_->caret();
            if (caret <= (uint64_t)doc_->encoding().bomBytes) return;   // never eat the BOM
            const uint64_t from = doc_->charStepBackward(caret);        // UTF-8 safe single char
            if (from == caret) return;
            doc_->eraseRange(from, caret - from);
            caret = from;
        }
        afterEdit(caret);
        return;
    }
    const uint64_t caret = eraseSelectionIfAny();
    const uint64_t before = doc_->size();
    if (wch == L'\r') {                                // Enter: the document's own EOL
        const std::vector<uint8_t> eol = doc_->eolBytes();
        doc_->insertBytes(caret, eol.data(), eol.size());
    } else {
        doc_->insertText(caret, std::wstring(1, wch));
    }
    afterEdit(caret + (doc_->size() - before));
}

void AppWindow::onKeyDown(WPARAM vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    const uint64_t caret = renderer_->caret();
    switch (vk) {
    // PgUp/PgDn (and the wheel) keep the M1 scrolling; plain arrows are now
    // exclusively caret movement, matching notepad.
    case VK_PRIOR: renderer_->scrollPages(-1); scheduleStatusUpdate(); return;
    case VK_NEXT:  renderer_->scrollPages(1);  scheduleStatusUpdate(); return;
    case VK_LEFT:  renderer_->setCaret(doc_->charStepBackward(caret), shift); break;
    case VK_RIGHT: renderer_->setCaret(doc_->charStepForward(caret), shift); break;
    case VK_UP:    renderer_->setCaret(caretLineVertical(-1), shift); break;
    case VK_DOWN:  renderer_->setCaret(caretLineVertical(+1), shift); break;
    case VK_HOME:
        renderer_->setCaret(ctrl ? (uint64_t)doc_->encoding().bomBytes
                                 : doc_->findPrevBreak(caret), shift);
        break;
    case VK_END:
        if (ctrl) renderer_->goEnd();                  // scroll first so the caret lands visible
        renderer_->setCaret(ctrl ? doc_->size() : lineEndOffset(caret), shift);
        break;
    case VK_DELETE: {
        uint64_t at;
        if (renderer_->hasSelection()) {
            at = eraseSelectionIfAny();
        } else {
            at = caret;
            const uint64_t to = doc_->charStepForward(caret);
            if (to == caret) return;
            doc_->eraseRange(caret, to - caret);
        }
        afterEdit(at);
        return;
    }
    default: return;
    }
    updateStatusBar();
}

void AppWindow::onEditCommand(UINT id) {
    switch (id) {
    case IDM_UNDO:
        if (doc_->canUndo()) {
            doc_->undo();
            afterEdit(std::min(renderer_->caret(), doc_->size()));
        }
        break;
    case IDM_REDO:
        if (doc_->canRedo()) {
            doc_->redo();
            afterEdit(std::min(renderer_->caret(), doc_->size()));
        }
        break;
    case IDM_CUT:
        if (copySelection()) afterEdit(eraseSelectionIfAny());
        break;
    case IDM_COPY:
        copySelection();
        break;
    case IDM_PASTE:
        pasteClipboard();
        break;
    case IDM_SELECTALL:
        renderer_->setCaret((uint64_t)doc_->encoding().bomBytes, false);
        renderer_->setCaret(doc_->size(), true);
        updateStatusBar();
        break;
    default: break;
    }
}

void AppWindow::layout() {
    SendMessageW(status_, WM_SIZE, 0, 0);    // status bar auto-sizes to the bottom edge
    RECT rc; GetClientRect(hwnd_, &rc);
    int parts[4] = { rc.right - 420, rc.right - 260, rc.right - 130, -1 };
    SendMessageW(status_, SB_SETPARTS, 4, (LPARAM)parts);
    // The view fills the client area minus the status bar's height.
    RECT src{}; GetWindowRect(status_, &src);
    const int statusH = src.bottom - src.top;
    int viewH = rc.bottom - statusH;
    if (viewH < 0) viewH = 0;
    if (view_) MoveWindow(view_, 0, 0, rc.right, viewH, TRUE);
}

LRESULT AppWindow::handle(UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE:
        status_ = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd_, nullptr, inst_, nullptr);
        SetWindowLongPtrW(status_, GWL_EXSTYLE,
            GetWindowLongPtrW(status_, GWL_EXSTYLE) | WS_EX_COMPOSITED);
        // Text view child: owns the D2D target and BOTH scrollbars, so the
        // horizontal bar renders at the view's bottom edge, above the status
        // bar. WS_CLIPSIBLINGS keeps its paints off the status bar.
        CreateWindowExW(0, L"FastPadView", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_CLIPSIBLINGS,
            0, 0, 0, 0, hwnd_, nullptr, inst_, this);   // viewProc stores view_ on WM_NCCREATE
        settings_ = load_settings();       // before buildMenu: it checks Word wrap
        buildMenu();
        layout();
        renderer_ = std::make_unique<Renderer>(view_);
        renderer_->setFontSize(settings_.fontSize);
        renderer_->setWordWrap(settings_.wordWrap);
        return 0;
    case WM_SIZE:
        // layout() moves the view; the view's own WM_SIZE drives renderer onResize.
        layout();
        return 0;
    case WM_SETFOCUS:
        // Keyboard input lives on the view: typing works the moment the app
        // (or a dialog returning) activates the main window.
        if (view_) SetFocus(view_);
        return 0;
    case WM_TIMER:
        if (wp == kStatusTimer) {      // debounced scroll-driven status update
            KillTimer(hwnd_, kStatusTimer);
            updateStatusBar();
            return 0;
        }
        // id 1: indexing progress timer
        updateStatusBar();
        if (doc_ && doc_->indexComplete() && indexTimer_) { KillTimer(hwnd_, indexTimer_); indexTimer_ = 0; }
        return 0;
    case WM_INITMENUPOPUP: {
        // Radio-check the Font size entry closest to the live size (Ctrl+wheel
        // zooming can land between the fixed entries).
        HMENU menu = (HMENU)wp;
        if (renderer_ && GetMenuItemID(menu, 0) == IDM_VIEW_FONTSIZE_BASE) {
            const float cur = renderer_->fontSize();
            UINT best = 0;
            for (UINT i = 1; i < (UINT)_countof(kFontSizes); ++i)
                if (std::abs((float)kFontSizes[i] - cur) < std::abs((float)kFontSizes[best] - cur))
                    best = i;
            CheckMenuRadioItem(menu, 0, (UINT)_countof(kFontSizes) - 1, best, MF_BYPOSITION);
        }
        // Help popup: exactly one of Register/Unregister is actionable.
        if (GetMenuItemID(menu, 0) == IDM_REGISTER) {
            const bool reg = is_registered();
            EnableMenuItem(menu, IDM_REGISTER,
                MF_BYCOMMAND | (reg ? MF_GRAYED : MF_ENABLED));
            EnableMenuItem(menu, IDM_UNREGISTER,
                MF_BYCOMMAND | (reg ? MF_ENABLED : MF_GRAYED));
        }
        return 0;
    }
    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        auto switchEncoding = [this](const EncodingInfo& e) {
            if (!doc_) return;
            if (!doc_->setEncoding(e)) {
                MessageBoxW(hwnd_, L"Save or undo changes before switching encoding.", L"FastPad", MB_ICONINFORMATION);
                return;
            }
            if (renderer_) renderer_->setDocument(doc_.get());   // resets layout cache too
            updateStatusBar();
            InvalidateRect(view_, nullptr, FALSE);
        };
        if (id == IDM_FIND && doc_ && renderer_) {
            // Always open the dialog for IDM_FIND
            FindResult fr = show_find_dialog(hwnd_, findNeedle_, findMatchCase_);
            if (!fr.ok) return 0;
            findNeedle_ = fr.needle;
            findMatchCase_ = fr.matchCase;
            if (!findNeedle_.empty()) doFind(fr.forward);
        }
        else if (id == IDM_FIND_NEXT && doc_ && renderer_) {
            if (findNeedle_.empty()) {
                // No previous needle: open the dialog
                FindResult fr = show_find_dialog(hwnd_, findNeedle_, findMatchCase_);
                if (!fr.ok) return 0;
                findNeedle_ = fr.needle;
                findMatchCase_ = fr.matchCase;
                if (!findNeedle_.empty()) doFind(fr.forward);
            } else {
                doFind(true);
            }
        }
        else if (id == IDM_FIND_PREV && doc_ && renderer_) {
            if (findNeedle_.empty()) {
                FindResult fr = show_find_dialog(hwnd_, findNeedle_, findMatchCase_);
                if (!fr.ok) return 0;
                findNeedle_ = fr.needle;
                findMatchCase_ = fr.matchCase;
                if (!findNeedle_.empty()) doFind(fr.forward);
            } else {
                doFind(false);
            }
        }
        else if (id == IDM_OPEN) onOpenDialog();
        else if (id == IDM_SAVE) {
            if (doc_ && doc_->dirty()) saveFlow(doc_->path(), std::nullopt);
        }
        else if (id == IDM_SAVEAS) onSaveAsDialog(std::nullopt);
        else if (id == IDM_SAVEAS_UTF8) onSaveAsDialog(EncodingInfo{ EncodingKind::Utf8, 65001, 3 });
        else if (id == IDM_SAVEAS_UTF16LE) onSaveAsDialog(EncodingInfo{ EncodingKind::Utf16LE, 1200, 2 });
        else if (id == IDM_SAVEAS_LF) onSaveAsDialog(std::nullopt, std::wstring(L"\n"));
        else if (id == IDM_SAVEAS_CRLF) onSaveAsDialog(std::nullopt, std::wstring(L"\r\n"));
        else if (id == IDM_EXIT) SendMessageW(hwnd_, WM_CLOSE, 0, 0);   // route through the dirty guard
        else if (id == IDM_ABOUT) MessageBoxW(hwnd_, L"FastPad - any-size text viewer/editor.", L"About FastPad", MB_OK);
        else if (id == IDM_REGISTER) {
            std::wstring err;
            if (register_open_with(&err))
                MessageBoxW(hwnd_,
                    L"FastPad now appears in Open With for text files.\n\n"
                    L"To make it the DEFAULT, pick it once in Settings > Default apps "
                    L"(Windows requires the user to do this).",
                    L"FastPad", MB_ICONINFORMATION);
            else
                MessageBoxW(hwnd_, (L"Registration failed:\n" + err).c_str(), L"FastPad", MB_ICONERROR);
        }
        else if (id == IDM_UNREGISTER) {
            std::wstring err;
            if (unregister_open_with(&err))
                MessageBoxW(hwnd_, L"FastPad removed from Open With.", L"FastPad", MB_ICONINFORMATION);
            else
                MessageBoxW(hwnd_, (L"Unregister failed:\n" + err).c_str(), L"FastPad", MB_ICONERROR);
        }
        else if (id == IDM_VIEW_WRAP) {
            if (renderer_) {
                renderer_->setWordWrap(!renderer_->wordWrap());
                settings_.wordWrap = renderer_->wordWrap();
                save_settings(settings_);
                CheckMenuItem(GetMenu(hwnd_), IDM_VIEW_WRAP,
                    MF_BYCOMMAND | (settings_.wordWrap ? MF_CHECKED : MF_UNCHECKED));
            }
        }
        else if (id >= IDM_VIEW_FONTSIZE_BASE && id < IDM_VIEW_FONTSIZE_BASE + (UINT)_countof(kFontSizes)) {
            if (renderer_) {
                renderer_->setFontSize((float)kFontSizes[id - IDM_VIEW_FONTSIZE_BASE]);
                settings_.fontSize = renderer_->fontSize();
                save_settings(settings_);
            }
        }
        else if (id >= IDM_UNDO && id <= IDM_SELECTALL && doc_ && renderer_) onEditCommand(id);
        else if (id == IDM_GOTO && doc_ && renderer_) {
            uint64_t v = 0; bool isLine = true;
            if (show_goto_dialog(hwnd_, &v, &isLine)) {
                if (isLine) {
                    if (doc_->dirty()) {
                        MessageBoxW(hwnd_, L"Line navigation uses the saved file's lines until you save - use :offset for exact positions in an edited file.", L"FastPad", MB_ICONINFORMATION);
                    } else {
                        uint64_t line = (v == 0) ? 0 : v - 1;
                        if (line < doc_->lineCount() && doc_->lineStart(line) <= doc_->indexedBytes()) renderer_->goToLine(line);
                        else MessageBoxW(hwnd_, L"That line is not indexed yet - try again in a moment, or use :offset.", L"FastPad", MB_ICONINFORMATION);
                    }
                } else renderer_->goToOffset(v);
                updateStatusBar();
            }
        }
        else if (doc_ && id == IDM_ENC_UTF8) switchEncoding({EncodingKind::Utf8, 65001, 0});
        else if (doc_ && id == IDM_ENC_UTF16LE) switchEncoding({EncodingKind::Utf16LE, 1200, 0});
        else if (doc_ && id == IDM_ENC_UTF16BE) switchEncoding({EncodingKind::Utf16BE, 1201, 0});
        else if (doc_ && id >= IDM_ENC_CP_BASE && id < IDM_ENC_CP_BASE + (UINT)g_codepages.size())
            switchEncoding({EncodingKind::Ansi, g_codepages[id - IDM_ENC_CP_BASE].first, 0});
        return 0;
    }
    case WM_CLOSE:
        if (doc_ && doc_->dirty()) {
            int r = MessageBoxW(hwnd_, L"Save changes?", L"FastPad", MB_YESNOCANCEL | MB_ICONWARNING);
            if (r == IDCANCEL) return 0;               // abort the close
            if (r == IDYES) {
                saveFlow(doc_->path(), std::nullopt);
                // Save failed or was cancelled: stay open so nothing is lost.
                if (doc_ && doc_->dirty()) return 0;
            }
        }
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd_, m, wp, lp);
}

// Text-view child wndproc body: paint, scrolling, mouse and keyboard input.
// The renderer (and its caret blink timer, id 2) live on this window; the
// main window keeps menus, accel-routed WM_COMMAND and timers 1/3.
LRESULT AppWindow::handleView(UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_SIZE:
        // Covers layout()'s MoveWindow AND scrollbar show/hide (ShowScrollBar
        // resizes the client area without any main-window WM_SIZE).
        if (renderer_) renderer_->onResize();
        return 0;
    case WM_PAINT:
        if (renderer_) { renderer_->onPaint(); return 0; }
        break;
    case WM_ERASEBKGND: return 1;
    case WM_VSCROLL:
        if (renderer_) { renderer_->onVScroll(wp); scheduleStatusUpdate(); }
        return 0;
    case WM_HSCROLL:
        if (renderer_) renderer_->onHScroll(wp);
        return 0;
    case WM_MOUSEHWHEEL:
        // Tilt/horizontal wheel: 48 DIP per notch, positive delta = right.
        if (renderer_ && doc_)
            renderer_->hScrollBy((float)GET_WHEEL_DELTA_WPARAM(wp) * 48.0f / (float)WHEEL_DELTA);
        return 0;
    case WM_MOUSEWHEEL:
        if (renderer_ && doc_) {
            if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
                renderer_->setFontSize(renderer_->fontSize() + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1.0f : -1.0f));
                settings_.fontSize = renderer_->fontSize();
                save_settings(settings_);
            }
            else {
                renderer_->scrollLines(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -3 : 3);
                scheduleStatusUpdate();
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (renderer_ && doc_) onKeyDown(wp);
        return 0;
    case WM_CHAR:
        if (renderer_ && doc_) onChar((wchar_t)wp);
        return 0;
    case WM_LBUTTONDOWN:
        if (renderer_ && doc_) {
            SetFocus(view_);               // child windows do not get focus on click by default
            SetCapture(view_);
            mouseSelecting_ = true;
            auto hit = renderer_->hitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hit.first) renderer_->setCaret(hit.second, GetKeyState(VK_SHIFT) < 0);
            updateStatusBar();
        }
        return 0;
    case WM_MOUSEMOVE:
        if (mouseSelecting_ && renderer_ && doc_ && (wp & MK_LBUTTON)) {
            // Captured drag past a VIEW client edge: auto-scroll one step
            // toward the cursor and hit-test at the clamped edge instead (the
            // cached layouts are x-independent, so the new scrollX_ applies at once).
            int px = GET_X_LPARAM(lp), py = GET_Y_LPARAM(lp);
            RECT rc; GetClientRect(view_, &rc);
            if (px < 0) { renderer_->hScrollBy(-24.0f); px = 0; }
            else if (px > rc.right) { renderer_->hScrollBy(24.0f); px = rc.right; }
            if (py < 0) { renderer_->scrollLines(-1); py = 0; scheduleStatusUpdate(); }
            else if (py > rc.bottom) { renderer_->scrollLines(1); py = rc.bottom; scheduleStatusUpdate(); }
            auto hit = renderer_->hitTest(px, py);
            if (hit.first) renderer_->setCaret(hit.second, true);
        }
        return 0;
    case WM_LBUTTONUP:
        if (mouseSelecting_) { mouseSelecting_ = false; ReleaseCapture(); }
        return 0;
    case WM_CAPTURECHANGED:
        mouseSelecting_ = false;
        return 0;
    case WM_TIMER:
        if (wp == 2 && renderer_) renderer_->onCaretTimer();   // blink, set on view_ by the renderer
        return 0;
    }
    return DefWindowProcW(view_, m, wp, lp);
}

} // namespace fastpad
