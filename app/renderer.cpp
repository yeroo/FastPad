#include "app/renderer.h"
#include "app/resource.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace fastpad {

static const wchar_t* kFont = L"Consolas";       // fallback when no embedded font
static const UINT_PTR kCaretTimerId = 2;
static const UINT kCaretBlinkMs = 530;
static const size_t kMaxLineChars = 4096;            // rendered chars per line cap (as in M1)
static const uint64_t kMaxLineBytes = 16 * 1024;     // decode cap: 4096 chars x 4 bytes worst case

Renderer::Renderer(HWND hwnd) : hwnd_(hwnd) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dwFactory_);
    loadEmbeddedFont();
    createTextFormat();
    // The view window is created with WS_HSCROLL; the bar is hidden by
    // default and only appears once visible content overflows the client.
    ShowScrollBar(hwnd_, SB_HORZ, FALSE);
    SetTimer(hwnd_, kCaretTimerId, kCaretBlinkMs, nullptr);
}

Renderer::~Renderer() {
    KillTimer(hwnd_, kCaretTimerId);
    clearLines();
    discardTarget();
    if (format_) format_->Release();
    if (fontCollection_) fontCollection_->Release();
    // Unregister BEFORE releasing the loader (DirectWrite holds no ref of its
    // own); only then drop the factory interfaces.
    if (memLoader_) {
        if (dwFactory5_) dwFactory5_->UnregisterFontFileLoader(memLoader_);
        memLoader_->Release();
    }
    if (dwFactory5_) dwFactory5_->Release();
    if (dwFactory_) dwFactory_->Release();
    if (d2dFactory_) d2dFactory_->Release();
}

// Load the MesloLGSDZ Nerd Font embedded as an RCDATA resource into a private
// DirectWrite collection (in-memory loader, IDWriteFactory5 / Windows 10+).
// Every failure path leaves fontCollection_/fontFamily_ empty, which makes
// createTextFormat fall back to the system Consolas - silent by design.
void Renderer::loadEmbeddedFont() {
    if (!dwFactory_) return;
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_FONT_MAIN), RT_RCDATA);
    if (!res) return;
    HGLOBAL hres = LoadResource(nullptr, res);
    if (!hres) return;
    const void* data = LockResource(hres);
    const DWORD size = SizeofResource(nullptr, res);
    if (!data || !size) return;

    if (FAILED(dwFactory_->QueryInterface(__uuidof(IDWriteFactory5), (void**)&dwFactory5_))
        || !dwFactory5_)
        return;

    IDWriteFontFile* fontFile = nullptr;
    IDWriteFontSetBuilder1* builder = nullptr;
    IDWriteFontSet* fontSet = nullptr;
    IDWriteFontFamily* family = nullptr;
    IDWriteLocalizedStrings* names = nullptr;
    bool registered = false;
    bool ok = false;

    do {
        if (FAILED(dwFactory5_->CreateInMemoryFontFileLoader(&memLoader_)) || !memLoader_) break;
        if (FAILED(dwFactory5_->RegisterFontFileLoader(memLoader_))) break;
        registered = true;
        if (FAILED(memLoader_->CreateInMemoryFontFileReference(dwFactory5_, data, (UINT32)size,
                                                               nullptr, &fontFile)) || !fontFile)
            break;
        if (FAILED(dwFactory5_->CreateFontSetBuilder(&builder)) || !builder) break;
        if (FAILED(builder->AddFontFile(fontFile))) break;
        if (FAILED(builder->CreateFontSet(&fontSet)) || !fontSet) break;
        if (FAILED(dwFactory5_->CreateFontCollectionFromFontSet(fontSet, &fontCollection_))
            || !fontCollection_)
            break;
        if (fontCollection_->GetFontFamilyCount() == 0) break;

        // Extract the REAL family name - never hardcode it.
        if (FAILED(fontCollection_->GetFontFamily(0, &family)) || !family) break;
        if (FAILED(family->GetFamilyNames(&names)) || !names) break;
        UINT32 len = 0;
        if (FAILED(names->GetStringLength(0, &len))) break;
        std::vector<wchar_t> buf((size_t)len + 1, L'\0');
        if (FAILED(names->GetString(0, buf.data(), len + 1))) break;
        fontFamily_.assign(buf.data());
        ok = !fontFamily_.empty();
    } while (false);

    if (names) names->Release();
    if (family) family->Release();
    if (fontSet) fontSet->Release();
    if (builder) builder->Release();
    if (fontFile) fontFile->Release();

    if (ok) {
        OutputDebugStringW((L"FastPad: embedded font family '" + fontFamily_ + L"'\n").c_str());
        return;
    }
    // Cleanup on failure so createTextFormat sees the fallback state.
    fontFamily_.clear();
    if (fontCollection_) { fontCollection_->Release(); fontCollection_ = nullptr; }
    if (memLoader_) {
        if (registered) dwFactory5_->UnregisterFontFileLoader(memLoader_);
        memLoader_->Release(); memLoader_ = nullptr;
    }
    dwFactory5_->Release(); dwFactory5_ = nullptr;
}

void Renderer::createTextFormat() {
    if (format_) { format_->Release(); format_ = nullptr; }
    if (!dwFactory_) return;
    const bool embedded = fontCollection_ && !fontFamily_.empty();
    dwFactory_->CreateTextFormat(embedded ? fontFamily_.c_str() : kFont,
        embedded ? static_cast<IDWriteFontCollection*>(fontCollection_) : nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        fontSize_ * 96.0f / 72.0f, L"", &format_);
    // One VisibleLine per layout: never wrap inside a layout (long lines clip
    // at the right edge, matching the M1 DrawTextW+CLIP visuals).
    if (format_) format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    lineHeight_ = fontSize_ * 96.0f / 72.0f * 1.35f;
}

void Renderer::ensureTarget() {
    if (target_ || !d2dFactory_) return;
    RECT rc; GetClientRect(hwnd_, &rc);
    d2dFactory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU((UINT32)rc.right, (UINT32)rc.bottom)),
        &target_);
    if (target_) {
        target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &textBrush_);
        target_->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.5f, 1.0f, 0.35f), &selBrush_);
    }
}

void Renderer::discardTarget() {
    if (selBrush_) { selBrush_->Release(); selBrush_ = nullptr; }
    if (textBrush_) { textBrush_->Release(); textBrush_ = nullptr; }
    if (target_) { target_->Release(); target_ = nullptr; }
}

void Renderer::onResize() {
    if (target_) {
        RECT rc; GetClientRect(hwnd_, &rc);
        target_->Resize(D2D1::SizeU((UINT32)rc.right, (UINT32)rc.bottom));
    }
    updateScrollBar();
    // Resize does not preserve content; rebuild layouts for the new client
    // size and repaint immediately (no erase - D2D covers the full target).
    linesDirty_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Renderer::setDocument(Document* doc) {
    doc_ = doc;
    topOffset_ = doc_ ? (uint64_t)doc_->encoding().bomBytes : 0;
    caret_ = selAnchor_ = topOffset_;
    lastVisibleEnd_ = topOffset_;
    scrollX_ = 0.0f;
    clearLines();
    linesDirty_ = true;
    caretOn_ = true;
    SetTimer(hwnd_, kCaretTimerId, kCaretBlinkMs, nullptr);
    updateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// Deliberately LOGICAL-line semantics, also under word wrap: a page then
// scrolls fewer visual rows when lines wrap (each wrapped line counts once).
// ensureCaretVisible shares the same logical-line walk - accepted M3 limit.
int Renderer::linesPerPage() const {
    RECT rc; GetClientRect(hwnd_, &rc);
    int n = (int)((float)(rc.bottom - 4) / lineHeight_);
    return n > 0 ? n : 1;
}

void Renderer::clearLines() {
    for (auto& vl : lines_)
        if (vl.layout) vl.layout->Release();
    lines_.clear();
}

void Renderer::buildVisibleLines() {
    clearLines();
    contentWidth_ = 0.0f;
    if (!doc_ || !dwFactory_ || !format_) { lastVisibleEnd_ = topOffset_; updateHScrollBar(); return; }

    RECT rc; GetClientRect(hwnd_, &rc);
    // Wrap mode keeps the text inside both 4 px margins; no-wrap layouts only
    // need the left margin (they clip at the right edge as in M1).
    float width = (float)rc.right - (wordWrap_ ? 8.0f : 4.0f);
    if (width < 1.0f) width = 1.0f;
    float y = 2.0f;
    uint64_t pos = topOffset_;
    const uint64_t docSize = doc_->size();
    bool lastEndedWithBreak = false;

    while (y < (float)rc.bottom && pos < docSize) {
        VisibleLine vl;
        vl.byteStart = pos;
        uint64_t lineEnd = doc_->findNextBreak(pos);   // offset AFTER the break; size() if none
        if (lineEnd <= pos) break;                     // defensive: never loop in place
        vl.byteEnd = lineEnd;

        uint64_t lineBytes = lineEnd - pos;
        size_t decodeBytes = (size_t)(lineBytes < kMaxLineBytes ? lineBytes : kMaxLineBytes);
        if (decodeBytes) doc_->decodeAt(pos, decodeBytes, vl.text);

        // The decoded range includes the break chars; strip them from the
        // rendered text (only possible when the full line fit the cap).
        lastEndedWithBreak = false;
        if ((uint64_t)decodeBytes == lineBytes) {
            if (vl.text.size() >= 2 && vl.text[vl.text.size() - 2] == L'\r' && vl.text.back() == L'\n') {
                vl.text.resize(vl.text.size() - 2); lastEndedWithBreak = true;
            } else if (!vl.text.empty() && (vl.text.back() == L'\n' || vl.text.back() == L'\r')) {
                vl.text.pop_back(); lastEndedWithBreak = true;
            }
        }
        if (vl.text.size() > kMaxLineChars) {
            vl.text.resize(kMaxLineChars);
            // never end on a dangling high surrogate
            if (vl.text.back() >= 0xD800 && vl.text.back() <= 0xDBFF) vl.text.pop_back();
        }

        // charByte[i] = byte offset of text[i], built with one charStepForward
        // per wchar. M2 approximation: a surrogate pair is ONE document char
        // but TWO wchars, so offsets drift by one char step after each pair
        // (caret/selection land on char boundaries, just not the exact one).
        vl.charByte.reserve(vl.text.size() + 1);
        uint64_t b = vl.byteStart;
        for (size_t i = 0; i < vl.text.size(); ++i) {
            vl.charByte.push_back(b);
            b = doc_->charStepForward(b);
        }
        vl.charByte.push_back(b);   // just past the last rendered char (end-of-line caret/hit)

        // Word wrap is enabled per-layout (the shared format_ stays NO_WRAP);
        // the slot height becomes the layout's measured height, so a wrapped
        // line occupies as many visual rows as DirectWrite produced. No-wrap
        // layouts get an effectively unbounded max width: the M3 horizontal
        // scroll draws them at x = 4 - scrollX_, so OPTIONS_CLIP must not cut
        // the line at the old client-width box (the target clips the window
        // edge anyway). Lines are char-capped, so the layouts stay bounded.
        dwFactory_->CreateTextLayout(vl.text.c_str(), (UINT32)vl.text.size(),
            format_, wordWrap_ ? width : 1.0e6f, wordWrap_ ? 1.0e6f : lineHeight_, &vl.layout);
        vl.height = lineHeight_;
        if (wordWrap_ && vl.layout) {
            vl.layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            DWRITE_TEXT_METRICS tm{};
            if (SUCCEEDED(vl.layout->GetMetrics(&tm)) && tm.height > lineHeight_)
                vl.height = tm.height;
        } else if (vl.layout) {
            DWRITE_TEXT_METRICS tm{};
            if (SUCCEEDED(vl.layout->GetMetrics(&tm))
                && tm.widthIncludingTrailingWhitespace > contentWidth_)
                contentWidth_ = tm.widthIncludingTrailingWhitespace;
        }
        vl.y = y;
        y += vl.height;
        lines_.push_back(std::move(vl));
        pos = lineEnd;
    }

    // Synthesize the empty last line when the document ends with a break (or
    // is empty): the caret can sit there at offset == size().
    if (y < (float)rc.bottom && pos >= docSize && (lines_.empty() || lastEndedWithBreak)) {
        VisibleLine vl;
        vl.byteStart = vl.byteEnd = docSize;
        vl.charByte.push_back(docSize);
        dwFactory_->CreateTextLayout(L"", 0, format_, width, lineHeight_, &vl.layout);
        vl.y = y;
        vl.height = lineHeight_;
        lines_.push_back(std::move(vl));
    }

    lastVisibleEnd_ = lines_.empty() ? topOffset_ : lines_.back().byteEnd;
    updateHScrollBar();
}

void Renderer::drawSelection() {
    if (!doc_ || !selBrush_) return;
    const uint64_t sb = selBegin(), se = selEnd();
    if (sb >= se) return;

    for (const auto& vl : lines_) {
        if (!vl.layout) continue;
        uint64_t lo = sb > vl.byteStart ? sb : vl.byteStart;
        uint64_t hi = se < vl.byteEnd ? se : vl.byteEnd;
        if (lo >= hi) continue;

        auto idxOf = [&](uint64_t off) {
            size_t i = (size_t)(std::lower_bound(vl.charByte.begin(), vl.charByte.end(), off) - vl.charByte.begin());
            return i < vl.text.size() ? i : vl.text.size();
        };
        size_t startIdx = idxOf(lo);
        size_t endIdx = idxOf(hi);
        UINT32 len = (UINT32)(endIdx > startIdx ? endIdx - startIdx : 0);

        if (len) {
            DWRITE_HIT_TEST_METRICS stackArr[8];
            UINT32 count = 0;
            HRESULT hr = vl.layout->HitTestTextRange((UINT32)startIdx, len, 0.0f, 0.0f, stackArr, 8, &count);
            std::vector<DWRITE_HIT_TEST_METRICS> heap;
            const DWRITE_HIT_TEST_METRICS* arr = stackArr;
            if (hr == E_NOT_SUFFICIENT_BUFFER && count > 0) {
                heap.resize(count);
                hr = vl.layout->HitTestTextRange((UINT32)startIdx, len, 0.0f, 0.0f, heap.data(), count, &count);
                arr = heap.data();
            }
            if (SUCCEEDED(hr)) {
                for (UINT32 i = 0; i < count; ++i) {
                    // HitTestTextRange metrics are layout-relative: add the
                    // LINE's y origin. Wrapped lines get one metric per visual
                    // row at its own top/height; unwrapped lines keep the full
                    // fixed slot (metric height is the shorter natural height).
                    const float top = wordWrap_ ? vl.y + arr[i].top : vl.y;
                    const float rh = (wordWrap_ && arr[i].height > 0.0f) ? arr[i].height : lineHeight_;
                    target_->FillRectangle(D2D1::RectF(
                        4.0f - scrollX_ + arr[i].left, top,
                        4.0f - scrollX_ + arr[i].left + arr[i].width, top + rh), selBrush_);
                }
            }
        }
        // Selected line break (incl. empty lines): a half-char stub past the text.
        if (hi == vl.byteEnd && vl.byteEnd > vl.charByte.back()) {
            FLOAT cx = 0, cy = 0;
            DWRITE_HIT_TEST_METRICS hm{};
            vl.layout->HitTestTextPosition((UINT32)endIdx, FALSE, &cx, &cy, &hm);
            float x0 = 4.0f - scrollX_ + cx;
            // The break stub belongs to the line's LAST visual row when wrapping.
            const float top = wordWrap_ ? vl.y + cy : vl.y;
            const float rh = (wordWrap_ && hm.height > 0.0f) ? hm.height : lineHeight_;
            target_->FillRectangle(D2D1::RectF(x0, top, x0 + lineHeight_ * 0.4f, top + rh), selBrush_);
        }
    }
}

void Renderer::drawCaret() {
    // Recompute the caret rect on EVERY paint (even blink-off) so the blink
    // timer always has a fresh rect to invalidate; draw only when visible.
    caretRectValid_ = false;
    if (!doc_ || !textBrush_ || lines_.empty()) return;
    const VisibleLine* hit = nullptr;
    for (const auto& vl : lines_)
        if (caret_ >= vl.byteStart && caret_ < vl.byteEnd) { hit = &vl; break; }
    // End of document: no line satisfies caret < byteEnd; the caret belongs to
    // the last visible line (real or the synthesized empty one).
    if (!hit && caret_ == doc_->size() && lines_.back().byteEnd == doc_->size())
        hit = &lines_.back();
    if (!hit || !hit->layout) return;

    size_t idx = (size_t)(std::lower_bound(hit->charByte.begin(), hit->charByte.end(), caret_) - hit->charByte.begin());
    if (idx > hit->text.size()) idx = hit->text.size();
    FLOAT cx = 0, cy = 0;
    DWRITE_HIT_TEST_METRICS m{};
    hit->layout->HitTestTextPosition((UINT32)idx, FALSE, &cx, &cy, &m);
    float x = 4.0f - scrollX_ + cx;
    // Wrapped lines: the caret sits on its actual visual row - cy is the
    // row's top within the layout, m.height its height. Unwrapped lines keep
    // the fixed slot rect (cy is 0 there but m.height is the natural font
    // height, shorter than the 1.35x slot, so keep the M1/M2 visuals).
    const float top = wordWrap_ ? hit->y + cy : hit->y;
    const float ch = (wordWrap_ && m.height > 0.0f) ? m.height : lineHeight_;
    const D2D1_RECT_F r = D2D1::RectF(x, top, x + 1.5f, top + ch);

    // Remember the caret in client PIXELS (D2D coords are DIPs) with a 1px
    // margin so onCaretTimer can invalidate just this sliver of the window.
    FLOAT dpiX = 96.0f, dpiY = 96.0f;
    target_->GetDpi(&dpiX, &dpiY);
    caretRect_.left   = (LONG)(r.left   * dpiX / 96.0f) - 1;
    caretRect_.top    = (LONG)(r.top    * dpiY / 96.0f) - 1;
    caretRect_.right  = (LONG)(r.right  * dpiX / 96.0f) + 2;
    caretRect_.bottom = (LONG)(r.bottom * dpiY / 96.0f) + 2;
    caretRectValid_ = true;

    if (caretOn_) target_->FillRectangle(r, textBrush_);
}

void Renderer::onPaint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    ensureTarget();
    if (!target_ || !format_ || !textBrush_) { EndPaint(hwnd_, &ps); return; }

    // Reuse the cached IDWriteTextLayouts when nothing that affects them
    // changed (pure caret-blink paints): rebuilding every layout per paint is
    // both CPU churn and a flicker source on slow frames.
    RECT crc; GetClientRect(hwnd_, &crc);
    if (linesDirty_ || crc.right != lastBuildWidth_ || crc.bottom != lastBuildHeight_) {
        buildVisibleLines();
        linesDirty_ = false;
        lastBuildWidth_ = crc.right;
        lastBuildHeight_ = crc.bottom;
    }

    target_->BeginDraw();
    target_->Clear(D2D1::ColorF(D2D1::ColorF::White));
    drawSelection();                                   // under the text
    for (const auto& vl : lines_)
        if (vl.layout)
            target_->DrawTextLayout(D2D1::Point2F(4.0f - scrollX_, vl.y), vl.layout, textBrush_,
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
    drawCaret();

    HRESULT hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) discardTarget();
    EndPaint(hwnd_, &ps);
}

std::pair<bool, uint64_t> Renderer::hitTest(int px, int py) {
    if (!doc_) return { false, 0 };
    if (lines_.empty()) return { true, topOffset_ };   // no paint yet / nothing visible

    // Word wrap makes slots variable-height (a wrapped line spans several
    // visual rows), so dividing y by a fixed lineHeight_ is wrong: scan the
    // painted lines' y ranges instead. Above the first line clamps onto it.
    const VisibleLine* hitLine = nullptr;
    for (const auto& l : lines_)
        if ((float)py < l.y + l.height) { hitLine = &l; break; }
    if (!hitLine)                                      // below the last line: its end
        return { true, lines_.back().charByte.back() };

    const VisibleLine& vl = *hitLine;
    if (!vl.layout || vl.text.empty()) return { true, vl.byteStart };

    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    vl.layout->HitTestPoint((FLOAT)px - 4.0f + scrollX_, (FLOAT)py - vl.y, &trailing, &inside, &m);
    size_t idx = (size_t)m.textPosition + (trailing ? (size_t)m.length : 0);
    if (idx > vl.text.size()) idx = vl.text.size();
    return { true, vl.charByte[idx] };
}

void Renderer::setCaret(uint64_t offset, bool extendSelection) {
    if (!doc_) return;
    const uint64_t lo = (uint64_t)doc_->encoding().bomBytes;
    if (offset < lo) offset = lo;
    if (offset > doc_->size()) offset = doc_->size();
    if (!extendSelection) selAnchor_ = offset;
    caret_ = offset;
    caretOn_ = true;                                   // restart the blink phase
    SetTimer(hwnd_, kCaretTimerId, kCaretBlinkMs, nullptr);
    ensureCaretVisible();
    invalidate();
}

void Renderer::ensureCaretVisible() {
    // The vertical walk may bail or jump (goToOffset resets scrollX_ to 0);
    // the horizontal reveal runs unconditionally AFTER it so the caret column
    // ends up on screen in every path - typing, Home/End, arrows, find.
    ensureCaretVisibleVert();
    ensureCaretVisibleX();
}

void Renderer::ensureCaretVisibleVert() {
    if (!doc_) return;
    if (caret_ < topOffset_) { goToOffset(caret_); return; }
    // Down direction needs the last paint's visible range; before the first
    // paint (or right after a scroll reset it) there is nothing to compare.
    if (lastVisibleEnd_ <= topOffset_) return;

    uint64_t end = lastVisibleEnd_;
    const uint64_t docSize = doc_->size();
    bool moved = false;
    int cap = linesPerPage();                          // never walk more than a page
    bool capExhausted = false;
    while (cap-- > 0 && (caret_ > end || (caret_ == end && end < docSize))) {
        uint64_t nTop = doc_->findNextBreak(topOffset_);
        if (nTop <= topOffset_ || nTop >= docSize) break;
        topOffset_ = nTop;
        moved = true;
        uint64_t nEnd = doc_->findNextBreak(end);      // window slides one line: end advances too
        if (nEnd <= end) break;
        end = nEnd;
    }
    // If the caret is still outside the visible range after the page walk (e.g.
    // a large jump or a caret far below the screen), fall back to a direct seek.
    if (caret_ > end || (caret_ == end && end < docSize)) {
        capExhausted = true;
    }
    if (capExhausted) {
        goToOffset(caret_);
        return;
    }
    if (moved) {
        lastVisibleEnd_ = end;
        linesDirty_ = true;           // topOffset_ moved: cached layouts are stale
        updateScrollBar();
        invalidate();
    }
}

void Renderer::onCaretTimer() {
    caretOn_ = !caretOn_;
    if (!doc_) return;
    // Blink touches only the caret sliver: never force a full-window repaint
    // pulse every 530 ms. Cached layouts make the resulting paint cheap too.
    if (caretRectValid_) InvalidateRect(hwnd_, &caretRect_, FALSE);
    else InvalidateRect(hwnd_, nullptr, FALSE);
}

// Line scans live in core now (Document::findNextBreak/findPrevBreak operate
// on live content with full-unit UTF-16 comparison). Thin wrappers kept for
// scrollLines/goToOffset call sites.
uint64_t Renderer::nextLineStart(uint64_t offset) {
    return doc_ ? doc_->findNextBreak(offset) : offset;
}

uint64_t Renderer::prevLineStart(uint64_t offset) {
    return doc_ ? doc_->findPrevBreak(offset) : 0;
}

void Renderer::scrollLines(int delta) {
    if (!doc_) return;
    while (delta > 0) {
        uint64_t n = nextLineStart(topOffset_);
        if (n == topOffset_ || n >= doc_->size()) break;
        topOffset_ = n; delta--;
    }
    while (delta < 0 && topOffset_ > 0) { topOffset_ = prevLineStart(topOffset_); delta++; }
    lastVisibleEnd_ = topOffset_;   // unknown until the next paint
    linesDirty_ = true;
    updateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Renderer::scrollPages(int pages) { scrollLines(pages * linesPerPage()); }

void Renderer::goToOffset(uint64_t offset) {
    if (!doc_) return;
    if (offset > doc_->size()) offset = doc_->size();
    // Clamp to size()-1 before prevLineStart(offset+1): passing size()+1 into
    // the index walk skips the last newline and lands past the real last line.
    const uint64_t sz = doc_->size();
    if (sz > 0 && offset >= sz) offset = sz - 1;
    topOffset_ = (offset == 0) ? (uint64_t)doc_->encoding().bomBytes : prevLineStart(offset + 1);
    lastVisibleEnd_ = topOffset_;   // unknown until the next paint
    scrollX_ = 0.0f;                // jump targets start at the line start
    linesDirty_ = true;
    updateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Renderer::goToLine(uint64_t lineNo) {
    if (!doc_) return;
    uint64_t lc = doc_->lineCount();
    if (lineNo >= lc) lineNo = lc ? lc - 1 : 0;
    topOffset_ = doc_->lineStart(lineNo);
    lastVisibleEnd_ = topOffset_;   // unknown until the next paint
    scrollX_ = 0.0f;                // jump targets start at the line start
    linesDirty_ = true;
    updateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Renderer::goEnd() {
    if (!doc_ || doc_->size() == 0) return;
    goToOffset(doc_->size() - 1);
}

void Renderer::setWordWrap(bool on) {
    if (wordWrap_ == on) return;
    wordWrap_ = on;
    scrollX_ = 0.0f;
    // Wrap: hides SB_HORZ immediately. No-wrap: re-arms with the (stale, soon
    // rebuilt) contentWidth_; the post-build updateHScrollBar sets the real range.
    updateHScrollBar();
    linesDirty_ = true;
    invalidate();
}

void Renderer::setFontSize(float pt) {
    fontSize_ = (pt < 6.0f) ? 6.0f : (pt > 72.0f ? 72.0f : pt);
    createTextFormat();
    linesDirty_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Renderer::updateScrollBar() {
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_POS | SIF_PAGE;
    si.nMin = 0; si.nMax = 100000; si.nPage = 1000;   // fallback: index still running
    if (doc_ && doc_->indexComplete()) {
        const uint64_t lc = doc_->lineCount();
        if (lc > 0) {
            // Proportional thumb: page covers (linesPerPage / lineCount) of the range.
            si.nPage = (UINT)std::clamp<uint64_t>(
                (uint64_t)linesPerPage() * 100000u / lc, 1u, 100000u);
        }
    }
    si.nPos = (doc_ && doc_->size())
        ? (int)((double)topOffset_ / (double)doc_->size() * 99000.0) : 0;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void Renderer::onVScroll(WPARAM wp) {
    if (!doc_) return;
    switch (LOWORD(wp)) {
    case SB_LINEUP: scrollLines(-1); break;
    case SB_LINEDOWN: scrollLines(1); break;
    case SB_PAGEUP: scrollPages(-1); break;
    case SB_PAGEDOWN: scrollPages(1); break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_TRACKPOS;
        GetScrollInfo(hwnd_, SB_VERT, &si);
        // Clamp to 99000 so dragging the thumb to the very bottom does not
        // exceed the range used by the position formula (nMax=100000, nPage>=1
        // means the thumb top can reach at most 99999, but we cap at 99000 to
        // keep the divide symmetric with the position-write path above).
        int trackPos = std::clamp(si.nTrackPos, 0, 99000);
        goToOffset((uint64_t)((double)trackPos / 99000.0 * (double)doc_->size()));
        break;
    }
    default: break;
    }
}

// ---- horizontal scrolling (no-wrap mode only) -------------------------------
// scrollX_/contentWidth_ are DIPs; the rest of the renderer already treats
// client pixels as DIPs 1:1 (buildVisibleLines, hitTest), so the scrollbar
// math stays in the same unit. The bar is HIDDEN whenever the visible content
// fits the page (scrollX_ snaps to 0 at once - no lingering bar after
// scrolling back to a short region) and shown only when a long line overflows.

// ShowScrollBar resizes the client area (-> WM_SIZE -> onResize -> repaint),
// so call it only when the visibility actually flips: an unconditional call
// per updateHScrollBar would re-trigger WM_SIZE on every scroll/paint and
// could loop resize -> rebuild -> resize.
void Renderer::showHBar(bool show) {
    if (hBarVisible_ == show) return;
    hBarVisible_ = show;
    ShowScrollBar(hwnd_, SB_HORZ, show);
}

void Renderer::updateHScrollBar() {
    if (wordWrap_) {
        scrollX_ = 0.0f;
        showHBar(false);
        return;
    }
    RECT rc; GetClientRect(hwnd_, &rc);
    const float page = (float)rc.right;
    const float maxX = contentWidth_ + 8.0f;      // text plus both 4 DIP margins
    if (maxX <= page) {
        // Everything visible fits: hide immediately and snap home. (The old
        // "never yank" max(contentWidth_, scrollX_ + page) range kept the bar
        // alive forever once it had appeared - the user complaint.)
        scrollX_ = 0.0f;
        showHBar(false);
        return;
    }
    // Overflow: clamp scrollX_ into the (possibly shrunken) new range first.
    const float maxScroll = maxX - page;
    if (scrollX_ > maxScroll) scrollX_ = maxScroll;
    showHBar(true);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_POS | SIF_PAGE;
    si.nMin = 0;
    si.nMax = (int)maxX - 1;                      // inclusive: max thumb pos = nMax - nPage + 1
    si.nPage = (UINT)(page > 0.0f ? page : 0.0f);
    si.nPos = (int)scrollX_;
    SetScrollInfo(hwnd_, SB_HORZ, &si, TRUE);
}

void Renderer::setScrollX(float x) {
    if (wordWrap_) return;
    RECT rc; GetClientRect(hwnd_, &rc);
    float maxScroll = contentWidth_ + 8.0f - (float)rc.right;
    if (maxScroll < 0.0f) maxScroll = 0.0f;
    if (x > maxScroll) x = maxScroll;
    if (x < 0.0f) x = 0.0f;
    if (x == scrollX_) return;
    scrollX_ = x;
    // The cached layouts are x-independent: repaint at the new origin, no
    // rebuild (linesDirty_ stays clear).
    updateHScrollBar();
    invalidate();
}

void Renderer::hScrollBy(float dips) {
    if (!doc_ || wordWrap_) return;
    setScrollX(scrollX_ + dips);
}

void Renderer::onHScroll(WPARAM wp) {
    if (!doc_ || wordWrap_) return;
    RECT rc; GetClientRect(hwnd_, &rc);
    float pageStep = (float)rc.right - 24.0f;
    if (pageStep < 24.0f) pageStep = 24.0f;
    switch (LOWORD(wp)) {
    case SB_LINELEFT:  setScrollX(scrollX_ - 24.0f); break;
    case SB_LINERIGHT: setScrollX(scrollX_ + 24.0f); break;
    case SB_PAGELEFT:  setScrollX(scrollX_ - pageStep); break;
    case SB_PAGERIGHT: setScrollX(scrollX_ + pageStep); break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_TRACKPOS;
        GetScrollInfo(hwnd_, SB_HORZ, &si);
        setScrollX((float)si.nTrackPos);
        break;
    }
    default: break;
    }
}

// Horizontal caret reveal (no-wrap): measure the caret's x by laying out the
// line prefix up to the caret (capped exactly like the rendered line, so the
// reveal agrees with what buildVisibleLines draws). Independent of lines_ -
// works right after a vertical jump, before the next paint.
void Renderer::ensureCaretVisibleX() {
    if (wordWrap_ || !doc_ || !dwFactory_ || !format_) return;
    RECT rc; GetClientRect(hwnd_, &rc);
    const float page = (float)rc.right;
    if (page <= 64.0f) return;                     // degenerate client: keep scrollX_

    const uint64_t lineStart = doc_->findPrevBreak(caret_);
    uint64_t prefixBytes = caret_ > lineStart ? caret_ - lineStart : 0;
    if (prefixBytes > kMaxLineBytes) prefixBytes = kMaxLineBytes;   // beyond the render cap
    std::wstring prefix;
    if (prefixBytes) doc_->decodeAt(lineStart, (size_t)prefixBytes, prefix);
    if (prefix.size() > kMaxLineChars) prefix.resize(kMaxLineChars);

    float cx = 0.0f;
    if (!prefix.empty()) {
        IDWriteTextLayout* tl = nullptr;
        dwFactory_->CreateTextLayout(prefix.c_str(), (UINT32)prefix.size(),
                                     format_, 1.0e6f, lineHeight_, &tl);
        if (tl) {
            DWRITE_TEXT_METRICS tm{};
            if (SUCCEEDED(tl->GetMetrics(&tm))) cx = tm.widthIncludingTrailingWhitespace;
            tl->Release();
        }
    }

    const float vx = 4.0f + cx - scrollX_;         // caret x in client DIPs
    if (vx >= 24.0f && vx <= page - 24.0f) return; // already comfortably visible

    // Minimal scroll that puts the caret back inside the 24 DIP margins keeps
    // edge drag-selection smooth; a jump farther than half a page (find hit,
    // Ctrl+End on a long line) centers the caret instead.
    const float lo = 4.0f + cx - (page - 24.0f);   // scrollX_ placing the caret at the right margin
    const float hi = 4.0f + cx - 24.0f;            // ... at the left margin
    float ns = scrollX_ < lo ? lo : (scrollX_ > hi ? hi : scrollX_);
    if (std::fabs(ns - scrollX_) > page * 0.5f) ns = 4.0f + cx - page * 0.5f;
    if (ns < 0.0f) ns = 0.0f;
    if (ns == scrollX_) return;
    // Assign directly (no setScrollX clamp): contentWidth_ may be stale while
    // linesDirty_. Bump it to at least the caret line's measured prefix width
    // (a legitimate lower bound on a soon-visible line) so updateHScrollBar's
    // fit-check/clamp cannot yank the fresh scrollX_ before the next rebuild.
    if (cx > contentWidth_) contentWidth_ = cx;
    scrollX_ = ns;
    updateHScrollBar();
    invalidate();
}

} // namespace fastpad
