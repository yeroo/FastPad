#include "doctest/doctest.h"
#include "core/encoding.h"
#include <string>
#include <vector>

using namespace fastpad;

static EncodingInfo det(std::vector<uint8_t> b) { return detect_encoding(b.data(), b.size()); }

TEST_CASE("encoding: BOMs") {
    CHECK(det({0xEF,0xBB,0xBF,'h','i'}).kind == EncodingKind::Utf8);
    CHECK(det({0xEF,0xBB,0xBF,'h','i'}).bomBytes == 3);
    CHECK(det({0xFF,0xFE,'a',0}).kind == EncodingKind::Utf16LE);
    CHECK(det({0xFE,0xFF,0,'a'}).kind == EncodingKind::Utf16BE);
}

TEST_CASE("encoding: pure ASCII and valid UTF-8 detect as UTF-8, no BOM") {
    auto a = det({'h','e','l','l','o'});
    CHECK(a.kind == EncodingKind::Utf8);
    CHECK(a.bomBytes == 0);
    auto r = det({0xD0,0x9F,0xD1,0x80,0xD0,0xB8,0xD0,0xB2,0xD0,0xB5,0xD1,0x82}); // "Privет" UTF-8
    CHECK(r.kind == EncodingKind::Utf8);
}

TEST_CASE("encoding: invalid UTF-8 falls back to ANSI") {
    auto e = det({'a', 0xC0, 0x20, 'b'});
    CHECK(e.kind == EncodingKind::Ansi);
    CHECK(e.codepage == GetACP());
}

TEST_CASE("encoding: BOM-less UTF-16LE via NUL heuristic") {
    std::vector<uint8_t> b;
    for (char c : std::string("hello world this is utf16")) { b.push_back((uint8_t)c); b.push_back(0); }
    CHECK(det(b).kind == EncodingKind::Utf16LE);
}

TEST_CASE("decode: UTF-8 window ending mid-character backs off") {
    std::vector<uint8_t> b{'a','b',0xC3};            // "e-acute"=C3 A9 cut after C3
    EncodingInfo enc{EncodingKind::Utf8, 65001, 0};
    std::wstring out;
    size_t consumed = decode_window(enc, b.data(), b.size(), out);
    CHECK(consumed == 2);
    CHECK(out == L"ab");
}

TEST_CASE("decode: UTF-16LE odd tail byte left unconsumed") {
    std::vector<uint8_t> b{'a',0,'b',0,'c'};
    EncodingInfo enc{EncodingKind::Utf16LE, 1200, 0};
    std::wstring out;
    size_t consumed = decode_window(enc, b.data(), b.size(), out);
    CHECK(consumed == 4);
    CHECK(out == L"ab");
}

TEST_CASE("decode: ANSI cp1251 window") {
    std::vector<uint8_t> b{0xDF};                    // "Ya" in cp1251
    EncodingInfo enc{EncodingKind::Ansi, 1251, 0};
    std::wstring out;
    size_t consumed = decode_window(enc, b.data(), b.size(), out);
    CHECK(consumed == 1);
    CHECK(out == L"\x042F");
}

TEST_CASE("encoding: name strings exist") {
    CHECK(encoding_name({EncodingKind::Utf8, 65001, 3}) == std::wstring(L"UTF-8 BOM"));
    CHECK(encoding_name({EncodingKind::Utf16LE, 1200, 2}) == std::wstring(L"UTF-16 LE"));
}

TEST_CASE("encode_text: round-trips through each encoding kind") {
    CHECK(encode_text({EncodingKind::Utf8, 65001, 0}, L"hi\x042F") == std::vector<uint8_t>{'h','i',0xD0,0xAF});
    CHECK(encode_text({EncodingKind::Utf16LE, 1200, 0}, L"hi") == std::vector<uint8_t>{'h',0,'i',0});
    CHECK(encode_text({EncodingKind::Utf16BE, 1201, 0}, L"hi") == std::vector<uint8_t>{0,'h',0,'i'});
    CHECK(encode_text({EncodingKind::Ansi, 1251, 0}, L"\x042F") == std::vector<uint8_t>{0xDF});
}

TEST_CASE("encode_text: unencodable char in ANSI becomes '?'") {
    auto b = encode_text({EncodingKind::Ansi, 1252, 0}, L"\x4E0A");   // CJK in cp1252
    CHECK(b == std::vector<uint8_t>{'?'});
}
