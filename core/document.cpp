#include "core/document.h"
#include <algorithm>
#include <vector>

namespace fastpad {

static const uint64_t kBackScanCap = 1ull << 20;     // findPrevBreak backward cap
static const uint64_t kMaxCounterScan = 16ull << 20; // raw break-count fallback cap
static const size_t kScanChunk = 64 * 1024;

// Reads one stride unit. For wide strides the two bytes at p form a single
// 16-bit code unit; comparisons against '\n'/'\r' are full-unit (no false
// positives on 0x0A bytes inside CJK units).
static uint32_t unit_at(const uint8_t* p, bool wide, bool le) {
    if (!wide) return p[0];
    return le ? (uint32_t)(p[0] | (p[1] << 8)) : (uint32_t)((p[0] << 8) | p[1]);
}

Document::~Document() { close(); }

LineStride Document::strideFor(const EncodingInfo& e) {
    switch (e.kind) {
        case EncodingKind::Utf16LE: return LineStride::TwoByteLE;
        case EncodingKind::Utf16BE: return LineStride::TwoByteBE;
        default: return LineStride::OneByte;
    }
}

bool Document::open(const wchar_t* path, std::wstring* error) {
    close();
    file_ = std::make_shared<MmapFile>();
    if (!file_->open(path, error)) { file_.reset(); return false; }
    path_ = path;

    uint8_t sample[4096];
    size_t sampleLen = (size_t)((size() < sizeof(sample)) ? size() : sizeof(sample));
    if (sampleLen) file_->read(0, sample, sampleLen);
    enc_ = detect_encoding(sample, sampleLen);

    restartIndex();
    return true;
}

void Document::restartIndex() {
    if (indexer_) { indexer_->cancel(); indexer_->wait(); }
    indexer_.reset();
    index_ = std::make_unique<LineIndex>(strideFor(enc_));
    // 16 MB window × 2 views: the indexer only needs sequential forward access
    // so a small sliding window is sufficient and keeps the mapped footprint
    // bounded (~32 MB) regardless of file size (was 64 MB × 8 = 512 MB).
    indexFile_ = std::make_shared<MmapFile>(16ull << 20, 2);
    std::wstring err;
    if (!indexFile_->open(path_.c_str(), &err)) { indexFile_.reset(); return; }
    indexer_ = std::make_unique<Indexer>();
    indexer_->start(indexFile_, index_.get(), 0, nullptr);
}

bool Document::setEncoding(const EncodingInfo& enc) {
    if (dirty()) return false;
    pieces_.reset();             // clean: any fully-undone history matches the original bytes
    eolCache_.clear();
    lineNumbersApproximate_ = false;
    enc_ = enc;
    restartIndex();
    return true;
}

size_t Document::decodeAt(uint64_t offset, size_t maxBytes, std::wstring& out) {
    out.clear();
    if (!file_ || offset >= size()) return 0;
    size_t want = (size_t)((offset + maxBytes > size()) ? size() - offset : maxBytes);
    std::vector<uint8_t> buf(want);
    if (!read(offset, buf.data(), want)) return 0;
    return decode_window(enc_, buf.data(), want, out);
}

void Document::waitForIndex() { if (indexer_) indexer_->wait(); }

void Document::close() {
    if (indexer_) { indexer_->cancel(); indexer_->wait(); }
    indexer_.reset(); index_.reset(); pieces_.reset(); indexFile_.reset(); file_.reset();
    path_.clear();
    eolCache_.clear();
    lineNumbersApproximate_ = false;
}

// ---- M2 editing -----------------------------------------------------------

bool Document::read(uint64_t offset, void* dst, size_t len) {
    if (len == 0) return true;
    if (pieces_) return pieces_->read(offset, dst, len);
    return file_ && file_->read(offset, dst, len);
}

void Document::ensurePieces() {
    if (pieces_) return;
    std::shared_ptr<MmapFile> file = file_;          // keeps the mapping alive for the reader
    PieceTable::OrigReader reader = [file](uint64_t off, void* dst, size_t len) {
        return file && file->read(off, dst, len);
    };
    PieceTable::BreakCounter counter = [this](uint64_t begin, uint64_t end) -> uint64_t {
        if (auto n = countOriginalBreaks(begin, end)) return *n;
        lineNumbersApproximate_ = true;              // line numbers become unknowable
        return 0;
    };
    pieces_ = std::make_unique<PieceTable>(file_ ? file_->size() : 0,
                                           std::move(reader), std::move(counter), strideFor(enc_));
}

void Document::insertText(uint64_t offset, const std::wstring& text) {
    std::vector<uint8_t> bytes = encode_text(enc_, text);
    insertBytes(offset, bytes.data(), bytes.size());
}

void Document::insertBytes(uint64_t offset, const uint8_t* bytes, size_t len) {
    if (!file_ || len == 0) return;
    ensurePieces();
    if (offset > pieces_->size()) offset = pieces_->size();
    pieces_->insert(offset, bytes, len);
}

void Document::eraseRange(uint64_t offset, uint64_t len) {
    if (!file_ || len == 0) return;
    ensurePieces();
    pieces_->erase(offset, len);
}

std::optional<uint64_t> Document::countOriginalBreaks(uint64_t begin, uint64_t end) {
    if (end <= begin) return 0ull;
    // Indexed diff. lineOfOffset(x) counts breaks whose END offset e (offset
    // AFTER the break) satisfies e <= x. A break whose last unit starts at q
    // ends at e = q + stride, so the '\n' units q in [begin, end) are exactly
    // the breaks with e in (begin, end] - including one ending exactly at
    // `end`, which is recorded because indexing already reached `end`.
    if (index_ && end <= index_->indexedBytes())
        return index_->lineOfOffset(end) - index_->lineOfOffset(begin);
    if (!file_ || end > file_->size()) return std::nullopt;
    if (end - begin > kMaxCounterScan) return std::nullopt;

    const LineStride st = strideFor(enc_);
    const bool wide = (st != LineStride::OneByte);
    const bool le = (st == LineStride::TwoByteLE);
    uint64_t n = 0;
    std::vector<uint8_t> buf;
    uint64_t pos = wide ? begin + (begin & 1) : begin;   // wide units sit at even file offsets
    while (pos < end) {
        size_t take = (size_t)std::min<uint64_t>(kScanChunk, end - pos);
        if (wide) take &= ~(size_t)1;
        if (take == 0) break;
        buf.resize(take);
        if (!file_->read(pos, buf.data(), take)) return std::nullopt;
        const size_t step = wide ? 2 : 1;
        for (size_t i = 0; i + step <= take; i += step)
            if (unit_at(buf.data() + i, wide, le) == '\n') n++;
        pos += take;
    }
    return n;
}

std::optional<uint64_t> Document::lineBreaksBefore(uint64_t offset) {
    if (offset > size()) offset = size();
    if (pieces_) {
        const uint64_t n = pieces_->lineBreaksBefore(offset);  // may set the flag via the counter
        if (lineNumbersApproximate_) return std::nullopt;
        return n;
    }
    return countOriginalBreaks(0, offset);
}

std::vector<uint8_t> Document::eolBytes() {
    if (!eolCache_.empty()) return eolCache_;
    const wchar_t* eol = L"\r\n";                    // default CRLF
    const uint64_t cap = std::min<uint64_t>(size(), (uint64_t)kScanChunk);
    if (cap > 0) {
        std::vector<uint8_t> buf((size_t)cap);
        if (read(0, buf.data(), buf.size())) {
            const LineStride st = strideFor(enc_);
            const bool wide = (st != LineStride::OneByte);
            const bool le = (st == LineStride::TwoByteLE);
            const size_t step = wide ? 2 : 1;
            for (size_t i = 0; i + step <= buf.size(); i += step) {
                const uint32_t u = unit_at(buf.data() + i, wide, le);
                if (u == '\n') { eol = L"\n"; break; }
                if (u == '\r') {
                    const bool lf = (i + 2 * step <= buf.size()) &&
                                    unit_at(buf.data() + i + step, wide, le) == '\n';
                    eol = lf ? L"\r\n" : L"\r";
                    break;
                }
            }
        }
    }
    eolCache_ = encode_text(enc_, eol);
    return eolCache_;
}

uint64_t Document::charStepForward(uint64_t offset) {
    const uint64_t sz = size();
    if (offset >= sz) return sz;
    switch (enc_.kind) {
    case EncodingKind::Utf16LE:
    case EncodingKind::Utf16BE:
        return std::min<uint64_t>(offset + 2, sz);
    case EncodingKind::Utf8: {
        uint64_t pos = offset + 1;
        while (pos < sz) {
            uint8_t b = 0;
            if (!read(pos, &b, 1) || (b & 0xC0) != 0x80) break;  // not a continuation byte
            pos++;
        }
        return pos;
    }
    default: {                                       // Ansi: DBCS best-effort
        uint8_t b = 0;
        if (read(offset, &b, 1) && IsDBCSLeadByteEx(enc_.codepage, b) && offset + 2 <= sz)
            return offset + 2;
        return offset + 1;
    }
    }
}

uint64_t Document::charStepBackward(uint64_t offset) {
    if (offset == 0) return 0;
    const uint64_t sz = size();
    if (offset > sz) offset = sz;
    switch (enc_.kind) {
    case EncodingKind::Utf16LE:
    case EncodingKind::Utf16BE:
        return (offset >= 2) ? offset - 2 : 0;
    case EncodingKind::Utf8: {
        uint64_t pos = offset - 1;
        while (pos > 0) {
            uint8_t b = 0;
            if (!read(pos, &b, 1) || (b & 0xC0) != 0x80) break;  // landed on a lead byte
            pos--;
        }
        return pos;
    }
    default: {                                       // Ansi: DBCS best-effort
        if (offset >= 2) {
            uint8_t b = 0;
            if (read(offset - 2, &b, 1) && IsDBCSLeadByteEx(enc_.codepage, b)) return offset - 2;
        }
        return offset - 1;
    }
    }
}

uint64_t Document::findNextBreak(uint64_t offset) {
    const uint64_t sz = size();
    if (offset >= sz) return sz;
    const LineStride st = strideFor(enc_);
    const bool wide = (st != LineStride::OneByte);
    const bool le = (st == LineStride::TwoByteLE);
    const uint64_t step = wide ? 2 : 1;
    uint64_t pos = wide ? offset + (offset & 1) : offset;
    std::vector<uint8_t> buf;
    while (pos + step <= sz) {
        size_t take = (size_t)std::min<uint64_t>(kScanChunk, sz - pos);
        if (wide) take &= ~(size_t)1;
        if (take < (size_t)step) break;
        buf.resize(take);
        if (!read(pos, buf.data(), take)) return sz;
        for (size_t i = 0; i + (size_t)step <= take; i += (size_t)step) {
            const uint32_t u = unit_at(buf.data() + i, wide, le);
            if (u == '\n') return pos + i + step;
            if (u == '\r') {
                const uint64_t q = pos + i;
                uint8_t nb[2] = {0, 0};
                const bool hasNext = (q + 2 * step <= sz) && read(q + step, nb, (size_t)step);
                if (hasNext && unit_at(nb, wide, le) == '\n') return q + 2 * step;  // CRLF
                return q + step;                                                    // lone CR
            }
        }
        pos += take;
    }
    return sz;
}

uint64_t Document::findPrevBreak(uint64_t offset) {
    const uint64_t sz = size();
    if (offset > sz) offset = sz;
    if (offset == 0) return 0;
    // Fast path: clean content is exactly the original file - use the index.
    if (!dirty() && index_ && index_->indexedBytes() >= offset)
        return index_->lineStart(index_->lineOfOffset(offset - 1));

    const uint64_t start = (offset > kBackScanCap) ? offset - kBackScanCap : 0;
    std::vector<uint8_t> buf((size_t)(offset - start));
    if (!read(start, buf.data(), buf.size())) return start;
    const LineStride st = strideFor(enc_);
    const bool le = (st == LineStride::TwoByteLE);
    if (st == LineStride::OneByte) {
        for (size_t i = buf.size(); i-- > 0;) {
            const uint8_t c = buf[i];
            if (c != '\n' && c != '\r') continue;
            uint64_t e = start + i + 1;
            if (c == '\r' && i + 1 < buf.size() && buf[i + 1] == '\n') e += 1;   // CRLF
            if (e < offset) return e;
        }
        return start;
    }
    const uint64_t base = start + (start & 1);       // wide units sit at even file offsets
    const size_t off0 = (size_t)(base - start);
    const size_t count = (buf.size() - off0) / 2;
    for (size_t k = count; k-- > 0;) {
        const uint32_t u = unit_at(buf.data() + off0 + 2 * k, true, le);
        if (u != '\n' && u != '\r') continue;
        uint64_t e = base + 2 * k + 2;
        if (u == '\r' && k + 1 < count && unit_at(buf.data() + off0 + 2 * (k + 1), true, le) == '\n')
            e += 2;                                  // CRLF
        if (e < offset) return e;
    }
    return start;
}

std::vector<uint8_t> normalize_paste(const EncodingInfo& enc, const std::wstring& text,
                                     const std::vector<uint8_t>& eolBytes) {
    std::vector<uint8_t> out;
    size_t runStart = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c != L'\n' && c != L'\r') continue;
        // Lone CR (not part of a CRLF pair) passes through with the run.
        if (c == L'\r' && (i + 1 >= text.size() || text[i + 1] != L'\n')) continue;
        if (i > runStart) {
            const std::vector<uint8_t> run = encode_text(enc, text.substr(runStart, i - runStart));
            out.insert(out.end(), run.begin(), run.end());
        }
        out.insert(out.end(), eolBytes.begin(), eolBytes.end());
        if (c == L'\r') ++i;                         // skip the LF of the CRLF pair
        runStart = i + 1;
    }
    if (runStart < text.size()) {
        const std::vector<uint8_t> run = encode_text(enc, text.substr(runStart));
        out.insert(out.end(), run.begin(), run.end());
    }
    return out;
}

} // namespace fastpad
