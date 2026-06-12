#include "doctest/doctest.h"
#include "core/document.h"
#include "core/save.h"
#include "test_helpers.h"
#include <atomic>

using namespace fastpad;

static std::vector<uint8_t> file_bytes(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    LARGE_INTEGER sz{}; GetFileSizeEx(h, &sz);
    std::vector<uint8_t> v((size_t)sz.QuadPart);
    DWORD r = 0;
    if (!v.empty()) ReadFile(h, v.data(), (DWORD)v.size(), &r, nullptr);
    CloseHandle(h);
    return v;
}

TEST_CASE("save: edited document saves in place atomically and reopens clean") {
    TempFileGuard g{ write_temp_file({'a','b','c'}) };
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    d.insertText(3, L"XY");
    std::atomic<bool> cancel{false};
    std::wstring err;
    REQUIRE(save_document(d, {L"", std::nullopt}, nullptr, cancel, &err));
    REQUIRE(d.reopenAfterSave(g.path.c_str(), &err));
    CHECK_FALSE(d.dirty());
    CHECK(file_bytes(g.path) == std::vector<uint8_t>{'a','b','c','X','Y'});
}

TEST_CASE("save: save-as to a new path leaves the original untouched") {
    TempFileGuard g{ write_temp_file({'1','2'}) };
    std::wstring target = g.path + L".out";
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    d.insertText(2, L"3");
    std::atomic<bool> cancel{false};
    std::wstring err;
    REQUIRE(save_document(d, {target, std::nullopt}, nullptr, cancel, &err));
    CHECK(file_bytes(g.path) == std::vector<uint8_t>{'1','2'});
    CHECK(file_bytes(target) == std::vector<uint8_t>{'1','2','3'});
    DeleteFileW(target.c_str());
}

TEST_CASE("save: transcode UTF-8 -> UTF-16LE with BOM") {
    TempFileGuard g{ write_temp_file({'h','i','\n',0xD0,0xAF}) };   // "hi\nЯ" UTF-8
    std::wstring target = g.path + L".u16";
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    std::atomic<bool> cancel{false};
    std::wstring err;
    REQUIRE(save_document(d, {target, EncodingInfo{EncodingKind::Utf16LE, 1200, 2}}, nullptr, cancel, &err));
    auto b = file_bytes(target);
    CHECK(b == std::vector<uint8_t>{0xFF,0xFE,'h',0,'i',0,'\n',0,0x2F,0x04});
    DeleteFileW(target.c_str());
}

TEST_CASE("save: force LF endings") {
    TempFileGuard g{ write_temp_file({'a','\r','\n','b','\r','c','\n','d'}) };
    std::wstring target = g.path + L".lf";
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    std::atomic<bool> cancel{false};
    std::wstring err;
    SaveOptions o; o.targetPath = target; o.forceEol = L"\n";
    REQUIRE(save_document(d, o, nullptr, cancel, &err));
    CHECK(file_bytes(target) == std::vector<uint8_t>{'a','\n','b','\n','c','\n','d'});
    DeleteFileW(target.c_str());
}

TEST_CASE("save: force CRLF endings on UTF-16 document") {
    std::vector<uint8_t> src{0xFF,0xFE,'a',0,'\n',0,'b',0};
    TempFileGuard g{ write_temp_file(src) };
    std::wstring target = g.path + L".crlf";
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    std::atomic<bool> cancel{false};
    std::wstring err;
    SaveOptions o; o.targetPath = target; o.forceEol = L"\r\n";
    REQUIRE(save_document(d, o, nullptr, cancel, &err));
    CHECK(file_bytes(target) == std::vector<uint8_t>{0xFF,0xFE,'a',0,'\r',0,'\n',0,'b',0});
    DeleteFileW(target.c_str());
}

TEST_CASE("save: cancel leaves no temp and target unchanged") {
    TempFileGuard g{ write_temp_file(std::vector<uint8_t>(8 * 1024 * 1024, 'x')) };
    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));
    d.insertText(0, L"y");
    std::atomic<bool> cancel{true};               // pre-cancelled
    std::wstring err;
    CHECK_FALSE(save_document(d, {L"", std::nullopt}, nullptr, cancel, &err));
    CHECK(file_bytes(g.path).size() == 8 * 1024 * 1024);
    CHECK(file_bytes(g.path)[0] == 'x');
}
