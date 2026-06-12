# FastPad M2 (Editor) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Full editing on files of any size: typing/delete/paste with selection and clipboard, undo/redo, and streaming save (with optional encoding transcode) — the M2 milestone of `docs/superpowers/specs/2026-06-11-fastpad-design.md`.

**Architecture:** A `PieceTable` (piece VECTOR with typing coalescing and per-piece line-break counts — deliberate simplification of the spec's balanced tree: identical public API, sub-ms ops at 100k+ pieces, swappable container if ever profiled otherwise) layered behind the existing `Document` facade. The original file stays memory-mapped and untouched; edits land in a RAM add-buffer. Save streams the piece sequence to a temp file and atomically replaces. The renderer gains caret/selection via per-visible-line `IDWriteTextLayout` hit testing. Line scans move from the renderer into core (`findNextBreak`/`findPrevBreak`) — fixing M2-BACKLOG item 2 (UTF-16 byte-scan false positives) by scanning full code units.

**Tech Stack:** unchanged — C++20 / MSVC v143 / x64 / static CRT, doctest, Win32 + Direct2D/DirectWrite.

**Repo:** `C:\Users\Boris.Kudriashov\GitHub\FastPad`. Branch for this work: `m2-editor` (create from `m1-viewer`). Build/test loop:
```powershell
$env:MSBUILDDISABLENODEREUSE="1"
& "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" FastPad.sln /t:Build /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
.\x64\Debug\tests.exe
```
Suite currently: 24 cases / 200,077 assertions green. W4+WX. New files go into the owning .vcxproj.

**Out of scope (M3/backlog):** find, word wrap, shell registration, full IME/TSF (WM_CHAR-level input only), add-buffer spill-to-temp for multi-GB pastes (RAM-only; note in README).

---

## File structure (M2 deltas)

```
core/piece_table.h / .cpp        (NEW: pieces, edits, undo/redo, break counts)
core/encoding.h / .cpp           (MOD: + encode_text)
core/document.h / .cpp           (MOD: editable layer, read/insertText/eraseRange/undo/redo/
                                  dirty/eolBytes/charStep/findNextBreak/findPrevBreak/
                                  lineBreaksBefore; decodeAt via pieces when dirty)
core/save.h / .cpp               (NEW: streaming save + transcode + atomic replace)
app/renderer.h / .cpp            (MOD: visible-line layouts, caret, selection, hitTest;
                                  delegates line scans to Document)
app/app_window.cpp               (MOD: Edit menu, input, clipboard, save UX, dirty title)
tests/test_piece_table.cpp       (NEW)
tests/test_document_edit.cpp     (NEW)
tests/test_save.cpp              (NEW)
tests/test_encoding.cpp          (MOD: encode_text cases)
```

---

### Task 0: branch

- [ ] `git checkout -b m2-editor` (from m1-viewer). No commit.

### Task 1: `encode_text` + failing tests (TDD)

**Files:** Modify `core/encoding.h/.cpp`; modify `tests/test_encoding.cpp`.

- [ ] **Step 1: tests** — append to `tests/test_encoding.cpp`:

```cpp
TEST_CASE("encode_text: round-trips through each encoding kind") {
    CHECK(encode_text({EncodingKind::Utf8, 65001, 0}, L"hi\x042F") == std::vector<uint8_t>{'h','i',0xD0,0xAF});
    CHECK(encode_text({EncodingKind::Utf16LE, 1200, 0}, L"hi") == std::vector<uint8_t>{'h',0,'i',0});
    CHECK(encode_text({EncodingKind::Utf16BE, 1201, 0}, L"hi") == std::vector<uint8_t>{0,'h',0,'i'});
    CHECK(encode_text({EncodingKind::Ansi, 1251, 0}, L"\x042F") == std::vector<uint8_t>{0xDF});
}

TEST_CASE("encode_text: unencodable char in ANSI becomes '?'") {
    auto b = encode_text({EncodingKind::Ansi, 1252, 0}, L"\x4E0A");   // CJK in cp1252
    CHECK(b == std::vector<uint8_t>{'?'});
}
```

- [ ] **Step 2:** run → RED (no `encode_text`).

- [ ] **Step 3:** declare in `core/encoding.h`:
```cpp
// Encodes UTF-16 text into the document's byte encoding. Unencodable
// characters become the codepage default char ('?').
std::vector<uint8_t> encode_text(const EncodingInfo& enc, const std::wstring& text);
```
Implement in `core/encoding.cpp`:
```cpp
std::vector<uint8_t> encode_text(const EncodingInfo& enc, const std::wstring& text) {
    std::vector<uint8_t> out;
    if (text.empty()) return out;
    if (enc.kind == EncodingKind::Utf16LE || enc.kind == EncodingKind::Utf16BE) {
        out.reserve(text.size() * 2);
        for (wchar_t ch : text) {
            uint8_t lo = (uint8_t)(ch & 0xFF), hi = (uint8_t)(ch >> 8);
            if (enc.kind == EncodingKind::Utf16LE) { out.push_back(lo); out.push_back(hi); }
            else { out.push_back(hi); out.push_back(lo); }
        }
        return out;
    }
    UINT cp = (enc.kind == EncodingKind::Utf8) ? 65001 : enc.codepage;
    int need = WideCharToMultiByte(cp, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return out;
    out.resize((size_t)need);
    WideCharToMultiByte(cp, 0, text.c_str(), (int)text.size(), (LPSTR)out.data(), need, nullptr, nullptr);
    return out;
}
```

- [ ] **Step 4:** green; commit `feat(core): encode_text for typed input (TDD)`.

---

### Task 2: PieceTable — pieces, read, insert/erase, coalescing (TDD)

**Files:** Create `core/piece_table.h/.cpp`, `tests/test_piece_table.cpp` (add to vcxprojs).

- [ ] **Step 1: tests** — `tests/test_piece_table.cpp`:

```cpp
#include "doctest/doctest.h"
#include "core/piece_table.h"
#include <cstring>
#include <string>

using namespace fastpad;

// Original buffer backed by a string for tests.
static PieceTable make(const std::string& orig) {
    auto reader = [orig](uint64_t off, void* dst, size_t len) -> bool {
        if (off + len > orig.size()) return false;
        memcpy(dst, orig.data() + off, len);
        return true;
    };
    auto counter = [orig](uint64_t b, uint64_t e) -> uint64_t {
        uint64_t n = 0;
        for (uint64_t i = b; i < e; ++i) if (orig[(size_t)i] == '\n') n++;
        return n;
    };
    return PieceTable(orig.size(), reader, counter, LineStride::OneByte);
}

static std::string text_of(const PieceTable& pt) {
    std::string s((size_t)pt.size(), '\0');
    REQUIRE(pt.read(0, s.data(), s.size()));
    return s;
}

static std::vector<uint8_t> bytes(const char* s) { return {s, s + strlen(s)}; }

TEST_CASE("piecetable: starts as one original piece") {
    auto pt = make("hello");
    CHECK(pt.size() == 5);
    CHECK(text_of(pt) == "hello");
    CHECK(pt.pieceCount() == 1);
}

TEST_CASE("piecetable: insert middle / start / end") {
    auto pt = make("helLO");
    auto b = bytes("xx");
    pt.insert(3, b.data(), b.size());
    CHECK(text_of(pt) == "helxxLO");
    pt.insert(0, b.data(), b.size());
    CHECK(text_of(pt) == "xxhelxxLO");
    pt.insert(pt.size(), b.data(), b.size());
    CHECK(text_of(pt) == "xxhelxxLOxx");
}

TEST_CASE("piecetable: erase within / across pieces") {
    auto pt = make("abcdef");
    auto b = bytes("123");
    pt.insert(3, b.data(), b.size());           // abc123def
    pt.erase(2, 3);                              // ab 3def -> "ab3def"
    CHECK(text_of(pt) == "ab3def");
    pt.erase(0, pt.size());
    CHECK(pt.size() == 0);
    CHECK(text_of(pt) == "");
}

TEST_CASE("piecetable: sequential typing coalesces into one add piece") {
    auto pt = make("");
    for (char c : std::string("typed")) {
        uint8_t byte = (uint8_t)c;
        pt.insert(pt.size(), &byte, 1);
    }
    CHECK(text_of(pt) == "typed");
    CHECK(pt.pieceCount() == 1);                 // coalesced
}

TEST_CASE("piecetable: read spanning original and add pieces") {
    auto pt = make("aaabbb");
    auto b = bytes("XY");
    pt.insert(3, b.data(), b.size());            // aaaXYbbb
    char buf[4];
    REQUIRE(pt.read(2, buf, 4));
    CHECK(std::string(buf, 4) == "aXYb");
    CHECK_FALSE(pt.read(6, buf, 4));             // 6+4 > 8
}
```

- [ ] **Step 2:** RED.

- [ ] **Step 3:** `core/piece_table.h`:

```cpp
#pragma once
#include "core/line_index.h"      // LineStride
#include <cstdint>
#include <functional>
#include <vector>

namespace fastpad {

// Editable byte document: a sequence of pieces referencing either the
// immutable ORIGINAL buffer (read via callback - typically the mmap) or the
// append-only ADD buffer (RAM). Piece VECTOR by design (not a tree): edits
// memmove piece descriptors only; sub-ms at 100k+ pieces. Single-threaded.
class PieceTable {
public:
    using OrigReader = std::function<bool(uint64_t offset, void* dst, size_t len)>;
    // Count of line breaks in [begin, end) of the ORIGINAL buffer.
    using BreakCounter = std::function<uint64_t(uint64_t begin, uint64_t end)>;

    PieceTable(uint64_t originalSize, OrigReader readOriginal, BreakCounter countBreaks, LineStride stride);

    uint64_t size() const { return size_; }
    size_t pieceCount() const { return pieces_.size(); }
    bool read(uint64_t offset, void* dst, size_t len) const;

    void insert(uint64_t offset, const uint8_t* bytes, size_t len);
    void erase(uint64_t offset, uint64_t len);

    bool canUndo() const { return journalPos_ > 0; }
    bool canRedo() const { return journalPos_ < journal_.size(); }
    void undo();
    void redo();

    // Line breaks contained in [0, offset). For original pieces uses the
    // BreakCounter; add-piece breaks are pre-counted at insert.
    uint64_t lineBreaksBefore(uint64_t offset) const;
    uint64_t lineBreaksTotal() const;

private:
    enum class Buf : uint8_t { Original, Add };
    struct Piece { Buf buf; uint64_t offset; uint64_t length; uint64_t breaks; };
    struct Edit { size_t index; std::vector<Piece> removed; size_t insertedCount; };

    size_t findPiece(uint64_t offset, uint64_t* pieceStart) const;  // piece containing offset
    uint64_t countAddBreaks(const uint8_t* bytes, size_t len) const;
    uint64_t countPieceBreaksPrefix(const Piece& p, uint64_t prefixLen) const;
    void applyReplace(size_t index, size_t removeCount, std::vector<Piece> add, bool intoJournal);

    OrigReader readOrig_;
    BreakCounter countOrig_;
    LineStride stride_;
    std::vector<uint8_t> addBuf_;
    std::vector<Piece> pieces_;
    uint64_t size_ = 0;
    std::vector<Edit> journal_;
    size_t journalPos_ = 0;
    bool lastWasTypingAppend_ = false;   // coalescing state
};

} // namespace fastpad
```

`core/piece_table.cpp` — complete implementation:

```cpp
#include "core/piece_table.h"
#include <cassert>
#include <cstring>

namespace fastpad {

PieceTable::PieceTable(uint64_t originalSize, OrigReader readOriginal, BreakCounter countBreaks, LineStride stride)
    : readOrig_(std::move(readOriginal)), countOrig_(std::move(countBreaks)), stride_(stride) {
    if (originalSize > 0) {
        pieces_.push_back({Buf::Original, 0, originalSize, countOrig_ ? countOrig_(0, originalSize) : 0});
        size_ = originalSize;
    }
}

uint64_t PieceTable::countAddBreaks(const uint8_t* p, size_t len) const {
    uint64_t n = 0;
    if (stride_ == LineStride::OneByte) {
        for (size_t i = 0; i < len; ++i) if (p[i] == '\n') n++;
    } else {
        bool le = (stride_ == LineStride::TwoByteLE);
        for (size_t i = 0; i + 1 < len; i += 2) {
            uint16_t u = le ? (uint16_t)(p[i] | (p[i+1] << 8)) : (uint16_t)((p[i] << 8) | p[i+1]);
            if (u == '\n') n++;
        }
    }
    return n;
}

size_t PieceTable::findPiece(uint64_t offset, uint64_t* pieceStart) const {
    uint64_t pos = 0;
    for (size_t i = 0; i < pieces_.size(); ++i) {
        if (offset < pos + pieces_[i].length) { *pieceStart = pos; return i; }
        pos += pieces_[i].length;
    }
    *pieceStart = pos;
    return pieces_.size();                       // offset == size_ (end)
}

bool PieceTable::read(uint64_t offset, void* dst, size_t len) const {
    if (len == 0) return true;
    if (offset + len > size_ || offset + len < offset) return false;
    uint8_t* out = (uint8_t*)dst;
    uint64_t pieceStart = 0;
    size_t i = findPiece(offset, &pieceStart);
    uint64_t inPiece = offset - pieceStart;
    while (len > 0 && i < pieces_.size()) {
        const Piece& p = pieces_[i];
        size_t take = (size_t)((p.length - inPiece < len) ? p.length - inPiece : len);
        if (p.buf == Buf::Add) {
            memcpy(out, addBuf_.data() + p.offset + inPiece, take);
        } else {
            if (!readOrig_(p.offset + inPiece, out, take)) return false;
        }
        out += take; len -= take;
        inPiece = 0; i++;
    }
    return len == 0;
}

void PieceTable::applyReplace(size_t index, size_t removeCount, std::vector<Piece> add, bool intoJournal) {
    Edit e;
    e.index = index;
    e.removed.assign(pieces_.begin() + index, pieces_.begin() + index + removeCount);
    e.insertedCount = add.size();
    for (const Piece& p : e.removed) size_ -= p.length;
    for (const Piece& p : add) size_ += p.length;
    pieces_.erase(pieces_.begin() + index, pieces_.begin() + index + removeCount);
    pieces_.insert(pieces_.begin() + index, add.begin(), add.end());
    if (intoJournal) {
        journal_.resize(journalPos_);            // drop redo tail
        journal_.push_back(std::move(e));
        journalPos_ = journal_.size();
    }
}

void PieceTable::insert(uint64_t offset, const uint8_t* bytes, size_t len) {
    if (len == 0) return;
    assert(offset <= size_);
    uint64_t breaks = countAddBreaks(bytes, len);

    // Coalesce: appending right after the previous typed insert extends the
    // last add piece in place (journal entry stays valid: undo still removes
    // the whole grown piece range).
    if (lastWasTypingAppend_ && journalPos_ == journal_.size() && !journal_.empty() && !pieces_.empty()) {
        uint64_t pieceStart = 0;
        size_t i = findPiece(offset == size_ ? size_ - 1 : offset, &pieceStart);
        Edit& last = journal_.back();
        if (i < pieces_.size() && pieces_[i].buf == Buf::Add &&
            last.insertedCount >= 1 && i == last.index + last.insertedCount - 1 &&
            pieces_[i].offset + pieces_[i].length == addBuf_.size() &&
            offset == pieceStart + pieces_[i].length) {
            addBuf_.insert(addBuf_.end(), bytes, bytes + len);
            pieces_[i].length += len;
            pieces_[i].breaks += breaks;
            size_ += len;
            return;
        }
    }

    uint64_t addOff = addBuf_.size();
    addBuf_.insert(addBuf_.end(), bytes, bytes + len);
    Piece np{Buf::Add, addOff, len, breaks};

    uint64_t pieceStart = 0;
    size_t i = findPiece(offset, &pieceStart);
    std::vector<Piece> repl;
    size_t removeCount = 0;
    if (i < pieces_.size() && offset > pieceStart) {
        // split pieces_[i]
        const Piece& p = pieces_[i];
        uint64_t leftLen = offset - pieceStart;
        Piece left{p.buf, p.offset, leftLen, countPieceBreaksPrefix(p, leftLen)};
        Piece right{p.buf, p.offset + leftLen, p.length - leftLen, p.breaks - left.breaks};
        repl = {left, np, right};
        removeCount = 1;
    } else {
        repl = {np};
        removeCount = 0;
    }
    applyReplace(i, removeCount, std::move(repl), true);
    lastWasTypingAppend_ = true;
}

uint64_t PieceTable::countPieceBreaksPrefix(const Piece& p, uint64_t prefixLen) const {
    if (prefixLen == 0) return 0;
    if (prefixLen == p.length) return p.breaks;
    if (p.buf == Buf::Original)
        return countOrig_ ? countOrig_(p.offset, p.offset + prefixLen) : 0;
    return countAddBreaks(addBuf_.data() + p.offset, (size_t)prefixLen);
}

void PieceTable::erase(uint64_t offset, uint64_t len) {
    if (len == 0 || offset >= size_) return;
    if (offset + len > size_) len = size_ - offset;
    lastWasTypingAppend_ = false;

    uint64_t firstStart = 0;
    size_t first = findPiece(offset, &firstStart);
    uint64_t lastStart = 0;
    size_t last = findPiece(offset + len - 1, &lastStart);

    std::vector<Piece> repl;
    if (offset > firstStart) {                   // keep head of first piece
        const Piece& p = pieces_[first];
        uint64_t keep = offset - firstStart;
        repl.push_back({p.buf, p.offset, keep, countPieceBreaksPrefix(p, keep)});
    }
    {
        const Piece& p = pieces_[last];
        uint64_t cutEnd = offset + len - lastStart;     // bytes removed from this piece's head
        if (cutEnd < p.length) {                  // keep tail of last piece
            uint64_t headBreaks = countPieceBreaksPrefix(p, cutEnd);
            repl.push_back({p.buf, p.offset + cutEnd, p.length - cutEnd, p.breaks - headBreaks});
        }
    }
    applyReplace(first, last - first + 1, std::move(repl), true);
}

void PieceTable::undo() {
    if (!canUndo()) return;
    lastWasTypingAppend_ = false;
    Edit& e = journal_[--journalPos_];
    // swap: current [index, index+insertedCount) <-> e.removed
    std::vector<Piece> current(pieces_.begin() + e.index, pieces_.begin() + e.index + e.insertedCount);
    for (const Piece& p : current) size_ -= p.length;
    for (const Piece& p : e.removed) size_ += p.length;
    pieces_.erase(pieces_.begin() + e.index, pieces_.begin() + e.index + e.insertedCount);
    pieces_.insert(pieces_.begin() + e.index, e.removed.begin(), e.removed.end());
    size_t removedCount = e.removed.size();
    e.removed = std::move(current);
    e.insertedCount = removedCount;              // entry now describes the redo swap
}

void PieceTable::redo() {
    if (!canRedo()) return;
    lastWasTypingAppend_ = false;
    Edit& e = journal_[journalPos_++];
    std::vector<Piece> current(pieces_.begin() + e.index, pieces_.begin() + e.index + e.insertedCount);
    for (const Piece& p : current) size_ -= p.length;
    for (const Piece& p : e.removed) size_ += p.length;
    pieces_.erase(pieces_.begin() + e.index, pieces_.begin() + e.index + e.insertedCount);
    pieces_.insert(pieces_.begin() + e.index, e.removed.begin(), e.removed.end());
    size_t removedCount = e.removed.size();
    e.removed = std::move(current);
    e.insertedCount = removedCount;
}

uint64_t PieceTable::lineBreaksBefore(uint64_t offset) const {
    uint64_t pos = 0, breaks = 0;
    for (const Piece& p : pieces_) {
        if (offset <= pos) break;
        if (offset >= pos + p.length) { breaks += p.breaks; pos += p.length; continue; }
        breaks += countPieceBreaksPrefix(p, offset - pos);
        break;
    }
    return breaks;
}

uint64_t PieceTable::lineBreaksTotal() const {
    uint64_t n = 0;
    for (const Piece& p : pieces_) n += p.breaks;
    return n;
}

} // namespace fastpad
```

- [ ] **Step 4:** green. **Step 5:** commit `feat(core): piece table - insert/erase/read with typing coalescing (TDD)`.

---

### Task 3: PieceTable undo/redo + break counts (TDD)

**Files:** Modify `tests/test_piece_table.cpp` only (implementation landed in Task 2 — these tests VERIFY it; expect possible fixes).

- [ ] **Step 1: tests** (append):

```cpp
TEST_CASE("piecetable: undo/redo round-trip") {
    auto pt = make("abcdef");
    auto b = bytes("123");
    pt.insert(3, b.data(), b.size());            // abc123def
    pt.erase(0, 2);                              // c123def
    CHECK(text_of(pt) == "c123def");
    pt.undo();
    CHECK(text_of(pt) == "abc123def");
    pt.undo();
    CHECK(text_of(pt) == "abcdef");
    CHECK_FALSE(pt.canUndo());
    pt.redo();
    CHECK(text_of(pt) == "abc123def");
    pt.redo();
    CHECK(text_of(pt) == "c123def");
    CHECK_FALSE(pt.canRedo());
}

TEST_CASE("piecetable: new edit truncates redo tail") {
    auto pt = make("xy");
    auto b = bytes("A");
    pt.insert(0, b.data(), 1);                   // Axy
    pt.undo();                                   // xy
    auto c = bytes("B");
    pt.insert(2, c.data(), 1);                   // xyB
    CHECK_FALSE(pt.canRedo());
    CHECK(text_of(pt) == "xyB");
}

TEST_CASE("piecetable: coalesced typing undoes as one unit") {
    auto pt = make("ab");
    for (char ch : std::string("123")) { uint8_t u = (uint8_t)ch; pt.insert(2 + (&ch - &std::string("123")[0]), &u, 1); }
    // simpler deterministic loop:
    auto pt2 = make("ab");
    std::string typed = "123";
    for (size_t i = 0; i < typed.size(); ++i) { uint8_t u = (uint8_t)typed[i]; pt2.insert(2 + i, &u, 1); }
    CHECK(text_of(pt2) == "ab123");
    pt2.undo();
    CHECK(text_of(pt2) == "ab");                 // ONE undo removes all coalesced typing
}

TEST_CASE("piecetable: line break accounting across edits") {
    auto pt = make("a\nb\nc");                   // 2 breaks
    CHECK(pt.lineBreaksTotal() == 2);
    CHECK(pt.lineBreaksBefore(2) == 1);
    auto b = bytes("x\ny\n");
    pt.insert(1, b.data(), b.size());            // a x\ny\n \nb\nc -> total 4
    CHECK(pt.lineBreaksTotal() == 4);
    CHECK(pt.lineBreaksBefore(pt.size()) == 4);
    pt.erase(0, pt.size());
    CHECK(pt.lineBreaksTotal() == 0);
    pt.undo();
    CHECK(pt.lineBreaksTotal() == 4);
}
```
NOTE the first variant inside "coalesced typing" test is junk (pointer arithmetic on a temporary) — write ONLY the pt2 loop version; delete the broken variant.

- [ ] **Step 2:** run; fix `PieceTable` where tests expose bugs (likely candidates: journal entry mutation in undo/redo, coalescing guard). Tests are the spec.
- [ ] **Step 3:** green → commit `test(core): piece table undo/redo and break accounting verified`.

---

### Task 4: Document v2 — editable layer + core line scans (TDD)

**Files:** Modify `core/document.h/.cpp`; create `tests/test_document_edit.cpp`; modify `app/renderer.cpp` ONLY to delegate (Task 7 does the visual work).

API additions to `Document` (header contracts):

```cpp
    // ---- M2 editing (UI-thread-affine like everything else) ----
    bool dirty() const;
    bool read(uint64_t offset, void* dst, size_t len);       // pieces when dirty, else mmap
    void insertText(uint64_t offset, const std::wstring& text);  // encodes via encoding()
    void insertBytes(uint64_t offset, const uint8_t* bytes, size_t len);
    void eraseRange(uint64_t offset, uint64_t len);
    bool canUndo() const; bool canRedo() const;
    void undo(); void redo();
    // EOL byte sequence for Enter, from the first break found in the first
    // 64 KB (CRLF default when none found).
    std::vector<uint8_t> eolBytes();
    // Steps one CHARACTER forward/backward from offset (UTF-8 continuation
    // aware; UTF-16 steps 2; ANSI DBCS steps via IsDBCSLeadByteEx best-effort).
    uint64_t charStepForward(uint64_t offset);
    uint64_t charStepBackward(uint64_t offset);
    // Line-break scans over the LIVE content (pieces when dirty). Full-unit
    // comparison for UTF-16 (fixes M2-BACKLOG item 2). Backward scan capped
    // at 1 MB (returns scan floor when no break found).
    uint64_t findNextBreak(uint64_t offset);    // offset AFTER the next break; size() if none
    uint64_t findPrevBreak(uint64_t offset);    // line start at/before offset
    // Breaks in [0, offset) of live content; uses LineIndex when clean and
    // indexed, PieceTable counts when dirty. nullopt when the range needs
    // unindexed original scanning beyond 1 MB.
    std::optional<uint64_t> lineBreaksBefore(uint64_t offset);
```

Implementation rules:
- `ensurePieces()`: lazily constructs `PieceTable(size(), reader-over-mmap, counter, strideFor(enc_))` on first edit. Reader = lambda calling `file_->read`. BreakCounter = lambda: if range fully `<= index_->indexedBytes()` → `lineOfOffset(end) - lineOfOffset(begin)` (careful: lineOfOffset counts breaks at positions <= off; the diff over [begin,end) is `lineOfOffset(end-1) - (begin ? lineOfOffset(begin-1) : 0)` — derive precisely and unit-test); else scan raw via `file_->read` in 64 KB chunks WHEN the range is ≤ 16 MB, else return the indexed part + scan remainder (ranges that large only occur for the initial whole-file piece, whose count may also be deferred: acceptable to pass `0` and mark `breaksValid_=false` → `lineBreaksBefore` returns nullopt while not fully indexed; SIMPLEST correct rule: PieceTable created with counter that works; if index incomplete at first-edit time, Document waits? NO — never block. Rule: counter returns exact when resolvable (indexed or ≤16 MB scan), else returns 0 and sets a Document flag `lineNumbersApproximate_ = true`; `lineBreaksBefore` returns nullopt when that flag is set. Status bar then shows Off only. Document the rule in the header.)
- `decodeAt`: when dirty, read via pieces into the buffer (same decode_window flow).
- `findNextBreak`/`findPrevBreak`: byte buffers via `read()` in 64 KB chunks; OneByte: state machine identical to renderer's old logic (CR/LF/CRLF); TwoByte strides: iterate ALIGNED 16-bit units (offset parity from file start), compare full units against 0x000A/0x000D — no false positives on CJK.
- `setEncoding` while dirty: refuse (return false; change signature to `bool setEncoding(...)`) — reinterpreting bytes mid-edit is a footgun; app shows a message. (Update the existing call sites/tests.)
- `eolBytes`: scan first 64 KB of LIVE content for the first break; return its byte sequence in the document encoding (CRLF default). Cache per open.

- [ ] **Step 1: tests** — `tests/test_document_edit.cpp` (write all BEFORE implementing; key cases):

```cpp
#include "doctest/doctest.h"
#include "core/document.h"
#include "test_helpers.h"
#include <string>

using namespace fastpad;

static std::wstring decode_all(Document& d) {
    std::wstring out;
    d.decodeAt(0, (size_t)d.size() + 16, out);
    return out;
}

TEST_CASE("document edit: insertText/eraseRange/undo/redo over UTF-8 file") {
    TempFileGuard g{ write_temp_file({'h','e','l','l','o','\n','w','o','r','l','d'}) };
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    d.waitForIndex();
    CHECK_FALSE(d.dirty());

    d.insertText(5, L" there");                 // hello there\nworld
    CHECK(d.dirty());
    CHECK(d.size() == 17);
    CHECK(decode_all(d) == L"hello there\nworld");

    d.eraseRange(0, 6);                          // there\nworld
    CHECK(decode_all(d) == L"there\nworld");

    d.undo(); d.undo();
    CHECK(decode_all(d) == L"hello\nworld");
    CHECK_FALSE(d.canUndo());
    d.redo();
    CHECK(decode_all(d) == L"hello there\nworld");
}

TEST_CASE("document edit: typed Cyrillic encodes to the file's codepage") {
    TempFileGuard g{ write_temp_file({'a','b'}) };
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    d.setEncodingUnchecked({EncodingKind::Ansi, 1251, 0});   // helper for tests OR open file with cp1251 content; see note
    d.insertText(1, L"\x042F");                  // Я -> 0xDF in cp1251
    uint8_t buf[3];
    REQUIRE(d.read(0, buf, 3));
    CHECK(buf[0] == 'a'); CHECK(buf[1] == 0xDF); CHECK(buf[2] == 'b');
}

TEST_CASE("document edit: findNextBreak/findPrevBreak on live edited content") {
    TempFileGuard g{ write_temp_file({'a','a','\n','b','b'}) };
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    d.waitForIndex();
    CHECK(d.findNextBreak(0) == 3);
    d.insertText(0, L"x\n");                     // x\naa\nbb
    CHECK(d.findNextBreak(0) == 2);
    CHECK(d.findNextBreak(2) == 5);
    CHECK(d.findPrevBreak(6) == 5);
    CHECK(d.findPrevBreak(1) == 0);
}

TEST_CASE("document edit: UTF-16 break scan ignores 0x0A inside CJK units") {
    // U+4E0A = bytes 0A 4E in LE; must NOT be a line break.
    std::vector<uint8_t> b{0xFF,0xFE, 0x0A,0x4E, '\n',0, 'z',0};
    TempFileGuard g{ write_temp_file(b) };
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    d.waitForIndex();
    CHECK(d.findNextBreak(2) == 6);              // breaks after the real 000A unit
}

TEST_CASE("document edit: eolBytes detects CRLF vs LF; default CRLF") {
    TempFileGuard a{ write_temp_file({'x','\r','\n','y'}) };
    Document d1; REQUIRE(d1.open(a.path.c_str(), nullptr));
    CHECK(d1.eolBytes() == std::vector<uint8_t>{'\r','\n'});
    TempFileGuard b{ write_temp_file({'x','\n','y'}) };
    Document d2; REQUIRE(d2.open(b.path.c_str(), nullptr));
    CHECK(d2.eolBytes() == std::vector<uint8_t>{'\n'});
    TempFileGuard c{ write_temp_file({'x','y'}) };
    Document d3; REQUIRE(d3.open(c.path.c_str(), nullptr));
    CHECK(d3.eolBytes() == std::vector<uint8_t>{'\r','\n'});
}

TEST_CASE("document edit: charStep over UTF-8 multibyte") {
    TempFileGuard g{ write_temp_file({'a', 0xD0,0xAF, 'b'}) };   // a Я b in UTF-8
    Document d; REQUIRE(d.open(g.path.c_str(), nullptr));
    CHECK(d.charStepForward(0) == 1);
    CHECK(d.charStepForward(1) == 3);
    CHECK(d.charStepBackward(3) == 1);
    CHECK(d.charStepBackward(4) == 3);
}

TEST_CASE("document edit: lineBreaksBefore exact on small indexed file") {
    TempFileGuard g{ write_temp_file({'a','\n','b','\n','c'}) };
    Document d; REQUIRE(d.open(g.path.c_str(), nullptr));
    d.waitForIndex();
    d.insertText(0, L"q\n");
    auto n = d.lineBreaksBefore(4);              // q\na\n -> 2 breaks before offset 4
    REQUIRE(n.has_value());
    CHECK(*n == 2);
}

TEST_CASE("document edit: setEncoding refused while dirty") {
    TempFileGuard g{ write_temp_file({'a'}) };
    Document d; REQUIRE(d.open(g.path.c_str(), nullptr));
    d.insertText(0, L"x");
    CHECK_FALSE(d.setEncoding({EncodingKind::Utf16LE, 1200, 0}));
}
```
Note on `setEncodingUnchecked`: add it as a small public test-support method (sets enc_ without the dirty check / reindex — document it as test-only in the comment), OR write the cp1251 bytes into the file so detection lands on Ansi naturally and call the normal path before editing. Choose ONE; adjust the test accordingly.

- [ ] **Step 2:** RED. **Step 3:** implement per the rules above (also: renderer's `nextLineStart`/`prevLineStart` bodies replaced by `doc_->findNextBreak(...)` / `doc_->findPrevBreak(...)` calls — delete the old raw-scan code; `setEncoding` call sites in `app_window.cpp` handle the new bool with a MessageBox "Save or undo changes before changing encoding."). Existing M1 tests must stay green (the `document: reopen with encoding override` test calls setEncoding on a CLEAN doc — still true).
- [ ] **Step 4:** green (M1 24 cases + ~8 new + piece-table cases). Commit `feat(core): editable Document - pieces, typed input, core line scans (TDD)`.

---

### Task 5: Save engine (TDD)

**Files:** Create `core/save.h/.cpp`, `tests/test_save.cpp` (add to vcxprojs); small `Document` addition.

`core/save.h`:

```cpp
#pragma once
#include "core/document.h"
#include <atomic>
#include <functional>
#include <optional>
#include <string>

namespace fastpad {

struct SaveOptions {
    std::wstring targetPath;                       // empty = document's own path
    std::optional<EncodingInfo> transcodeTo;       // nullopt = keep bytes as-is
};

// Streams the document (4 MB chunks) to "<target>.fptmp" in the target's
// directory, then atomically replaces the target (ReplaceFileW when it exists,
// MoveFileExW otherwise). Returns false + error on failure or cancel; the
// target is never left half-written. Synchronous - run from a worker if the UI
// must stay live (M2 ships it synchronous with a progress callback pumping).
bool save_document(Document& doc, const SaveOptions& opts,
                   const std::function<void(uint64_t done, uint64_t total)>& progress,
                   const std::atomic<bool>& cancel, std::wstring* error);

} // namespace fastpad
```

Implementation rules (`core/save.cpp`):
- Preflight: `GetDiskFreeSpaceExW` on the target dir; require free >= doc.size() + 64 MB slack; clear error message otherwise.
- No transcode: loop `doc.read(off, buf 4MB)` → WriteFile to temp. With transcode: decode each chunk with carry (use `decode_window`'s consumed to handle split tails: re-read unconsumed bytes into next chunk), `encode_text` to target encoding, write; prepend target BOM when `transcodeTo->bomBytes > 0`; skip source BOM bytes at offset 0 (`doc.encoding().bomBytes`).
- Cancel check per chunk → delete temp, return false with error L"Cancelled".
- `FlushFileBuffers` before close; `ReplaceFileW(target, temp, nullptr, 0, ...)` when target exists else `MoveFileExW(temp, target, MOVEFILE_REPLACE_EXISTING)`.
- Document addition: `bool reopenAfterSave(const wchar_t* savedPath, std::wstring* err)` → `close()` + `open(savedPath, ...)` (drops pieces; the saved file IS the new original). App calls it post-save.

- [ ] **Step 1: tests** — `tests/test_save.cpp`:

```cpp
#include "doctest/doctest.h"
#include "core/document.h"
#include "core/save.h"
#include "test_helpers.h"
#include <atomic>

using namespace fastpad;

static std::vector<uint8_t> file_bytes(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    LARGE_INTEGER sz{}; GetFileSizeEx(h, &sz);
    std::vector<uint8_t> v((size_t)sz.QuadPart);
    DWORD r = 0;
    if (!v.empty()) ReadFile(h, v.data(), (DWORD)v.size(), &r, nullptr);
    CloseHandle(h);
    return v;
}

TEST_CASE("save: edited document saves in place atomically and reopens clean") {
    TempFileGuard g{ write_temp_file({'a','b','c'}) };
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    d.insertText(3, L"XY");
    std::atomic<bool> cancel{false};
    std::wstring err;
    REQUIRE(save_document(d, {L"", std::nullopt}, nullptr, cancel, &err));
    REQUIRE(d.reopenAfterSave(g.path.c_str(), &err));
    CHECK_FALSE(d.dirty());
    CHECK(file_bytes(g.path) == std::vector<uint8_t>{'a','b','c','X','Y'});
}

TEST_CASE("save: save-as to a new path leaves the original untouched") {
    TempFileGuard g{ write_temp_file({'1','2'}) };
    std::wstring target = g.path + L".out";
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    d.insertText(2, L"3");
    std::atomic<bool> cancel{false};
    std::wstring err;
    REQUIRE(save_document(d, {target, std::nullopt}, nullptr, cancel, &err));
    CHECK(file_bytes(g.path) == std::vector<uint8_t>{'1','2'});
    CHECK(file_bytes(target) == std::vector<uint8_t>{'1','2','3'});
    DeleteFileW(target.c_str());
}

TEST_CASE("save: transcode UTF-8 -> UTF-16LE with BOM") {
    TempFileGuard g{ write_temp_file({'h','i','\n',0xD0,0xAF}) };   // "hi\nЯ" UTF-8
    std::wstring target = g.path + L".u16";
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    std::atomic<bool> cancel{false};
    std::wstring err;
    REQUIRE(save_document(d, {target, EncodingInfo{EncodingKind::Utf16LE, 1200, 2}}, nullptr, cancel, &err));
    auto b = file_bytes(target);
    CHECK(b == std::vector<uint8_t>{0xFF,0xFE,'h',0,'i',0,'\n',0,0x2F,0x04});
    DeleteFileW(target.c_str());
}

TEST_CASE("save: cancel leaves no temp and target unchanged") {
    TempFileGuard g{ write_temp_file(std::vector<uint8_t>(8 * 1024 * 1024, 'x')) };
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    d.insertText(0, L"y");
    std::atomic<bool> cancel{true};               // pre-cancelled
    std::wstring err;
    CHECK_FALSE(save_document(d, {L"", std::nullopt}, nullptr, cancel, &err));
    CHECK(file_bytes(g.path).size() == 8 * 1024 * 1024);
    CHECK(file_bytes(g.path)[0] == 'x');
}
```

- [ ] **Step 2:** RED → implement → green (progress callback exercised implicitly; verify temp file absence after each test via `Test-Path` spot check if convenient). **Step 3:** commit `feat(core): streaming save with transcode and atomic replace (TDD)`.

---

### Task 6: Renderer — caret, selection, hit testing

**Files:** Modify `app/renderer.h/.cpp`. Manual smoke. The renderer becomes editable-aware but INPUT arrives in Task 7.

Required additions (signatures the next task depends on):

```cpp
    // caret/selection (byte offsets into the live document)
    uint64_t caret() const { return caret_; }
    void setCaret(uint64_t offset, bool extendSelection);   // clamps; snaps INTO the visible char grid
    bool hasSelection() const { return selAnchor_ != caret_; }
    uint64_t selBegin() const; uint64_t selEnd() const;
    void clearSelection() { selAnchor_ = caret_; }
    std::pair<bool, uint64_t> hitTest(int px, int py);      // client px -> byte offset (false if no doc)
    void ensureCaretVisible();                              // scrolls topOffset_ so the caret line shows
    void invalidate() { InvalidateRect(hwnd_, nullptr, FALSE); }
```

Implementation essentials:
- `onPaint` builds `std::vector<VisibleLine>` where `VisibleLine { uint64_t byteStart, byteEnd; std::wstring text; std::vector<uint64_t> charByte; IDWriteTextLayout* layout; float y; }`:
  - walk lines from `topOffset_` using `doc_->findNextBreak`; per line `doc_->decodeAt(byteStart, lineByteLen, text)` (cap 4096 chars as before);
  - `charByte[i]` = byte offset of text[i]: build by stepping `doc_->charStepForward` from byteStart per decoded char (UTF-16 surrogate pairs: a pair consumes one step of 2 units — acceptable M2 approximation: step once per wchar; document it);
  - `dwFactory_->CreateTextLayout(text…, format_, width, lineHeight_, &layout)`; cache only within the paint (Release at end) — M2 keeps it simple; measure before optimizing.
- Caret draw: line containing `caret_` → `layout->HitTestTextPosition(charIndex, FALSE, &x, &y, &metrics)` → `FillRectangle` 1.5px bar. Caret blink: `SetTimer` 530 ms toggling `caretOn_` + invalidate (timer id 2, owned by renderer via its hwnd).
- Selection draw: per visible line compute overlap [selBegin, selEnd) ∩ [byteStart, byteEnd) → char range via charByte binary search → `layout->HitTestTextRange(start, len, 0, 0, hitArr, …)` → semi-transparent blue `FillRectangle`s UNDER the text.
- `hitTest`: y → line slot (clamp); x → `layout->HitTestPoint` → char index (+trailing) → `charByte[...]` (end of line → byteEnd before the break).
- `ensureCaretVisible`: if caret < topOffset_ → `goToOffset(caret_)`; if below last visible line, advance topOffset_ by findNextBreak until visible (cap a page).
- `setDocument` resets caret/selection to bomBytes.

Manual smoke (with a temporary harness in app_window: temporarily route left-click to `setCaret(hitTest(...))` if Task 7 not yet merged — or just defer visual confirmation to Task 7's smoke; building clean + tests green is this task's gate).

- [ ] Build clean; tests stay green; commit `feat(app): renderer caret/selection/hit-testing over live document`.

---

### Task 7: Input — typing, editing keys, mouse selection, clipboard

**Files:** Modify `app/app_window.cpp` (+ `.h` if a helper member is needed).

Wiring (all guarded by `doc_ && renderer_`):
- **Edit menu**: Undo (Ctrl+Z, IDM_UNDO=202), Redo (Ctrl+Y, 203), Cut (Ctrl+X, 204), Copy (Ctrl+C, 205), Paste (Ctrl+V, 206), Select All (Ctrl+A, 207) — append to the accelerator table in `main.cpp` (keep IDs in sync with the app_window enum; add a shared header `app/commands.h` with the enum to kill the duplication noted in M2-BACKLOG).
- **WM_CHAR**: printable chars (`wch >= 0x20 || wch == L'\t'`): if selection → `eraseRange(selBegin, selEnd-selBegin)` first; `doc_->insertText(caret, std::wstring(1, wch))`; advance caret by the encoded byte length (use `doc_->size()` delta or `encode_text` length); `ensureCaretVisible(); invalidate(); updateStatusBar(); updateTitleDirty();`. Enter (`wch == L'\r'`): insert `doc_->eolBytes()` via `insertBytes`. Backspace (`wch == 0x08`): selection ? erase selection : erase `[charStepBackward(caret), caret)`.
- **WM_KEYDOWN**: VK_DELETE (selection ? erase : erase `[caret, charStepForward(caret))`); arrows move caret (`charStepBackward/Forward`; Up/Down: target line via `findPrevBreak`/`findNextBreak` preserving an approximate column = byte delta within line — M2 keeps byte-column; note in code); Home/End (line start via `findPrevBreak(caret)`, line end = `findNextBreak(caret)` minus the break bytes); SHIFT held (`GetKeyState(VK_SHIFT) < 0`) → `setCaret(..., true)` extends selection. The M1 PgUp/PgDn/scroll keys keep working; plain arrows now move the CARET (scrolling via wheel/PgUp/PgDn only) — matches notepad.
- **Mouse**: WM_LBUTTONDOWN → `SetCapture`, `setCaret(hitTest(...), shiftHeld)`; WM_MOUSEMOVE while captured → `setCaret(hitTest(...), true)`; WM_LBUTTONUP → `ReleaseCapture`.
- **Clipboard**: Copy = selection bytes → decode whole selection (cap 1 GB with a MessageBox warning per spec) → `CF_UNICODETEXT` (OpenClipboard/EmptyClipboard/GlobalAlloc GMEM_MOVEABLE/SetClipboardData). Cut = Copy + erase. Paste = GetClipboardData(CF_UNICODETEXT) → normalize lone `\n`/`\r` pairs? (M2: insert as-is via `insertText` after converting `\r\n`→doc eol? Keep SIMPLE: replace any `\r\n` in pasted text with `\n` then replace `\n` with the doc's eol sequence — implement via insertBytes of the converted byte string; one helper + 1 unit test in `test_document_edit.cpp` for the conversion helper `normalize_paste(text, eolBytes, enc)->bytes` placed in core/document).
- **Dirty title**: `updateTitleDirty()` prefixes `*` when `doc_->dirty()` (and shows the open-ms suffix only when clean — keep simple: title = `(*<path> | <path>) - FastPad`).
- **Close guard**: WM_CLOSE → if dirty, MessageBox Yes/No/Cancel "Save changes?" (Yes → save flow from Task 8 — wire after Task 8; until then Yes acts as No with a comment; final wiring in Task 8).
- **Encoding menu while dirty**: `setEncoding` now returns bool — show "Save or undo changes before switching encoding." on false.

Manual smoke: type/edit/select/copy/paste in a small file; type at the END of a 1 GB+ sparse file (caret nav + typing must stay instant); Ctrl+Z chain.

- [ ] Build; tests green; commit `feat(app): editing input - typing, selection, clipboard, undo/redo`.

---

### Task 8: Save UX

**Files:** Modify `app/app_window.cpp`, `app/main.cpp` (accels: Ctrl+S=IDM_SAVE 103, Ctrl+Shift+S=IDM_SAVEAS 104).

- File menu gains Save / Save As… above Exit. Save: no-op when clean; runs `save_document` with a modal progress window for docs > 64 MB (reuse the goto-dialog pattern: small popup with a progress bar (msctls_progress32) + Cancel button toggling the atomic; pump messages between chunks via the progress callback calling a `pump()` that does PeekMessage/Dispatch — the save engine is synchronous by design). For small docs just call it inline (fast).
- After successful save: `doc_->reopenAfterSave(path)`, `renderer_->setDocument(doc_.get())` (caret resets — acceptable M2; note), title cleans, status refreshes.
- Save As: GetSaveFileNameW (OFN_OVERWRITEPROMPT) + a follow-up MessageBox asking "Keep current encoding?" Yes/No — No shows the encoding picker via a simple submenu-like chooser: M2 keeps it minimal: Save As always keeps current encoding; transcode is exposed via Encoding menu items "Save As UTF-8…" / "Save As UTF-16 LE…" which preset `transcodeTo` then run the same dialog. (Three menu items, one code path.)
- Wire the WM_CLOSE Yes branch to the save flow; Cancel aborts close.

Manual smoke: edit→Ctrl+S→file content verified in another viewer; Save As new file; Save As UTF-16 of a UTF-8 doc → reopen → detected as UTF-16; cancel a big save → original intact.

- [ ] Build; tests green; commit `feat(app): save/save-as with transcode and close guard`.

---

### Task 9: Final review + docs

- [ ] Full suite Debug + Release; fix anything flagged.
- [ ] README: editing keys, save behavior (streaming, atomic, transient disk = file size), the RAM-paste limitation, encoding-switch-requires-clean rule. Update `docs/superpowers/M2-BACKLOG.md`: mark item 2 fixed (core scans), item 1 fixed if Task 6 reordered setDocument (DO fix it there — one line), carry the rest + new known-limits (byte-column arrows, caret-reset-on-save, WM_CHAR-level IME).
- [ ] Dispatch final whole-M2 review (controller does this); fix Criticals; commit `docs: M2 complete`.

---

## Notes / risks

- **PieceTable is the correctness keystone** — its tests (Tasks 2-3) must be merciless; everything else trusts it.
- **Undo journal entries mutate in place on undo/redo** (swap semantics) — subtle; the round-trip tests pin it.
- **Coalescing guard**: only extends when the edit appends to the LAST journal entry's final add piece AND the add piece tail abuts addBuf_ end AND no redo tail exists. When in doubt, not coalescing is always correct (just more pieces/undo steps).
- **Renderer per-paint TextLayout creation** is O(visible lines) — fine; do NOT cache across paints in M2.
- **Save reopens the document** — pin pointers and caret die; renderer must re-setDocument (Tasks 6/8 handle).
- Surrogate pairs step as 2 separate wchar units in charByte mapping — caret can land mid-pair in exotic text; M3 polish.
- The save progress dialog pumps messages re-entrantly — guard against re-entering save (disable menu during save via a `saving_` flag).
