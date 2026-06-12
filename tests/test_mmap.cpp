#include "doctest/doctest.h"
#include "core/mmap_file.h"
#include "test_helpers.h"

using fastpad::MmapFile;

TEST_CASE("mmap: open missing file fails with error") {
    MmapFile f;
    std::wstring err;
    CHECK_FALSE(f.open(L"Z:\\no\\such\\file.bin", &err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("mmap: empty file opens, size 0") {
    TempFileGuard g{ write_temp_file({}) };
    MmapFile f;
    CHECK(f.open(g.path.c_str(), nullptr));
    CHECK(f.size() == 0);
}

TEST_CASE("mmap: read spans view windows") {
    std::vector<uint8_t> bytes(1 << 20);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = (uint8_t)(i * 31 + 7);
    TempFileGuard g{ write_temp_file(bytes) };
    MmapFile f(64 * 1024 /*viewSize*/, 4 /*maxViews*/);
    REQUIRE(f.open(g.path.c_str(), nullptr));
    REQUIRE(f.size() == bytes.size());

    std::vector<uint8_t> out(200000);
    REQUIRE(f.read(60000, out.data(), out.size()));
    for (size_t i = 0; i < out.size(); ++i) REQUIRE(out[i] == bytes[60000 + i]);

    size_t avail = 0;
    const uint8_t* p = f.pin(70000, &avail);
    REQUIRE(p != nullptr);
    CHECK(avail == 64 * 1024 - (70000 % (64 * 1024)));
    CHECK(p[0] == bytes[70000]);
}

TEST_CASE("mmap: 4GB sparse file - both ends readable, open is instant") {
    const uint64_t SZ = 4ull * 1024 * 1024 * 1024;
    std::vector<uint8_t> head{'H','E','A','D'}, tail{'T','A','I','L'};
    TempFileGuard g{ write_sparse_file(SZ, head, tail) };
    MmapFile f;
    REQUIRE(f.open(g.path.c_str(), nullptr));
    REQUIRE(f.size() == SZ);
    uint8_t buf[4];
    REQUIRE(f.read(0, buf, 4));
    CHECK(memcmp(buf, "HEAD", 4) == 0);
    REQUIRE(f.read(SZ - 4, buf, 4));
    CHECK(memcmp(buf, "TAIL", 4) == 0);
    CHECK_FALSE(f.read(SZ - 2, buf, 4));
}
