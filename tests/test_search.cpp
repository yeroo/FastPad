#include "doctest/doctest.h"
#include "core/search.h"
#include "test_helpers.h"
#include <atomic>
#include <cstring>
#include <memory>

using namespace fastpad;

static std::atomic<bool> g_noCancel{false};

static std::optional<uint64_t> find(Document& d, const wchar_t* needle, uint64_t from,
                                    bool cs = true, bool fwd = true, size_t chunk = 4u << 20) {
    SearchOptions o; o.caseSensitive = cs; o.forward = fwd; o.chunkSize = chunk;
    return search_document(d, needle, from, o, nullptr, g_noCancel);
}

// Document is non-copyable/non-movable (unique_ptr members + user-declared
// dtor), so the helper hands back a unique_ptr instead of a value.
static std::unique_ptr<Document> open_bytes(const std::vector<uint8_t>& b, std::wstring* keepPath, TempFileGuard& g) {
    g.path = write_temp_file(b);
    if (keepPath) *keepPath = g.path;
    auto d = std::make_unique<Document>();
    REQUIRE(d->open(g.path.c_str(), nullptr));
    return d;
}

TEST_CASE("search: forward / backward / not found") {
    TempFileGuard g;
    auto d = open_bytes({'a','b','c','a','b','c','x'}, nullptr, g);
    CHECK(find(*d, L"abc", 0) == 0);
    CHECK(find(*d, L"abc", 1) == 3);
    CHECK(find(*d, L"abc", 4) == std::nullopt);
    CHECK(find(*d, L"abc", 7, true, false) == 3);     // backward from end
    CHECK(find(*d, L"abc", 3, true, false) == 0);     // backward excludes from
    CHECK(find(*d, L"zzz", 0) == std::nullopt);
    CHECK(find(*d, L"", 0) == std::nullopt);
}

TEST_CASE("search: case-insensitive ASCII fold") {
    TempFileGuard g;
    auto d = open_bytes({'H','e','L','L','o'}, nullptr, g);
    CHECK(find(*d, L"hello", 0, false) == 0);
    CHECK(find(*d, L"hello", 0, true) == std::nullopt);
}

TEST_CASE("search: match straddles chunk boundary") {
    std::vector<uint8_t> b(300, 'x');
    b[97] = 'N'; b[98] = 'E'; b[99] = 'E'; b[100] = 'D';   // straddles 100-byte chunks
    TempFileGuard g;
    auto d = open_bytes(b, nullptr, g);
    CHECK(find(*d, L"NEED", 0, true, true, 100) == 97);
}

TEST_CASE("search: UTF-8 multibyte needle") {
    // "xЯy" in UTF-8
    TempFileGuard g;
    auto d = open_bytes({'x', 0xD0, 0xAF, 'y'}, nullptr, g);
    CHECK(find(*d, L"\x042F", 0) == 1);
}

TEST_CASE("search: UTF-16 alignment - no match at odd offset") {
    // UTF-16LE BOM + "ab": bytes FF FE 61 00 62 00. The byte pattern 00 62
    // exists at odd offset 3 but is not a unit; searching for U+6200 must miss.
    TempFileGuard g;
    auto d = open_bytes({0xFF,0xFE,'a',0,'b',0}, nullptr, g);
    REQUIRE(d->encoding().kind == EncodingKind::Utf16LE);
    CHECK(find(*d, L"\x6200", 0) == std::nullopt);
    CHECK(find(*d, L"ab", 0) == 2);                   // real match, unit-aligned
}

TEST_CASE("search: searches live edited content") {
    TempFileGuard g;
    auto d = open_bytes({'a','b'}, nullptr, g);
    d->insertText(1, L"FIND");
    CHECK(find(*d, L"FIND", 0) == 1);
}

TEST_CASE("search: cancel returns nullopt") {
    TempFileGuard g;
    auto d = open_bytes(std::vector<uint8_t>(1000, 'q'), nullptr, g);
    std::atomic<bool> cancelled{true};
    SearchOptions o;
    CHECK(search_document(*d, L"q", 0, o, nullptr, cancelled) == std::nullopt);
}
