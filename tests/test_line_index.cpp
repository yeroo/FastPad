#include "doctest/doctest.h"
#include "core/line_index.h"
#include "core/mmap_file.h"
#include "test_helpers.h"
#include <atomic>
#include <cstring>
#include <memory>

using namespace fastpad;

static std::vector<uint8_t> bytes_of(const char* s) { return {s, s + strlen(s)}; }

TEST_CASE("lineindex: LF / CRLF / lone CR all break lines") {
    LineIndex idx(LineStride::OneByte);
    auto b = bytes_of("aa\nbb\r\ncc\rdd");
    idx.feed(0, b.data(), b.size());
    idx.finish(b.size());
    CHECK(idx.lineCount() == 4);
    CHECK(idx.lineStart(0) == 0);
    CHECK(idx.lineStart(1) == 3);
    CHECK(idx.lineStart(2) == 7);
    CHECK(idx.lineStart(3) == 10);
    CHECK(idx.lineOfOffset(0) == 0);
    CHECK(idx.lineOfOffset(4) == 1);
    CHECK(idx.lineOfOffset(11) == 3);
}

TEST_CASE("lineindex: incremental feeds split across a CRLF boundary") {
    LineIndex idx(LineStride::OneByte);
    auto b = bytes_of("xy\r\nz");
    idx.feed(0, b.data(), 3);
    idx.feed(3, b.data() + 3, 2);
    idx.finish(5);
    CHECK(idx.lineCount() == 2);
    CHECK(idx.lineStart(1) == 4);
}

TEST_CASE("lineindex: lone CR at EOF breaks at fileSize") {
    LineIndex idx(LineStride::OneByte);
    auto b = bytes_of("ab\r");
    idx.feed(0, b.data(), b.size());
    idx.finish(b.size());
    CHECK(idx.lineCount() == 2);
    CHECK(idx.lineStart(1) == 3);
}

TEST_CASE("lineindex: UTF-16LE stride") {
    LineIndex idx(LineStride::TwoByteLE);
    std::vector<uint8_t> b{'a',0,'\n',0,'b',0};
    idx.feed(0, b.data(), b.size());
    idx.finish(b.size());
    CHECK(idx.lineCount() == 2);
    CHECK(idx.lineStart(1) == 4);
}

TEST_CASE("indexer: background thread indexes a real file with progress and matches naive count") {
    std::vector<uint8_t> b;
    int expectLines = 1;
    for (int i = 0; i < 200000; ++i) {
        b.push_back((uint8_t)('a' + (i % 26)));
        if (i % 37 == 0) { b.push_back('\n'); expectLines++; }
    }
    TempFileGuard g{ write_temp_file(b) };
    auto file = std::make_shared<MmapFile>(64 * 1024, 4);
    REQUIRE(file->open(g.path.c_str(), nullptr));

    LineIndex idx(LineStride::OneByte);
    std::atomic<int> progressCalls{0};
    Indexer ix;
    ix.start(file, &idx, 0, [&](uint64_t, uint64_t) { progressCalls++; });
    ix.wait();
    CHECK(idx.complete(file->size()));
    CHECK((int)idx.lineCount() == expectLines);
    CHECK(progressCalls.load() > 0);
}

TEST_CASE("indexer: cancel returns promptly without crash") {
    TempFileGuard g{ write_sparse_file(512ull << 20, {'a','\n'}, {'z'}) };
    auto file = std::make_shared<MmapFile>();
    REQUIRE(file->open(g.path.c_str(), nullptr));
    LineIndex idx(LineStride::OneByte);
    Indexer ix;
    ix.start(file, &idx, 0, nullptr);
    ix.cancel();
    ix.wait();
    CHECK(true);   // the assertion is that wait() returned
}

TEST_CASE("lineindex: empty 16MB chunks between breaks query correctly") {
    LineIndex idx(LineStride::OneByte);
    const uint64_t kChunk = 16ull << 20;
    // ~35 MB break-free, then a break in chunk 2
    std::vector<uint8_t> block(1u << 20, 'x');                  // 1 MB
    uint64_t off = 0;
    for (int i = 0; i < 35; ++i) { idx.feed(off, block.data(), block.size()); off += block.size(); }
    std::vector<uint8_t> tail{'\n','y'};
    idx.feed(off, tail.data(), tail.size());
    idx.finish(off + tail.size());
    CHECK(idx.lineCount() == 2);
    CHECK(idx.lineStart(1) == 35ull * (1u << 20) + 1);
    CHECK(idx.lineOfOffset(kChunk) == 0);
    CHECK(idx.lineOfOffset(off + 1) == 1);
}

TEST_CASE("lineindex: breaks across 16MB chunk boundary query correctly") {
    LineIndex idx(LineStride::OneByte);
    const uint64_t kChunk = 16ull << 20;
    std::vector<uint8_t> prefix((size_t)kChunk - 2, 'x');
    idx.feed(0, prefix.data(), prefix.size());
    std::vector<uint8_t> tail{'\n','y','\n','z'};                // breaks at kChunk-1 and kChunk+1
    idx.feed(prefix.size(), tail.data(), tail.size());
    idx.finish(prefix.size() + tail.size());
    CHECK(idx.lineCount() == 3);
    CHECK(idx.lineStart(1) == kChunk - 1);
    CHECK(idx.lineStart(2) == kChunk + 1);
    CHECK(idx.lineOfOffset(kChunk) == 1);
    CHECK(idx.lineOfOffset(kChunk + 1) == 2);
}
