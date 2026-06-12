#include "core/line_index.h"
#include "core/mmap_file.h"
#include <algorithm>

namespace fastpad {

LineIndex::LineIndex(LineStride stride) : stride_(stride) {}

// Caller holds m_. Break offsets arrive in strictly increasing order (one
// sequential writer), so once chunk i exists no break is ever added to an
// earlier chunk -- the prefix sums computed at append time stay correct.
void LineIndex::addBreak(uint64_t afterOffset) {
    const size_t chunk = (size_t)(afterOffset / kChunk);
    while (chunks_.size() <= chunk) {
        breaksBeforeChunk_.push_back(chunks_.empty()
            ? 0
            : breaksBeforeChunk_.back() + (uint64_t)chunks_.back().size());
        chunks_.emplace_back();
    }
    chunks_[chunk].push_back((uint32_t)(afterOffset % kChunk));
}

void LineIndex::feed(uint64_t offset, const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(m_);
    if (stride_ == LineStride::OneByte) {
        for (size_t i = 0; i < len; ++i) {
            const uint8_t c = data[i];
            if (pendingCR_) {
                pendingCR_ = false;
                if (c == '\n') { addBreak(offset + i + 1); continue; }
                addBreak(offset + i);          // lone CR ended a line just before this byte
            }
            if (c == '\n') addBreak(offset + i + 1);
            else if (c == '\r') pendingCR_ = true;
        }
    } else {
        // Examine 2-byte units at even file offsets. Feeds start at even
        // offsets in practice (64 KB-aligned windows); compute defensively.
        for (uint64_t pos = offset + (offset & 1); pos + 1 < offset + len; pos += 2) {
            const size_t i = (size_t)(pos - offset);
            const uint16_t u = (stride_ == LineStride::TwoByteLE)
                ? (uint16_t)(data[i] | (data[i + 1] << 8))
                : (uint16_t)((data[i] << 8) | data[i + 1]);
            if (pendingCR_) {
                pendingCR_ = false;
                if (u == u'\n') { addBreak(pos + 2); continue; }
                addBreak(pos);                 // lone CR ended a line just before this unit
            }
            if (u == u'\n') addBreak(pos + 2);
            else if (u == u'\r') pendingCR_ = true;
        }
    }
    indexed_.store(offset + len);
}

void LineIndex::finish(uint64_t fileSize) {
    std::lock_guard<std::mutex> lock(m_);
    if (pendingCR_) {
        pendingCR_ = false;
        addBreak(fileSize);
    }
    indexed_.store(fileSize);
    finished_.store(true);
}

bool LineIndex::complete(uint64_t fileSize) const {
    return finished_.load() && indexed_.load() >= fileSize;
}

uint64_t LineIndex::lineCount() const {
    std::lock_guard<std::mutex> lock(m_);
    const uint64_t breaks = chunks_.empty()
        ? 0
        : breaksBeforeChunk_.back() + (uint64_t)chunks_.back().size();
    return breaks + 1;
}

uint64_t LineIndex::lineStart(uint64_t lineNo) const {
    if (lineNo == 0) return 0;
    std::lock_guard<std::mutex> lock(m_);
    const uint64_t breakIdx = lineNo - 1;      // line N starts after break N-1
    auto it = std::upper_bound(breaksBeforeChunk_.begin(), breaksBeforeChunk_.end(), breakIdx);
    const size_t c = (size_t)(it - breaksBeforeChunk_.begin()) - 1;
    return (uint64_t)c * kChunk + chunks_[c][(size_t)(breakIdx - breaksBeforeChunk_[c])];
}

uint64_t LineIndex::lineOfOffset(uint64_t offset) const {
    std::lock_guard<std::mutex> lock(m_);
    if (chunks_.empty()) return 0;
    const uint64_t chunk = offset / kChunk;
    if (chunk >= chunks_.size())               // all breaks lie at or before offset
        return breaksBeforeChunk_.back() + (uint64_t)chunks_.back().size();
    const auto& v = chunks_[(size_t)chunk];
    const uint32_t rel = (uint32_t)(offset % kChunk);
    const uint64_t inChunk = (uint64_t)(std::upper_bound(v.begin(), v.end(), rel) - v.begin());
    return breaksBeforeChunk_[(size_t)chunk] + inChunk;
}

void Indexer::start(std::shared_ptr<MmapFile> file, LineIndex* index, uint64_t startOffset, Progress progress) {
    t_ = std::thread([this, file = std::move(file), index, startOffset, progress = std::move(progress)]() {
        constexpr uint64_t kProgressEvery = 32ull << 20;
        constexpr size_t kFeedSlice = 2ull << 20;
        const uint64_t total = file->size();
        uint64_t off = startOffset;
        uint64_t lastReported = off;
        while (off < total && !cancel_.load()) {
            size_t avail = 0;
            const uint8_t* p = file->pin(off, &avail);
            if (!p) break;
            const size_t windowEnd = (size_t)std::min<uint64_t>(avail, total - off);
            size_t done = 0;
            while (done < windowEnd && !cancel_.load()) {
                size_t take = ((windowEnd - done) < kFeedSlice) ? (windowEnd - done) : kFeedSlice;
                index->feed(off + done, p + done, take);
                done += take;
            }
            off += done;
            if (done < windowEnd) break;        // cancelled mid-window
            if (progress && (off - lastReported >= kProgressEvery || off >= total)) {
                lastReported = off;
                progress(off, total);
            }
        }
        if (off >= total) index->finish(total);  // a cancelled run must not mark finished
    });
}

void Indexer::wait() {
    if (t_.joinable()) t_.join();
}

} // namespace fastpad
