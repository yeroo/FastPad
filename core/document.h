#pragma once
#include "core/encoding.h"
#include "core/line_index.h"
#include "core/mmap_file.h"
#include "core/piece_table.h"
#include <memory>
#include <optional>
#include <vector>

namespace fastpad {

// Document facade: memory-mapped original file + detected encoding + background
// line index (M1), plus an editable PieceTable layer (M2) that materializes on
// the first edit. The original file is never modified; edits live in RAM.
class Document {
public:
    ~Document();
    bool open(const wchar_t* path, std::wstring* error);
    void close();

    // Live size: piece-table size once edits exist, else the mapped file size.
    uint64_t size() const { return pieces_ ? pieces_->size() : (file_ ? file_->size() : 0); }
    const std::wstring& path() const { return path_; }
    const EncodingInfo& encoding() const { return enc_; }
    // Document is UI-thread-affine: all calls including setEncoding/close must come
    // from one thread. Indexer progress callbacks run on the indexer thread and must
    // NOT call back into Document - marshal via PostMessage.
    // Refused (returns false) while dirty - reinterpreting bytes mid-edit is a
    // footgun. When clean: drops any fully-undone edit history (the bytes are
    // identical to the original) and restarts line indexing with the new stride.
    bool setEncoding(const EncodingInfo& enc);

    // Decode up to maxBytes starting at offset; returns bytes consumed.
    // Reads LIVE content (pieces once they exist).
    size_t decodeAt(uint64_t offset, size_t maxBytes, std::wstring& out);
    // Zero-copy access to the ORIGINAL bytes (ignores edits - M1 API kept for
    // the indexer/save paths). Never cache the returned pointer across other
    // Document calls - consume immediately.
    const uint8_t* pin(uint64_t offset, size_t* available) { return file_ ? file_->pin(offset, available) : nullptr; }

    uint64_t indexedBytes() const { return index_ ? index_->indexedBytes() : 0; }
    bool indexComplete() const { return index_ && index_->complete(file_ ? file_->size() : 0); }
    uint64_t lineCount() const { return index_ ? index_->lineCount() : 1; }
    uint64_t lineStart(uint64_t lineNo) const { return index_ ? index_->lineStart(lineNo) : 0; }
    uint64_t lineOfOffset(uint64_t off) const { return index_ ? index_->lineOfOffset(off) : 0; }

    void waitForIndex();

    // ---- M2 editing (UI-thread-affine like everything else) ----
    bool dirty() const { return pieces_ && pieces_->canUndo(); }
    bool read(uint64_t offset, void* dst, size_t len);       // pieces when present, else mmap
    void insertText(uint64_t offset, const std::wstring& text);  // encodes via encoding()
    void insertBytes(uint64_t offset, const uint8_t* bytes, size_t len);
    void eraseRange(uint64_t offset, uint64_t len);
    bool canUndo() const { return pieces_ && pieces_->canUndo(); }
    bool canRedo() const { return pieces_ && pieces_->canRedo(); }
    void undo() { if (pieces_) pieces_->undo(); }
    void redo() { if (pieces_) pieces_->redo(); }
    // EOL byte sequence for Enter, from the first break found in the first
    // 64 KB of live content (CRLF default when none found). Cached per open.
    std::vector<uint8_t> eolBytes();
    // Steps one CHARACTER forward/backward from offset (UTF-8 continuation
    // aware; UTF-16 steps 2; ANSI DBCS steps via IsDBCSLeadByteEx best-effort).
    uint64_t charStepForward(uint64_t offset);
    uint64_t charStepBackward(uint64_t offset);
    // Line-break scans over the LIVE content (pieces when present). Full-unit
    // comparison for UTF-16 - no false positives on 0x0A bytes inside CJK
    // units. Backward scan capped at 1 MB (returns the scan floor when no
    // break is found within the cap).
    uint64_t findNextBreak(uint64_t offset);    // offset AFTER the next break at/past offset; size() if none
    uint64_t findPrevBreak(uint64_t offset);    // last break end STRICTLY before offset (start of the line containing offset-1); 0 if none
    // Post-save reset: closes everything (pieces, history, index, mapping) and
    // reopens the saved file as the new pristine original. Returns false +
    // error when the reopen fails (document is then closed/empty).
    bool reopenAfterSave(const wchar_t* savedPath, std::wstring* err) {
        close();
        return open(savedPath, err);
    }
    // Breaks in [0, offset) of live content. Exact via the LineIndex diff when
    // the needed original ranges are indexed, or via a raw scan when a range
    // is <= 16 MB. Returns nullopt once any original range was unresolvable at
    // piece-split time (lineNumbersApproximate_) - the status bar then shows
    // offsets only. Break counting is '\n'-unit based (CRLF counts once; a
    // lone CR counts via the index for original ranges but not for typed
    // text - documented M2 approximation).
    std::optional<uint64_t> lineBreaksBefore(uint64_t offset);

private:
    void restartIndex();
    void ensurePieces();
    // Exact '\n'-break count over the ORIGINAL file range [begin, end), or
    // nullopt when the range is unindexed and too large (> 16 MB) to scan.
    std::optional<uint64_t> countOriginalBreaks(uint64_t begin, uint64_t end);
    static LineStride strideFor(const EncodingInfo& e);

    std::shared_ptr<MmapFile> file_;
    std::shared_ptr<MmapFile> indexFile_;            // indexer's own instance (no view contention)
    std::unique_ptr<LineIndex> index_;
    std::unique_ptr<Indexer> indexer_;
    std::unique_ptr<PieceTable> pieces_;             // created lazily on first edit
    EncodingInfo enc_{};
    std::wstring path_;
    std::vector<uint8_t> eolCache_;
    bool lineNumbersApproximate_ = false;
};

// Converts clipboard text into document bytes for pasting: CRLF pairs collapse
// to a single break, then every break becomes the document's eolBytes sequence;
// the text runs between breaks go through encode_text. Lone CRs pass through
// unchanged (encoded as-is).
std::vector<uint8_t> normalize_paste(const EncodingInfo& enc, const std::wstring& text,
                                     const std::vector<uint8_t>& eolBytes);

} // namespace fastpad
