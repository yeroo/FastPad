# Backlog (post-M3, from M3 final review, 2026-06-12)

Open items only. Everything M3 delivered (find, word wrap, horizontal
scrolling, EOL conversion, proportional scrollbar / thumb clamp, bounded
indexer footprint, Open-With registration) has been dropped from this list.

## New in M3 review

1. **Exe name vs registration key**: the output binary is `app.exe` but the
   Open-With registration writes `Software\Classes\Applications\FastPad.exe`.
   The Applications key name never has to match the actual exe (the command
   value carries the real path), so the mismatch is inert - but it is
   confusing. Consider renaming the output exe to `FastPad.exe` in M4.
2. **Command-ID packing**: `IDM_VIEW_FONTSIZE_BASE` occupies 500-509 and
   `IDM_VIEW_WRAP = 510` sits immediately after; adding an 11th font size
   requires moving WRAP first (noted in `app/commands.h`).

## Carried from M2 era (verified still open against m3-polish)

3. **Undo history cleared by save**: after a successful save the document is
   rebound as the new pristine original; the undo journal does not survive.
4. **Caret resets on save**: the post-save rebind puts the caret back at the
   start of the document.
5. **256 MB copy cap**: clipboard copy is refused above `kMaxClipboardBytes`
   (256 MB); Save As is the export path for larger selections.
6. **Paste is RAM-only**: pasted data lives in the piece-table ADD buffer in
   RAM; paste size is bounded by available memory.
7. **WM_CHAR input**: no TSF/IME inline composition; dead keys and composed
   characters work, full IME does not.
8. **Up/Down byte-column approximation**: vertical caret movement targets the
   same BYTE delta from line start, snapped to a character boundary -
   multi-byte characters cause visual column drift.
9. **AppWindow teardown singleton**: `AppWindow` is a leak-by-design
   process-lifetime object (`new`, never deleted) - fine until orderly
   teardown (e.g. save-on-exit) is needed.
10. **Surrogate-pair caret drift**: a surrogate pair is one document char but
    two wchars in `VisibleLine::charByte`, so caret/selection offsets drift by
    one char step after each pair (renderer.cpp).
11. **Double-click word-select**: not implemented (no WM_LBUTTONDBLCLK
    handling).
12. **ADD-buffer spill to disk**: the piece-table ADD buffer is append-only
    RAM; gigantic edit sessions should spill it to a temp file.
13. **Piece vector -> balanced tree**: the piece table is a flat
    `std::vector<Piece>` by design; pathological edit counts (millions of
    scattered edits) degrade - revisit as a balanced tree if that ever
    matters.
