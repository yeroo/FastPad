# FastPad

A minimal, blazing-fast text editor that opens files of **any size** instantly.
M1 viewer, M2 editor, and M3 polish are complete (see `docs/superpowers/specs`).

Design goals: classic-notepad simplicity, zero bloat, any encoding.

Current version: **0.3.1.0** (versioned exe with app icon).

---

## Build

Prerequisites: Visual Studio 2022 (any edition — Community, Professional, or
Enterprise) with the **Desktop development with C++** workload, which provides
the v143 toolset and the Windows SDK.

Locate MSBuild for whichever edition is installed via `vswhere`, then build —
this works regardless of edition or install path:

```powershell
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild `
    -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1

$env:MSBUILDDISABLENODEREUSE="1"
& $msbuild FastPad.sln /p:Configuration=Release /p:Platform=x64 /m
```

Run tests (59 cases):

```powershell
.\x64\Release\tests.exe
```

Output binary: `x64\Release\app.exe`

---

## Usage

```
app.exe <file>
```

Renders with embedded MesloLGS DZ Nerd Font (falls back to Consolas); View > Font size (8-36, persisted) or Ctrl+wheel.

### Keyboard shortcuts

| Key | Action |
|-----|--------|
| Ctrl+O | Open file dialog |
| Ctrl+S / Ctrl+Shift+S | Save / Save As |
| Ctrl+F | Find dialog |
| F3 / Shift+F3 | Find next / previous |
| Ctrl+G | Go to line or offset (`:offset`, `:0x...` hex) |
| PgUp / PgDn | Scroll one screen |
| Ctrl+Home | Jump to start of file |
| Ctrl+End | Jump to end of file |
| Mouse wheel | Scroll lines |
| Shift+Wheel / tilt wheel | Scroll horizontally |
| Ctrl+Wheel | Zoom font size |

### Encoding menu

**Encoding** menu lets you reopen the current file as UTF-8, UTF-16 LE/BE, or any installed Windows codepage.

### Settings

Font size and word wrap are persisted across restarts in `%APPDATA%\FastPad\settings.txt` (plain `key=value`: `fontSize`, `wordWrap`).

---

## Editing (M2)

### Editing keys

Type any printable character to insert at the caret; Backspace/Delete erase one character; Enter inserts the document's own line ending (auto-detected on open). Arrow keys move the caret: Left/Right by one character boundary; Up/Down move to the same byte-column on the adjacent line (multi-byte characters cause minor visual column drift). Shift held with any arrow extends the selection. Click or click-drag to set or extend the selection with the mouse. PgUp/PgDn and the mouse wheel scroll the view without moving the caret.

### Clipboard

Ctrl+X cuts the selection, Ctrl+C copies, Ctrl+V pastes. Copy is refused for selections larger than 256 MB - use Save As to export data that size. On paste, line endings in the clipboard text are normalized to the document's own EOL sequence.

### Undo / Redo

Ctrl+Z undoes the last edit; Ctrl+Y redoes. Sequential characters typed at the end of the document are coalesced and undone as one unit. **Undo history is cleared by saving** - after a successful save the document is pristine and there is nothing to undo.

### Save / Save As

Ctrl+S saves in place; Ctrl+Shift+S opens Save As. Saving is streaming and atomic: the engine writes to a temporary file, then replaces the original on success, so a power failure during save does not corrupt the existing file. The operation needs transient free disk space of approximately the file size plus 64 MB. For files larger than 64 MB a progress dialog is shown with a Cancel button; pressing Cancel or Esc aborts the save (the original file is left untouched). After a successful save the caret returns to the start of the document (known limitation).

### Encoding conversions

The Encoding menu contains "Save As UTF-8..." and "Save As UTF-16 LE..." entries that transcode while saving. Switching the interpretation of the current file (Reopen as ...) requires an unedited document; you will be prompted to save or undo changes first.

### Memory and IME

Pasted data is held in RAM; paste size is bounded by available memory. Input is processed at the WM_CHAR level, which gives correct behavior for dead keys and composed characters; full IME support (inline composition) is not implemented yet.

---

## Polish (M3)

### Find

Ctrl+F opens the Find dialog (Edit > Find...); F3 repeats forward, Shift+F3 backward. A "Match case" option controls case sensitivity; the search wraps around the ends of the document. Searching runs in the file's own encoding; case folding is ASCII-only, so case-insensitive matching of non-ASCII characters is exact-only. Files larger than 64 MB are searched on a background thread with a progress popup; Esc cancels.

### Word wrap

View > Word wrap toggles soft wrapping at the window edge; the setting is persisted across restarts. Under wrap, PgUp/PgDn move by logical lines, so a screenful of heavily wrapped text may page through fewer document lines than expected.

### Horizontal scrolling

Long lines get an auto-hiding horizontal scrollbar (hidden when wrap is on or every visible line fits). The viewport follows the caret and selection horizontally; Shift+wheel and tilt-wheel scroll sideways.

### Line endings

The Encoding menu has "Save with LF endings..." and "Save with CRLF endings..." Save-As variants that convert every line ending while writing; BOM and encoding are preserved.

### Open With registration

Help > "Register in Open With" adds FastPad to the Windows Open With list for `.txt .log .csv .md .json .xml .ini .yaml .yml` - per-user only (HKCU), no admin rights needed. After registering, pick FastPad via right-click > Open With or Settings > Default apps. Help > "Unregister from Open With" removes everything it wrote (your settings in `%APPDATA%\FastPad` survive).

---

## Honest notes

- **Line numbers and total-line count** appear in the status bar as background indexing completes; the status bar shows `indexing N%` until done.
- **BOM bytes** are included in position math (offset counts from byte 0).
- Remaining known limits are tracked in `docs/superpowers/BACKLOG.md`.

---

## Performance

Measured on a 10 GB sparse file (Release x64):

> **opened in 8.1 ms**

The open time is shown in the window title after every file load (e.g. `file.txt - FastPad (opened in 8.1 ms)`), and emitted to the debug output channel via `OutputDebugStringW` for profiling.
