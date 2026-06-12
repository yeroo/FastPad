#include "core/search.h"
#include "core/encoding.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace fastpad {

namespace {

// Cancel is checked per chunk AND at least once per this many scanned bytes
// inside a chunk.
constexpr size_t kCancelStride = 1u << 20;

inline uint8_t fold_ascii(uint8_t b) {
    return (b >= 'A' && b <= 'Z') ? static_cast<uint8_t>(b + 0x20) : b;
}

// In-place ASCII case fold of a byte buffer per the document encoding.
// absBase = absolute document offset of buf[0] (drives UTF-16 byte parity).
// - Utf8/Ansi: fold every A-Z byte. Safe in UTF-8 (ASCII bytes never occur
//   inside multibyte sequences); for ANSI DBCS codepages a trail byte could
//   theoretically fold - documented ASCII-only limitation.
// - UTF-16: fold only the LOW byte of a unit whose HIGH byte is 0 (i.e. units
//   U+0041..U+005A). LE: low byte sits at even absolute offsets, high at +1;
//   BE: low byte at odd absolute offsets, high at -1. A low byte whose high
//   neighbor falls outside the buffer is left unfolded - such a byte can only
//   belong to a unit that extends past the buffer, which no candidate match
//   in this buffer covers.
void fold_buffer(std::vector<uint8_t>& buf, uint64_t absBase, EncodingKind kind) {
    const size_t n = buf.size();
    if (kind == EncodingKind::Utf16LE) {
        for (size_t i = 0; i < n; ++i) {
            if (((absBase + i) & 1) != 0) continue;          // not a low byte
            if (i + 1 >= n || buf[i + 1] != 0) continue;     // high byte not 0
            buf[i] = fold_ascii(buf[i]);
        }
    } else if (kind == EncodingKind::Utf16BE) {
        for (size_t i = 0; i < n; ++i) {
            if (((absBase + i) & 1) == 0) continue;          // not a low byte
            if (i == 0 || buf[i - 1] != 0) continue;         // high byte not 0
            buf[i] = fold_ascii(buf[i]);
        }
    } else {
        for (size_t i = 0; i < n; ++i) buf[i] = fold_ascii(buf[i]);
    }
}

} // namespace

std::optional<uint64_t> search_document(Document& doc, const std::wstring& needle,
    uint64_t from, const SearchOptions& opts,
    const std::function<void(uint64_t scanned, uint64_t total)>& progress,
    const std::atomic<bool>& cancel) {

    std::vector<uint8_t> pat = encode_text(doc.encoding(), needle);
    if (pat.empty()) return std::nullopt;

    const EncodingKind kind = doc.encoding().kind;
    const bool utf16 = (kind == EncodingKind::Utf16LE || kind == EncodingKind::Utf16BE);
    const bool fold = !opts.caseSensitive;
    // Fold the needle ONCE. Its bytes align at even absolute offsets in UTF-16
    // (candidates are alignment-checked), so absBase 0 gives the right parity.
    if (fold) fold_buffer(pat, 0, kind);

    const uint64_t total = doc.size();
    const size_t n = pat.size();
    if (total < n) return std::nullopt;
    const size_t chunk = opts.chunkSize ? opts.chunkSize : (4u << 20);
    std::vector<uint8_t> buf;

    if (opts.forward) {
        // First match at offset >= from.
        if (from > total - n) return std::nullopt;
        const uint64_t toScan = total - from;
        uint64_t scanned = 0;
        for (uint64_t base = from; base <= total - n; base += chunk) {
            if (cancel.load(std::memory_order_relaxed)) return std::nullopt;
            // Read chunk + (n-1) overlap so straddling matches are found here;
            // candidates beyond chunk-1 belong to the NEXT chunk (no dupes).
            const size_t want = static_cast<size_t>(
                std::min<uint64_t>(static_cast<uint64_t>(chunk) + n - 1, total - base));
            buf.resize(want);
            if (!doc.read(base, buf.data(), want)) return std::nullopt;
            if (fold) fold_buffer(buf, base, kind);
            const size_t lastP = std::min(chunk - 1, want - n);
            const uint8_t first = pat[0];
            size_t p = 0;
            while (p <= lastP) {
                // 1 MB segments: cancel check inside large chunks.
                const size_t segEnd = std::min(lastP, p + (kCancelStride - 1));
                size_t q = p;
                while (q <= segEnd) {
                    const void* hit = memchr(buf.data() + q, first, segEnd - q + 1);
                    if (!hit) break;
                    q = static_cast<size_t>(static_cast<const uint8_t*>(hit) - buf.data());
                    if ((!utf16 || ((base + q) & 1) == 0) &&
                        memcmp(buf.data() + q, pat.data(), n) == 0)
                        return base + q;
                    ++q;
                }
                p = segEnd + 1;
                if (p <= lastP && cancel.load(std::memory_order_relaxed)) return std::nullopt;
            }
            scanned += lastP + 1;
            if (progress) progress(std::min(scanned, toScan), toScan);
        }
        return std::nullopt;
    }

    // Backward: LAST match with offset < from (from itself excluded).
    if (from == 0) return std::nullopt;
    uint64_t hi = std::min(from - 1, total - n);      // highest candidate offset
    const uint64_t toScan = hi + 1;                   // candidate positions to visit
    uint64_t scanned = 0;
    for (;;) {
        if (cancel.load(std::memory_order_relaxed)) return std::nullopt;
        // Candidates [lo, hi]; read [lo, hi + n) so the last one can compare.
        const uint64_t lo = (hi >= chunk - 1) ? hi - (chunk - 1) : 0;
        const size_t len = static_cast<size_t>(hi - lo) + n;
        buf.resize(len);
        if (!doc.read(lo, buf.data(), len)) return std::nullopt;
        if (fold) fold_buffer(buf, lo, kind);
        size_t p = static_cast<size_t>(hi - lo);
        for (;;) {
            // Descend in 1 MB segments with a cancel check between them.
            const size_t segLo = (p >= kCancelStride - 1) ? p - (kCancelStride - 1) : 0;
            for (size_t q = p + 1; q-- > segLo;) {
                if (buf[q] != pat[0]) continue;
                if (utf16 && ((lo + q) & 1) != 0) continue;
                if (memcmp(buf.data() + q, pat.data(), n) == 0) return lo + q;
            }
            if (segLo == 0) break;
            if (cancel.load(std::memory_order_relaxed)) return std::nullopt;
            p = segLo - 1;
        }
        scanned += hi - lo + 1;
        if (progress) progress(std::min(scanned, toScan), toScan);
        if (lo == 0) break;
        hi = lo - 1;
    }
    return std::nullopt;
}

} // namespace fastpad
