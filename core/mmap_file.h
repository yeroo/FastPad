#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <windows.h>

namespace fastpad {

// Read-only memory-mapped file. The file is never loaded: fixed-size aligned
// view windows are mapped on demand and recycled LRU. Thread-compatible
// (external synchronization required if shared across threads; the Indexer
// uses its own MmapFile instance over the same path).
class MmapFile {
public:
    explicit MmapFile(size_t viewSize = 64ull << 20, size_t maxViews = 8);
    ~MmapFile();
    MmapFile(const MmapFile&) = delete;
    MmapFile& operator=(const MmapFile&) = delete;

    // Opens read-only with FILE_SHARE_READ|FILE_SHARE_WRITE (logs keep growing).
    bool open(const wchar_t* path, std::wstring* error);
    void close();
    bool isOpen() const { return file_ != INVALID_HANDLE_VALUE; }
    uint64_t size() const { return size_; }

    // Copies len bytes at offset into dst, spanning windows. False if out of range.
    bool read(uint64_t offset, void* dst, size_t len);

    // Zero-copy: pointer at offset, *available = bytes to the end of that window
    // (>=1 on success). Pointer valid until the window is evicted.
    const uint8_t* pin(uint64_t offset, size_t* available);

private:
    struct View { uint64_t base = 0; const uint8_t* ptr = nullptr; size_t len = 0; uint64_t stamp = 0; };
    const View* mapWindow(uint64_t base);

    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    uint64_t size_ = 0;
    size_t viewSize_;
    size_t maxViews_;
    uint64_t clock_ = 0;
    std::vector<View> views_;
};

} // namespace fastpad
