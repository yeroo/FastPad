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
