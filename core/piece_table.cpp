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
    if (offset + len < offset) len = size_ - offset;   // wrap guard (mirrors read's overflow check)
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
