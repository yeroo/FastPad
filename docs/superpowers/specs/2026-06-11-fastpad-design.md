# FastPad - Design

**Date:** 2026-06-11
**Repo:** FastPad (personal project; local repo, no Shell org remote)
**Working name:** FastPad (display + exe name; trivially renameable)

## Problem

Windows Notepad has become slow and bloated (AI features); nothing free combines
"simple as classic notepad" with "opens files of any size" and "reads any
encoding". Viewers exist (klogg - read-only), giant-file editors exist
(EmEditor - commercial, featureful); the gap is a free, minimal, blazing-fast
NOTEPAD-grade editor that does not care about file size.

## Requirements (verbatim intent)

1. Light-speed fast: cold start < 50 ms target; file open is O(1) in file size.
2. Bare minimum features - classic notepad scope, nothing smart.
3. Opens ANY file size (GB/TB - anything NTFS holds).
4. Blazing navigation in huge files (scroll/jump instantly).
5. Reads any encoding (the must-have): detection + manual override + save-as.
6. Registered so it appears in Open With suggestions / default-app candidates
   for text files.
7. FULL editing at any size (user decision - not a viewer, not a hybrid).

## Core architecture

### Piece tree over memory-mapped file
- Original file: opened read-only with FILE_SHARE_READ, memory-mapped via
  on-demand 64 MB views with a small LRU cache. The file is NEVER loaded;
  opening 100 GB costs milliseconds and a few MB of working set.
- Edits: append-only add-buffer in RAM, spilling to a temp file beyond a
  threshold (e.g. 256 MB of accumulated edits).
- Document model: piece tree (balanced tree of {buffer, offset, length}
  spans - the VS Code structure). O(log n) insert/delete/position lookup at
  arbitrary offsets regardless of file size. Undo/redo = inverse piece
  operations on a journal stack.
- Each tree node caches the newline count of its span, so line <-> offset is
  O(log n) once line positions are known.

### Background line indexing
- A 100 GB file cannot be line-scanned at open. A background thread scans the
  original buffer once (sequential, ~disk speed), recording line starts in
  delta-encoded chunked arrays; progress shown in the status bar.
- Before/during indexing: scrolling and goto work by byte offset; the visible
  window finds its local line breaks on the fly. After indexing: exact line
  numbers, line-accurate scrollbar, goto-line.

### Save = streaming rewrite
- Save streams the piece sequence into a temp file in the target directory,
  then atomic ReplaceFile. Progress dialog with cancel (cancel = original
  untouched). Requires transient free disk ~ file size; checked upfront with
  a clear error. Saving a 100 GB file takes minutes - that is physics, shown
  honestly. "Save As" identical. No partial in-place writes in v1.

### Encodings
- Detection order: BOM (UTF-8/16LE/16BE/32) -> strict UTF-8 validation of a
  leading sample (e.g. 4 MB) -> UTF-16 heuristic (NUL distribution) -> system
  ANSI codepage fallback.
- Encoding menu: detected encoding shown; full Windows codepage list
  (EnumSystemCodePages) + UTF-8/UTF-16 variants; "Reopen with encoding";
  "Save with encoding" (transcodes during the save stream).
- Decode-on-view: bytes stay in the file's encoding everywhere; only the
  visible window is decoded for rendering. Malformed bytes render as
  replacement glyphs, never block opening. Typed/pasted text is encoded to the
  file encoding at insert time (unencodable chars -> warning, suggest UTF-8
  save).
- EOL: CRLF/LF/CR (and mixed) detected; preserved on edit; status bar shows;
  conversion command CRLF<->LF.

## UI

- Pure Win32 + Direct2D/DirectWrite. Single static-CRT x64 exe (~300 KB), no
  installer required to run, no telemetry, no online anything.
- Chrome: menu bar (File, Edit, View, Encoding, Help), client text area,
  status bar (Ln/Col, byte offset, encoding, EOL, file size, indexing %).
- Editing UX: caret, mouse + keyboard selection, clipboard, IME via TSF,
  standard accelerators (Ctrl+S/O/N/F/G/Z/Y/A/C/X/V, Ctrl+wheel zoom).
- View: optional word wrap (off by default for giants - wrap requires local
  re-layout only), font face/size picker (DirectWrite), light/dark follows
  system. No syntax highlighting, no tabs, no plugins - one window per file.
- Find (Ctrl+F): literal text, case toggle, direction; runs over the piece
  sequence in the file's encoding; on giants searches in background with
  progress + cancel. Ctrl+G: goto line OR byte offset.
- Clipboard copy capped (warning) at 1 GB.
- Rendering virtualized: only visible lines are decoded + laid out.

## Shell integration

- Per-user, no admin: HKCU\Software\Classes\Applications\FastPad.exe
  registration + Default Programs capabilities, so FastPad shows up in Open
  With suggestions for .txt/.log/.csv/.md/etc and as a default-app candidate.
  In-app "Register" / "Unregister" menu items. (Windows 10+ requires the user
  to click the final "set default" - cannot be forced programmatically.)

## Delivery phases (one architecture, shippable steps)

- **M1 Viewer-complete:** mmap window manager, open-any-size, virtualized
  rendering, scroll/goto, encoding detection + reopen-with, status bar,
  background line indexing. (Already a daily-usable klogg/notepad replacement
  for reading.)
- **M2 Editor:** piece tree editing (type/delete/paste), selection, clipboard,
  undo/redo, streaming save / save-as (+ encoding transcode).
- **M3 Polish:** find, goto dialog, word wrap, font picker, EOL conversion,
  shell registration, icon, Ctrl+wheel zoom.

## Engineering

- Toolchain: VS 2022 MSVC, C++20, x64 only, /O2 + /GL release, static CRT.
  Plain .sln + .vcxproj (no CMake), matching the user's daily toolchain.
- Layout:
  - `core/` - static lib, NO windows.h in public headers where avoidable:
    piece tree, mmap window manager, line index, encoding detect/convert
    (Win32 MultiByteToWideChar based), search, document facade.
  - `app/` - Win32/D2D shell: window, renderer, input, dialogs, registration.
  - `tests/` - vendored doctest; unit tests for every core component;
    integration tests over synthetic sparse files (NTFS sparse: create 10 GB
    test files instantly without disk cost) so huge-file paths are testable.
  - `docs/superpowers/` - this spec + plan.
- Performance budgets (asserted in tests where feasible): open < 50 ms for any
  size; scroll frame < 8 ms; typing latency < 10 ms at any file size; search
  >= 1 GB/s on cached regions.

## Non-goals (YAGNI)

- No tabs, plugins, syntax highlighting, autosave, cloud, settings sync,
  printing (v1), regex find (v2 maybe), macros, shell context-menu "Edit with"
  (registration covers Open With), x86/ARM builds (x64 only).

## Risks / honest costs

- Save of giant files takes minutes and transient 2x disk - by design,
  surfaced in UI.
- Line numbers near the end of a fresh giant file appear after background
  indexing completes (seconds to ~a minute); navigation works meanwhile.
- IME/TSF correctness is the fiddliest UI part; M2 risk item, mitigated by
  starting from WM_IME messages (good enough for v1) before full TSF.
- Mixed/exotic encodings (stateful ISO-2022, EBCDIC) render via codepage
  tables only as well as Windows converts them; that matches the requirement
  ("read any encoding" via codepage override).
