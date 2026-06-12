#include "doctest/doctest.h"
#include "core/document.h"
#include "test_helpers.h"
#include <string>
#include <vector>

using namespace fastpad;

static std::wstring decode_all(Document& d) {
    std::wstring out;
    d.decodeAt(0, (size_t)d.size() + 16, out);
    return out;
}

TEST_CASE("document edit: insertText/eraseRange/undo/redo over UTF-8 file") {
    TempFileGuard g{ write_temp_file({'h','e','l','l','o','\n','w','o','r','l','d'}) };
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    d.waitForIndex();
    CHECK_FALSE(d.dirty());

    d.insertText(5, L" there");                  // hello there\nworld
    CHECK(d.dirty());
    CHECK(d.size() == 17);
    CHECK(decode_all(d) == L"hello there\nworld");

    d.eraseRange(0, 6);                          // there\nworld
    CHECK(decode_all(d) == L"there\nworld");

    d.undo(); d.undo();
    CHECK(decode_all(d) == L"hello\nworld");
    CHECK_FALSE(d.canUndo());
    d.redo();
    CHECK(decode_all(d) == L"hello there\nworld");
}

TEST_CASE("document edit: typed Cyrillic encodes to the file's codepage") {
    TempFileGuard g{ write_temp_file({'a','b'}) };
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    // Document is still clean, so the normal encoding switch is allowed.
    REQUIRE(d.setEncoding({EncodingKind::Ansi, 1251, 0}));
    d.insertText(1, L"\x042F");                  // Cyrillic Ya -> 0xDF in cp1251
    uint8_t buf[3];
    REQUIRE(d.read(0, buf, 3));
    CHECK(buf[0] == 'a'); CHECK(buf[1] == 0xDF); CHECK(buf[2] == 'b');
}

TEST_CASE("document edit: findNextBreak/findPrevBreak on live edited content") {
    TempFileGuard g{ write_temp_file({'a','a','\n','b','b'}) };
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    d.waitForIndex();
    CHECK(d.findNextBreak(0) == 3);
    d.insertText(0, L"x\n");                     // x\naa\nbb
    CHECK(d.findNextBreak(0) == 2);
    CHECK(d.findNextBreak(2) == 5);
    CHECK(d.findPrevBreak(6) == 5);
    CHECK(d.findPrevBreak(1) == 0);
}

TEST_CASE("document edit: UTF-16 break scan ignores 0x0A inside CJK units") {
    // U+4E0A = bytes 0A 4E in LE; must NOT be a line break.
    std::vector<uint8_t> b{0xFF,0xFE, 0x0A,0x4E, '\n',0, 'z',0};
    TempFileGuard g{ write_temp_file(b) };
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    d.waitForIndex();
    CHECK(d.findNextBreak(2) == 6);              // breaks after the real 000A unit
}

TEST_CASE("document edit: eolBytes detects CRLF vs LF; default CRLF") {
    TempFileGuard a{ write_temp_file({'x','\r','\n','y'}) };
    Document d1; REQUIRE(d1.open(a.path.c_str(), nullptr));
    CHECK(d1.eolBytes() == std::vector<uint8_t>{'\r','\n'});
    TempFileGuard b{ write_temp_file({'x','\n','y'}) };
    Document d2; REQUIRE(d2.open(b.path.c_str(), nullptr));
    CHECK(d2.eolBytes() == std::vector<uint8_t>{'\n'});
    TempFileGuard c{ write_temp_file({'x','y'}) };
    Document d3; REQUIRE(d3.open(c.path.c_str(), nullptr));
    CHECK(d3.eolBytes() == std::vector<uint8_t>{'\r','\n'});
}

TEST_CASE("document edit: charStep over UTF-8 multibyte") {
    TempFileGuard g{ write_temp_file({'a', 0xD0,0xAF, 'b'}) };   // a Ya b in UTF-8
    Document d; REQUIRE(d.open(g.path.c_str(), nullptr));
    CHECK(d.charStepForward(0) == 1);
    CHECK(d.charStepForward(1) == 3);
    CHECK(d.charStepBackward(3) == 1);
    CHECK(d.charStepBackward(4) == 3);
}

TEST_CASE("document edit: lineBreaksBefore exact on small indexed file") {
    TempFileGuard g{ write_temp_file({'a','\n','b','\n','c'}) };
    Document d; REQUIRE(d.open(g.path.c_str(), nullptr));
    d.waitForIndex();
    d.insertText(0, L"q\n");
    auto n = d.lineBreaksBefore(4);              // q\na\n -> 2 breaks before offset 4
    REQUIRE(n.has_value());
    CHECK(*n == 2);
}

TEST_CASE("document edit: break counter includes a break ending exactly at the range end") {
    // Original "a\n": the initial piece's counter range is [0,2); the break's
    // END offset (offset AFTER the LF) is exactly 2 and must still be counted.
    // Pins the indexed-diff formula lineOfOffset(end) - lineOfOffset(begin).
    TempFileGuard g{ write_temp_file({'a','\n'}) };
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    d.waitForIndex();
    d.insertText(2, L"x");                       // a\nx - materializes pieces
    auto n = d.lineBreaksBefore(d.size());
    REQUIRE(n.has_value());
    CHECK(*n == 1);
}

TEST_CASE("normalize_paste: CRLF clipboard text into an LF document") {
    EncodingInfo enc{EncodingKind::Utf8, 65001, 0};
    auto b = normalize_paste(enc, L"a\r\nb\r\n", {'\n'});
    CHECK(b == std::vector<uint8_t>{'a','\n','b','\n'});
}

TEST_CASE("normalize_paste: LF clipboard text into a CRLF document") {
    EncodingInfo enc{EncodingKind::Utf8, 65001, 0};
    auto b = normalize_paste(enc, L"a\nb", {'\r','\n'});
    CHECK(b == std::vector<uint8_t>{'a','\r','\n','b'});
}

TEST_CASE("document edit: setEncoding refused while dirty") {
    TempFileGuard g{ write_temp_file({'a'}) };
    Document d; REQUIRE(d.open(g.path.c_str(), nullptr));
    d.insertText(0, L"x");
    CHECK_FALSE(d.setEncoding({EncodingKind::Utf16LE, 1200, 0}));
}
