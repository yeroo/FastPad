#include "core/mmap_file.h"

namespace fastpad {

MmapFile::MmapFile(size_t viewSize, size_t maxViews)
    : viewSize_(viewSize), maxViews_(maxViews) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    if (viewSize_ % si.dwAllocationGranularity != 0)
        viewSize_ = ((viewSize_ / si.dwAllocationGranularity) + 1) * si.dwAllocationGranularity;
}

MmapFile::~MmapFile() { close(); }

bool MmapFile::open(const wchar_t* path, std::wstring* error) {
    close();
    // FILE_SHARE_DELETE: in-place save replaces the file via ReplaceFileW
    // (a rename) while this mapping is still open; the rename needs DELETE
    // sharing. Mapped views stay valid across the rename.
    file_ = CreateFileW(path, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        if (error) {
            wchar_t buf[256];
            FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, GetLastError(), 0, buf, 256, nullptr);
            *error = buf;
        }
        return false;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(file_, &sz);
    size_ = (uint64_t)sz.QuadPart;
    if (size_ > 0) {
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_) {
            if (error) *error = L"CreateFileMapping failed";
            close();
            return false;
        }
    }
    return true;
}

void MmapFile::close() {
    for (auto& v : views_)
        if (v.ptr) UnmapViewOfFile(v.ptr);
    views_.clear();
    if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
    if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
    size_ = 0;
}

const MmapFile::View* MmapFile::mapWindow(uint64_t base) {
    for (auto& v : views_)
        if (v.ptr && v.base == base) { v.stamp = ++clock_; return &v; }

    size_t len = (size_t)((base + viewSize_ <= size_) ? viewSize_ : size_ - base);
    const uint8_t* p = (const uint8_t*)MapViewOfFile(
        mapping_, FILE_MAP_READ, (DWORD)(base >> 32), (DWORD)(base & 0xFFFFFFFF), len);
    if (!p) return nullptr;

    View* slot = nullptr;
    if (views_.size() < maxViews_) {
        views_.push_back({});
        slot = &views_.back();
    } else {
        slot = &views_[0];
        for (auto& v : views_)
            if (v.stamp < slot->stamp) slot = &v;
        UnmapViewOfFile(slot->ptr);
    }
    *slot = View{ base, p, len, ++clock_ };
    return slot;
}

const uint8_t* MmapFile::pin(uint64_t offset, size_t* available) {
    if (!mapping_ || offset >= size_) return nullptr;
    uint64_t base = (offset / viewSize_) * viewSize_;
    const View* v = mapWindow(base);
    if (!v) return nullptr;
    size_t inView = (size_t)(offset - base);
    if (available) *available = v->len - inView;
    return v->ptr + inView;
}

bool MmapFile::read(uint64_t offset, void* dst, size_t len) {
    if (len == 0) return true;
    if (offset + len > size_) return false;
    uint8_t* out = (uint8_t*)dst;
    while (len > 0) {
        size_t avail = 0;
        const uint8_t* p = pin(offset, &avail);
        if (!p) return false;
        size_t take = (len < avail) ? len : avail;
        memcpy(out, p, take);
        out += take; offset += take; len -= take;
    }
    return true;
}

} // namespace fastpad
