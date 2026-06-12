#include "doctest/doctest.h"
#include "core/document.h"
#include "test_helpers.h"
#include <cstring>
#include <string>

using namespace fastpad;

TEST_CASE("document: open detects encoding, indexes, navigates") {
    auto b = std::vector<uint8_t>{0xEF,0xBB,0xBF};
    const char* text = "line one\nline two\nline three";
    b.insert(b.end(), text, text + strlen(text));
    TempFileGuard g{ write_temp_file(b) };

    Document d;
    std::wstring err;
    REQUIRE(d.open(g.path.c_str(), &err));
    CHECK(d.size() == b.size());
    CHECK(d.encoding().kind == EncodingKind::Utf8);
    CHECK(d.encoding().bomBytes == 3);
    d.waitForIndex();
    CHECK(d.lineCount() == 3);
    CHECK(d.lineStart(1) == 3 + 9);
}

TEST_CASE("document: reopen with encoding override restarts index with right stride") {
    std::vector<uint8_t> b;
    for (char c : std::string("a\nb\nc")) { b.push_back((uint8_t)c); b.push_back(0); }
    TempFileGuard g{ write_temp_file(b) };

    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    REQUIRE(d.setEncoding({EncodingKind::Utf16LE, 1200, 0}));   // clean doc -> allowed
    d.waitForIndex();
    CHECK(d.lineCount() == 3);
    CHECK(d.encoding().kind == EncodingKind::Utf16LE);
}

TEST_CASE("document: decodeAt returns text and consumed bytes") {
    TempFileGuard g{ write_temp_file({'h','i','\n','y','o'}) };
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    std::wstring out;
    size_t consumed = d.decodeAt(0, 5, out);
    CHECK(consumed == 5);
    CHECK(out == L"hi\nyo");
}
