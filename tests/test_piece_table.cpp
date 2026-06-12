#include "doctest/doctest.h"
#include "core/piece_table.h"
#include <cstring>
#include <string>

using namespace fastpad;

// Original buffer backed by a string for tests.
static PieceTable make(const std::string& orig) {
    auto reader = [orig](uint64_t off, void* dst, size_t len) -> bool {
        if (off + len > orig.size()) return false;
        memcpy(dst, orig.data() + off, len);
        return true;
    };
    auto counter = [orig](uint64_t b, uint64_t e) -> uint64_t {
        uint64_t n = 0;
        for (uint64_t i = b; i < e; ++i) if (orig[(size_t)i] == '\n') n++;
        return n;
    };
    return PieceTable(orig.size(), reader, counter, LineStride::OneByte);
}

static std::string text_of(const PieceTable& pt) {
    std::string s((size_t)pt.size(), '\0');
    REQUIRE(pt.read(0, s.data(), s.size()));
    return s;
}

static std::vector<uint8_t> bytes(const char* s) { return {s, s + strlen(s)}; }

TEST_CASE("piecetable: starts as one original piece") {
    auto pt = make("hello");
    CHECK(pt.size() == 5);
    CHECK(text_of(pt) == "hello");
    CHECK(pt.pieceCount() == 1);
}

TEST_CASE("piecetable: insert middle / start / end") {
    auto pt = make("helLO");
    auto b = bytes("xx");
    pt.insert(3, b.data(), b.size());
    CHECK(text_of(pt) == "helxxLO");
    pt.insert(0, b.data(), b.size());
    CHECK(text_of(pt) == "xxhelxxLO");
    pt.insert(pt.size(), b.data(), b.size());
    CHECK(text_of(pt) == "xxhelxxLOxx");
}

TEST_CASE("piecetable: erase within / across pieces") {
    auto pt = make("abcdef");
    auto b = bytes("123");
    pt.insert(3, b.data(), b.size());           // abc123def
    pt.erase(2, 3);                              // ab 3def -> "ab3def"
    CHECK(text_of(pt) == "ab3def");
    pt.erase(0, pt.size());
    CHECK(pt.size() == 0);
    CHECK(text_of(pt) == "");
}

TEST_CASE("piecetable: sequential typing coalesces into one add piece") {
    auto pt = make("");
    for (char c : std::string("typed")) {
        uint8_t byte = (uint8_t)c;
        pt.insert(pt.size(), &byte, 1);
    }
    CHECK(text_of(pt) == "typed");
    CHECK(pt.pieceCount() == 1);                 // coalesced
}

TEST_CASE("piecetable: read spanning original and add pieces") {
    auto pt = make("aaabbb");
    auto b = bytes("XY");
    pt.insert(3, b.data(), b.size());            // aaaXYbbb
    char buf[4];
    REQUIRE(pt.read(2, buf, 4));
    CHECK(std::string(buf, 4) == "aXYb");
    CHECK_FALSE(pt.read(6, buf, 4));             // 6+4 > 8
}

TEST_CASE("piecetable: undo/redo round-trip") {
    auto pt = make("abcdef");
    auto b = bytes("123");
    pt.insert(3, b.data(), b.size());            // abc123def
    pt.erase(0, 2);                              // c123def
    CHECK(text_of(pt) == "c123def");
    pt.undo();
    CHECK(text_of(pt) == "abc123def");
    pt.undo();
    CHECK(text_of(pt) == "abcdef");
    CHECK_FALSE(pt.canUndo());
    pt.redo();
    CHECK(text_of(pt) == "abc123def");
    pt.redo();
    CHECK(text_of(pt) == "c123def");
    CHECK_FALSE(pt.canRedo());
}

TEST_CASE("piecetable: new edit truncates redo tail") {
    auto pt = make("xy");
    auto b = bytes("A");
    pt.insert(0, b.data(), 1);                   // Axy
    pt.undo();                                   // xy
    auto c = bytes("B");
    pt.insert(2, c.data(), 1);                   // xyB
    CHECK_FALSE(pt.canRedo());
    CHECK(text_of(pt) == "xyB");
}

TEST_CASE("piecetable: coalesced typing undoes as one unit") {
    auto pt = make("ab");
    std::string typed = "123";
    for (size_t i = 0; i < typed.size(); ++i) { uint8_t u = (uint8_t)typed[i]; pt.insert(2 + i, &u, 1); }
    CHECK(text_of(pt) == "ab123");
    pt.undo();
    CHECK(text_of(pt) == "ab");                  // ONE undo removes all coalesced typing
}

TEST_CASE("piecetable: line break accounting across edits") {
    auto pt = make("a\nb\nc");                   // 2 breaks
    CHECK(pt.lineBreaksTotal() == 2);
    CHECK(pt.lineBreaksBefore(2) == 1);
    auto b = bytes("x\ny\n");
    pt.insert(1, b.data(), b.size());            // a x\ny\n \nb\nc -> total 4
    CHECK(pt.lineBreaksTotal() == 4);
    CHECK(pt.lineBreaksBefore(pt.size()) == 4);
    pt.erase(0, pt.size());
    CHECK(pt.lineBreaksTotal() == 0);
    pt.undo();
    CHECK(pt.lineBreaksTotal() == 4);
}
