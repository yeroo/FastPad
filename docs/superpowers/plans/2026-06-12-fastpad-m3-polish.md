# FastPad M3 (Find, Wrap, Polish) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The M3 milestone: find (any size, any encoding), word wrap, EOL conversion on save, Open-With shell registration, app icon — plus the M2-BACKLOG scrollbar/footprint fixes. After M3 FastPad is feature-complete v1.

**Architecture:** Search is a core, chunked, cancellable byte-scanner over `Document::read` (needle encoded into the document's encoding; ASCII-only case folding — documented limit; UTF-16 matches alignment-checked). Word wrap rides the existing per-line `IDWriteTextLayout` (enable wrapping, advance y by measured height). EOL conversion reuses the save transcode path (decode → normalize breaks → encode), correct for every encoding at any size. Registration is per-user (HKCU Capabilities + RegisteredApplications), no admin.

**Tech Stack:** unchanged. Repo `C:\Users\Boris.Kudriashov\GitHub\FastPad`, branch `m3-polish` (create from `m2-editor`). Build/test loop as before (MSBuild Debug x64 + `x64\Debug\tests.exe`, currently 50 cases green; W4+WX; new files into the owning vcxproj).

---

## File structure (M3 deltas)

```
core/search.h / .cpp            (NEW: chunked document search)
core/save.h / .cpp              (MOD: SaveOptions.forceEol via decode/encode path)
app/find_dialog.h / .cpp        (NEW: find UI, goto-dialog pattern)
app/registration.h / .cpp       (NEW: per-user Open-With registration)
app/renderer.h / .cpp           (MOD: word wrap, scrollbar fixes)
app/app_window.cpp(+h)          (MOD: find wiring, wrap toggle, EOL save items,
                                 register menu, F3 accels)
app/settings.h / .cpp           (MOD: wordWrap persisted)
app/app.rc / resource.h         (MOD: icon + VERSIONINFO)
app/app.ico                     (NEW: generated)
tests/test_search.cpp           (NEW)
tests/test_save.cpp             (MOD: forceEol cases)
```

---

### Task 0: branch
- [ ] `git checkout -b m3-polish` (from m2-editor).

### Task 1: core search engine (TDD)

**Files:** `core/search.h/.cpp`, `tests/test_search.cpp`.

`core/search.h`:

```cpp
#pragma once
#include "core/document.h"
#include <atomic>
#include <functional>
#include <optional>

namespace fastpad {

struct SearchOptions {
    bool caseSensitive = true;
    bool forward = true;
    // Test hook: chunk size for the scan (default 4 MB).
    size_t chunkSize = 4u << 20;
};

// Finds the needle in the live document. Forward: first match with
// offset >= from. Backward: last match with offset < from. Returns the match's
// byte offset, or nullopt (not found / cancelled / empty needle).
// The needle is encoded into the document's encoding and matched on bytes.
// Case-insensitive folds ASCII A-Z only (safe in UTF-8: ASCII bytes never
// occur inside multibyte sequences; UTF-16: folds units 0x41..0x5A).
// UTF-16 documents: only matches at even offsets count (unit alignment).
// progress may be null; cancel checked per chunk.
std::optional<uint64_t> search_document(Document& doc, const std::wstring& needle,
    uint64_t from, const SearchOptions& opts,
    const std::function<void(uint64_t scanned, uint64_t total)>& progress,
    const std::atomic<bool>& cancel);

} // namespace fastpad
```

- [ ] **Step 1: tests** — `tests/test_search.cpp` (exact; helpers from test_helpers.h):

```cpp
#include "doctest/doctest.h"
#include "core/search.h"
#include "test_helpers.h"
#include <atomic>
#include <cstring>

using namespace fastpad;

static std::atomic<bool> g_noCancel{false};

static std::optional<uint64_t> find(Document& d, const wchar_t* needle, uint64_t from,
                                    bool cs = true, bool fwd = true, size_t chunk = 4u << 20) {
    SearchOptions o; o.caseSensitive = cs; o.forward = fwd; o.chunkSize = chunk;
    return search_document(d, needle, from, o, nullptr, g_noCancel);
}

static Document open_bytes(const std::vector<uint8_t>& b, std::wstring* keepPath, TempFileGuard& g) {
    g.path = write_temp_file(b);
    if (keepPath) *keepPath = g.path;
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    return d;
}

TEST_CASE("search: forward / backward / not found") {
    TempFileGuard g;
    auto d = open_bytes({'a','b','c','a','b','c','x'}, nullptr, g);
    CHECK(find(d, L"abc", 0) == 0);
    CHECK(find(d, L"abc", 1) == 3);
    CHECK(find(d, L"abc", 4) == std::nullopt);
    CHECK(find(d, L"abc", 7, true, false) == 3);     // backward from end
    CHECK(find(d, L"abc", 3, true, false) == 0);     // backward excludes from
    CHECK(find(d, L"zzz", 0) == std::nullopt);
    CHECK(find(d, L"", 0) == std::nullopt);
}

TEST_CASE("search: case-insensitive ASCII fold") {
    TempFileGuard g;
    auto d = open_bytes({'H','e','L','L','o'}, nullptr, g);
    CHECK(find(d, L"hello", 0, false) == 0);
    CHECK(find(d, L"hello", 0, true) == std::nullopt);
}

TEST_CASE("search: match straddles chunk boundary") {
    std::vector<uint8_t> b(300, 'x');
    b[97] = 'N'; b[98] = 'E'; b[99] = 'E'; b[100] = 'D';   // straddles 100-byte chunks
    TempFileGuard g;
    auto d = open_bytes(b, nullptr, g);
    CHECK(find(d, L"NEED", 0, true, true, 100) == 97);
}

TEST_CASE("search: UTF-8 multibyte needle") {
    // "xЯy" in UTF-8
    TempFileGuard g;
    auto d = open_bytes({'x', 0xD0, 0xAF, 'y'}, nullptr, g);
    CHECK(find(d, L"\x042F", 0) == 1);
}

TEST_CASE("search: UTF-16 alignment - no match at odd offset") {
    // UTF-16LE BOM + "ab": bytes FF FE 61 00 62 00. The byte pattern 00 62
    // exists at odd offset 3 but is not a unit; searching for U+6200 must miss.
    TempFileGuard g;
    auto d = open_bytes({0xFF,0xFE,'a',0,'b',0}, nullptr, g);
    REQUIRE(d.encoding().kind == EncodingKind::Utf16LE);
    CHECK(find(d, L"\x6200", 0) == std::nullopt);
    CHECK(find(d, L"ab", 0) == 2);                   // real match, unit-aligned
}

TEST_CASE("search: searches live edited content") {
    TempFileGuard g;
    auto d = open_bytes({'a','b'}, nullptr, g);
    d.insertText(1, L"FIND");
    CHECK(find(d, L"FIND", 0) == 1);
}

TEST_CASE("search: cancel returns nullopt") {
    TempFileGuard g;
    auto d = open_bytes(std::vector<uint8_t>(1000, 'q'), nullptr, g);
    std::atomic<bool> cancelled{true};
    SearchOptions o;
    CHECK(search_document(d, L"q", 0, o, nullptr, cancelled) == std::nullopt);
}
```

- [ ] **Step 2:** RED. **Step 3:** implement `core/search.cpp`:
  - needle bytes = `encode_text(doc.encoding(), needle)`; empty → nullopt.
  - fold helper: when !caseSensitive, fold bytes: for 1-byte/UTF-8 docs fold `A-Z`→`a-z` per byte; for UTF-16 fold the unit's low byte when its high byte is 0 (LE: even index is low byte; BE: odd index) — implement as a per-position fold function used in the comparator; fold the needle ONCE upfront.
  - forward scan: chunks of `opts.chunkSize` + overlap `needleLen-1` (read via `doc.read` into a buffer; last chunk shorter). Naive scan with first-byte skip is fine (memchr on the first needle byte, then memcmp) — budget >= 500 MB/s; no Boyer-Moore (YAGNI).
  - backward: iterate chunk windows descending from `from`; within a chunk find the LAST match with offset < from.
  - UTF-16 alignment: reject matches at odd `offset` (BOM is 2 bytes so units sit at even offsets).
  - progress(scannedBytes, totalToScan) once per chunk; cancel per chunk AND per 1 MB inside the scan loop.
- [ ] **Step 4:** green (50 prior + 7 new = 57). **Step 5:** commit `feat(core): chunked cancellable document search (TDD)`.

---

### Task 2: Find UI

**Files:** `app/find_dialog.h/.cpp` (goto_dialog pattern), `app/app_window.cpp(+h)`, `app/commands.h`, `app/main.cpp`.

- Dialog (owner-disabled modal, done-flag loop — copy the goto_dialog discipline): Edit field (prefilled with last needle), "Match &case" checkbox, buttons "Find &Next" (IDOK), "Find &Prev", Cancel. Returns {needle, matchCase, direction} via a state struct; remembers nothing itself.
- `app/commands.h`: IDM_FIND=208, IDM_FIND_NEXT=209, IDM_FIND_PREV=210. Edit menu: "&Find...\tCtrl+F" above Go to; accels Ctrl+F, F3 (IDM_FIND_NEXT, FVIRTKEY VK_F3), Shift+F3 (IDM_FIND_PREV, FSHIFT|FVIRTKEY).
- AppWindow members: `std::wstring findNeedle_; bool findMatchCase_ = false;`
- `doFind(bool forward)` helper:
  - empty needle → open the dialog instead.
  - start offset: forward → `renderer_->hasSelection() ? renderer_->selEnd() : renderer_->caret()`; backward → `selBegin()/caret()`.
  - docs > 64 MB: run `search_document` under the save-style progress popup ("Searching… X%", Cancel/Esc); smaller: inline.
  - hit: select the match (`setCaret(match, false)` then `setCaret(match + matchByteLen, true)` where matchByteLen = encode_text(doc encoding, needle).size()), `ensureCaretVisible()`, status update.
  - miss: wrap once (forward → from 0/bomBytes; backward → from size()); still miss → `MessageBoxW(... L"Cannot find \"<needle>\"", MB_ICONINFORMATION)`.
- IDM_FIND → dialog → store needle/case → doFind(direction from the button). F3/Shift+F3 → doFind(true/false) with stored needle.
- [ ] Build; 57 tests stay green; headless launch smoke; commit `feat(app): find with F3 repeat, wrap-around, background search on giants`.

---

### Task 3: word wrap

**Files:** `app/renderer.h/.cpp`, `app/app_window.cpp`, `app/commands.h`, `app/settings.h/.cpp`.

- Renderer: `bool wordWrap_ = false;` + `void setWordWrap(bool)` (linesDirty_ + invalidate) + getter. In `buildVisibleLines`: when wrap on, create each layout with maxWidth = clientWidth - 8 and `SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP)` (per-layout — the shared format stays NO_WRAP); after creation `GetMetrics` → advance `y += max(metrics.height, lineHeight_)`. When off: existing single-row path. Hit-testing/caret/selection already use the layout APIs — they handle wrapped rows without change (verify HitTestPoint with y relative to the LINE's top: the stored `y` per VisibleLine plus point-in-line slot lookup must use each line's actual height — replace the fixed `lineHeight_` slot math in `hitTest` with a scan over `lines_` comparing y ranges).
- `ensureCaretVisible`/`linesPerPage` keep logical-line semantics (acceptable: a page scrolls fewer visual rows when wrapping; note in code).
- `app/commands.h`: IDM_VIEW_WRAP=510. View menu: "&Word wrap" above Font size; WM_COMMAND toggles `renderer_->setWordWrap(!...)`, persists, and `CheckMenuItem(MF_CHECKED/UNCHECKED)`; set initial check in buildMenu from settings.
- `app/settings.h/.cpp`: `bool wordWrap = false;` in AppSettings; parse/write `wordWrap=0/1`. WM_CREATE applies it after renderer construction.
- [ ] Build; tests green; commit `feat(app): word wrap toggle (persisted)`.

---

### Task 4: EOL conversion on save (TDD)

**Files:** `core/save.h/.cpp`, `tests/test_save.cpp`, then `app/app_window.cpp` menu items.

- `SaveOptions` gains `std::optional<std::wstring> forceEol;` (L"\n" or L"\r\n"). When set, the save runs through the decode/encode path even if `transcodeTo` is empty (target encoding = source encoding). EOL normalization on each decoded chunk: replace `\r\n`→`\n`, lone `\r`→`\n`, then `\n`→`*forceEol`. CHUNK-EDGE RULE: if a decoded chunk ends with `\r`, hold that character back (carry into the next chunk) so a split CRLF can't double-convert; flush a held `\r` as a break at EOF.
- Tests (append to `tests/test_save.cpp`):

```cpp
TEST_CASE("save: force LF endings") {
    TempFileGuard g{ write_temp_file({'a','\r','\n','b','\r','c','\n','d'}) };
    std::wstring target = g.path + L".lf";
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    std::atomic<bool> cancel{false};
    std::wstring err;
    SaveOptions o; o.targetPath = target; o.forceEol = L"\n";
    REQUIRE(save_document(d, o, nullptr, cancel, &err));
    CHECK(file_bytes(target) == std::vector<uint8_t>{'a','\n','b','\n','c','\n','d'});
    DeleteFileW(target.c_str());
}

TEST_CASE("save: force CRLF endings on UTF-16 document") {
    std::vector<uint8_t> src{0xFF,0xFE,'a',0,'\n',0,'b',0};
    TempFileGuard g{ write_temp_file(src) };
    std::wstring target = g.path + L".crlf";
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    std::atomic<bool> cancel{false};
    std::wstring err;
    SaveOptions o; o.targetPath = target; o.forceEol = L"\r\n";
    REQUIRE(save_document(d, o, nullptr, cancel, &err));
    CHECK(file_bytes(target) == std::vector<uint8_t>{0xFF,0xFE,'a',0,'\r',0,'\n',0,'b',0});
    DeleteFileW(target.c_str());
}
```
(NOTE the UTF-16 expectation: source had a BOM → the decode/encode path must re-emit the BOM when the effective target encoding has bomBytes>0 — effective target = transcodeTo.value_or(source encoding INCLUDING its bom setting). Derive carefully; the test pins it.)
- Menu: Encoding menu gains "Save with &LF endings..." (IDM_SAVEAS_LF=405) and "Save with C&RLF endings..." (406) → Save As dialog flow with forceEol set.
- [ ] RED → implement → green (59 cases); commit `feat(core): EOL conversion on save (TDD) + menu entries`.

---

### Task 5: scrollbar fixes + indexer footprint (M2-BACKLOG)

**Files:** `app/renderer.cpp`, `core/document.cpp`.

- `updateScrollBar`: when `doc_->indexComplete()` and lineCount > 0: `si.nPage = (UINT)std::clamp<uint64_t>((uint64_t)linesPerPage() * 100000 / doc_->lineCount(), 1, 100000); si.nMax = 100000;` pos stays offset-proportional. Else current fixed values.
- Thumb off-by-one: clamp `si.nTrackPos` to 99000 before the divide in `onVScroll`; and in `goToOffset`, clamp the incoming offset to `size() ? size()-1 : 0` BEFORE the `prevLineStart(offset+1)` call (kills the prevLineStart(size+1) index-skip).
- Indexer footprint: in `Document::restartIndex`, construct the indexer's MmapFile as `std::make_shared<MmapFile>(16ull << 20, 2)` (16 MB windows, 2 views) — bounded mapped footprint during the background pass; comment why.
- [ ] Build; 59 tests green (no behavior tests break — verify); headless 1 GB sparse launch: working set after 3 s should be far below the previous ~900 MB (report the number). Commit `fix(app): proportional scrollbar, thumb clamp, bounded indexer footprint`.

---

### Task 6: icon, version info, Open-With registration

**Files:** `app/app.ico` (generated), `app/app.rc`, `app/resource.h`, `app/registration.h/.cpp`, `app/app_window.cpp`, `app/commands.h`.

- **Icon**: generate with PowerShell System.Drawing — blue (#1B66C9) rounded square, white bold "F" (Segoe UI or the embedded Meslo via GDI fallback Consolas), sizes 16/32/48/256 packed into one .ico (write the ICO header manually: PNG-compressed 256 + BMP smaller sizes is complex — simplest correct: save four PNGs and pack as PNG-type ICO entries, which Windows 10+ supports for all sizes). Provide the generation script inline in the task execution; commit the resulting `app/app.ico` binary. Add `IDI_APP ICON "app.ico"` to app.rc (+ `#define IDI_APP 1` in resource.h) and `wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP)); wc.hIconSm` likewise via WNDCLASSEXW (switch RegisterClass to the EX variants).
- **VERSIONINFO** in app.rc: FileVersion/ProductVersion 0.3.0.0, ProductName "FastPad", FileDescription "FastPad - any-size text editor", LegalCopyright "Boris Kudriashov".
- **Registration** (`app/registration.h/.cpp`): `bool register_open_with(std::wstring* err)` / `bool unregister_open_with(std::wstring* err)` / `bool is_registered()`. Per-user only:
  - `HKCU\Software\Classes\Applications\FastPad.exe\shell\open\command` = `"<full exe path>" "%1"`; `FriendlyAppName` = "FastPad".
  - ProgID `HKCU\Software\Classes\FastPad.Document\shell\open\command` (same) + `FriendlyTypeName`.
  - `HKCU\Software\FastPad\Capabilities`: `ApplicationName`, `ApplicationDescription`, `FileAssociations` subkey mapping `.txt .log .csv .md .json .xml .ini .yaml .yml` → `FastPad.Document`.
  - `HKCU\Software\RegisteredApplications` value `FastPad` = `Software\FastPad\Capabilities`.
  - `SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr)` after both register and unregister. Unregister deletes all the above (RegDeleteTree).
- Menu: Help → "Register in Open With" / "Unregister" (IDM_REGISTER=310, IDM_UNREGISTER=311); after success a MessageBox explains: "FastPad now appears in Open With for text files. To make it the DEFAULT, pick it once in Settings > Default apps (Windows requires the user to do this)."
- Smoke: call register, `reg query HKCU\Software\RegisteredApplications /v FastPad` shows the value; unregister removes it (leave the machine UNREGISTERED after the smoke).
- [ ] Build; tests green; commit `feat(app): icon, version info, per-user Open-With registration`.

---

### Task 7: final review + docs + version

- [ ] Dispatch final M3 review (controller); fix Criticals.
- [ ] README: Find/F3, word wrap, EOL-conversion saves, registration how-to (+ the Default-apps note), icon. Replace `docs/superpowers/M2-BACKLOG.md` with `BACKLOG.md` carrying only still-open items (teardown singleton, surrogate drift, TSF IME, double-click word select, add-buffer spill, piece-vector→tree if ever needed).
- [ ] Commit `docs: M3 complete - FastPad v1 feature set`.

---

## Notes / risks
- Search folding is ASCII-only by design — say so in the Find dialog label? No: README only (dialog stays clean).
- Backward search within a chunk must find the LAST match — easy to get wrong; the `backward excludes from` test pins the boundary.
- Word-wrap hit-testing must switch from fixed-slot math to per-line y ranges — the one real renderer change; everything else rides the layout APIs.
- Registration writes ONLY under HKCU — never elevate, never touch HKLM.
- PNG-type ICO requires Win10+ for small sizes in some shell views; if Explorer shows blanks at 16px, fall back to BMP-encoded 16/32 entries (System.Drawing Icon.Save can do classic packing via Icon.FromHandle per size — iterate if needed).
