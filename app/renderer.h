#pragma once
#include "core/document.h"
#include <d2d1.h>
#include <dwrite_3.h>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

namespace fastpad {

// Virtualized text view over the live Document. Owns the byte-anchored scroll
// position (topOffset_ always sits at a line start or the BOM end) and, since
// M2, the caret/selection state. Caret and selection anchor are byte offsets
// into the live document. Input wiring arrives in the next task; this class
// only renders and answers hit tests.
class Renderer {
public:
    explicit Renderer(HWND hwnd);
    ~Renderer();
    void setDocument(Document* doc);                // not owned
    void onPaint();
    void onResize();
    void scrollLines(int delta);                    // +down, -up
    void scrollPages(int pages);
    void goToOffset(uint64_t offset);               // snaps to a line start
    void goToLine(uint64_t lineNo);
    void goHome() { goToOffset(0); }
    void goEnd();
    void setFontSize(float pt);
    float fontSize() const { return fontSize_; }
    void setWordWrap(bool on);                      // re-layouts + repaints
    bool wordWrap() const { return wordWrap_; }
    uint64_t topOffset() const { return topOffset_; }
    void updateScrollBar();
    void onVScroll(WPARAM wp);
    void onHScroll(WPARAM wp);
    void hScrollBy(float dips);                     // +right, -left; clamped; no-wrap only

    // caret/selection (byte offsets into the live document)
    uint64_t caret() const { return caret_; }
    void setCaret(uint64_t offset, bool extendSelection);   // clamps to [bom, size]
    bool hasSelection() const { return selAnchor_ != caret_; }
    uint64_t selBegin() const { return selAnchor_ < caret_ ? selAnchor_ : caret_; }
    uint64_t selEnd() const { return selAnchor_ < caret_ ? caret_ : selAnchor_; }
    void clearSelection() { selAnchor_ = caret_; }
    std::pair<bool, uint64_t> hitTest(int px, int py);      // client px -> byte offset (false if no doc)
    void ensureCaretVisible();                              // scrolls topOffset_ so the caret line shows
    void invalidate() { InvalidateRect(hwnd_, nullptr, FALSE); }
    // Document content changed (edit/undo/redo/paste/encoding): cached line
    // layouts are stale - rebuild on the next paint.
    void noteDocumentChanged() { linesDirty_ = true; invalidate(); }
    void onCaretTimer();                                    // WM_TIMER id 2 (blink)

private:
    // One on-screen line. byteEnd points at the start of the NEXT line, i.e.
    // it INCLUDES the line's break bytes; text/charByte cover only the
    // rendered chars (break chars stripped).
    struct VisibleLine {
        uint64_t byteStart = 0;
        uint64_t byteEnd = 0;
        std::wstring text;
        std::vector<uint64_t> charByte;  // [i] = byte offset of text[i]; text.size()+1 entries
        IDWriteTextLayout* layout = nullptr;
        float y = 0.0f;
        float height = 0.0f;             // slot height: lineHeight_, or the
                                         // wrapped layout's measured height
    };

    void ensureTarget();
    void discardTarget();
    void loadEmbeddedFont();
    void createTextFormat();
    void buildVisibleLines();
    void clearLines();
    void drawSelection();
    void drawCaret();
    uint64_t prevLineStart(uint64_t offset);
    uint64_t nextLineStart(uint64_t offset);
    int linesPerPage() const;
    void updateHScrollBar();                 // SB_HORZ; hidden unless content overflows
    void showHBar(bool show);                // ShowScrollBar only on actual change
    void setScrollX(float x);                // clamps, updates SB_HORZ, repaints
    void ensureCaretVisibleVert();           // the M2 vertical line walk
    void ensureCaretVisibleX();              // no-wrap horizontal caret reveal

    HWND hwnd_;
    Document* doc_ = nullptr;
    ID2D1Factory* d2dFactory_ = nullptr;
    IDWriteFactory* dwFactory_ = nullptr;
    // Embedded-font plumbing (Windows 10+ in-memory loader). All stay null
    // when IDWriteFactory5 is unavailable or the resource fails to load; the
    // renderer then falls back to system Consolas silently.
    IDWriteFactory5* dwFactory5_ = nullptr;
    IDWriteInMemoryFontFileLoader* memLoader_ = nullptr;
    IDWriteFontCollection1* fontCollection_ = nullptr;
    std::wstring fontFamily_;                       // real family name from the collection
    ID2D1HwndRenderTarget* target_ = nullptr;
    ID2D1SolidColorBrush* textBrush_ = nullptr;
    ID2D1SolidColorBrush* selBrush_ = nullptr;
    IDWriteTextFormat* format_ = nullptr;
    float fontSize_ = 14.0f;
    float lineHeight_ = 18.0f;
    bool wordWrap_ = false;              // per-layout wrap; format_ stays NO_WRAP
    uint64_t topOffset_ = 0;
    float scrollX_ = 0.0f;               // horizontal scroll in DIPs; always 0 in wrap mode
    float contentWidth_ = 0.0f;          // widest visible line (DIPs); 0 in wrap mode
    bool hBarVisible_ = false;           // tracked so ShowScrollBar runs only on change
                                         // (each call resizes the client -> WM_SIZE)

    std::vector<VisibleLine> lines_;     // last paint's lines; reused by hitTest
    uint64_t lastVisibleEnd_ = 0;        // byteEnd of the last painted line
    uint64_t caret_ = 0;
    uint64_t selAnchor_ = 0;
    bool caretOn_ = true;

    // Layout cache: rebuild lines_ only when scroll/content/font/size changed,
    // not on every paint (caret blinks reuse the cached IDWriteTextLayouts).
    bool linesDirty_ = true;
    LONG lastBuildWidth_ = -1;           // client width lines_ was built for
    LONG lastBuildHeight_ = -1;          // client height lines_ was built for

    // Caret's last computed position in client pixels: the blink timer
    // invalidates only this rect instead of the whole window.
    RECT caretRect_{};
    bool caretRectValid_ = false;
};

} // namespace fastpad
