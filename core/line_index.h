#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fastpad {

class MmapFile;

enum class LineStride { OneByte, TwoByteLE, TwoByteBE };

// Stores line-break offsets in 16 MB chunks (32-bit deltas within a chunk).
// feed() must be called with strictly sequential, contiguous ranges by ONE
// writer; queries from other threads share the mutex. Queries are valid for
// the indexed prefix [0, indexedBytes()).
class LineIndex {
public:
    explicit LineIndex(LineStride stride);
    void feed(uint64_t offset, const uint8_t* data, size_t len);
    void finish(uint64_t fileSize);
    bool complete(uint64_t fileSize) const;
    uint64_t indexedBytes() const { return indexed_.load(); }
    uint64_t lineCount() const;                     // >= 1
    uint64_t lineStart(uint64_t lineNo) const;      // 0-based; lineNo < lineCount()
    uint64_t lineOfOffset(uint64_t offset) const;   // offset < indexedBytes()

private:
    static constexpr uint64_t kChunk = 16ull << 20;
    void addBreak(uint64_t afterOffset);            // caller holds m_

    LineStride stride_;
    mutable std::mutex m_;
    std::vector<std::vector<uint32_t>> chunks_;     // break positions (offset AFTER the break) per chunk
    std::vector<uint64_t> breaksBeforeChunk_;       // prefix sums; breaksBeforeChunk_[i] = total breaks in chunks [0, i)
    std::atomic<uint64_t> indexed_{0};
    std::atomic<bool> finished_{false};
    bool pendingCR_ = false;
};

// Owns a background thread that walks an MmapFile sequentially and feeds a
// LineIndex. start() once per instance.
class Indexer {
public:
    ~Indexer() { cancel(); wait(); }
    using Progress = std::function<void(uint64_t indexed, uint64_t total)>;
    // The progress callback runs on the indexer thread.
    void start(std::shared_ptr<MmapFile> file, LineIndex* index, uint64_t startOffset, Progress progress);
    void cancel() { cancel_.store(true); }
    void wait();

private:
    std::thread t_;
    std::atomic<bool> cancel_{false};
};

} // namespace fastpad
