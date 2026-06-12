#pragma once
#include <windows.h>
#include <winioctl.h>
#include <string>
#include <vector>
#include <cstdint>

// Creates a unique temp file with the given bytes; returns its path.
inline std::wstring write_temp_file(const std::vector<uint8_t>& bytes) {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"fpt", 0, path);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    if (!bytes.empty()) WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(h);
    return path;
}

// Creates an NTFS sparse file of `size` bytes with `head` at offset 0 and
// `tail` ending exactly at `size`. Allocates almost no disk.
inline std::wstring write_sparse_file(uint64_t size, const std::vector<uint8_t>& head, const std::vector<uint8_t>& tail) {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"fps", 0, path);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD br = 0;
    DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &br, nullptr);
    DWORD w = 0;
    if (!head.empty()) WriteFile(h, head.data(), (DWORD)head.size(), &w, nullptr);
    LARGE_INTEGER pos; pos.QuadPart = (LONGLONG)(size - tail.size());
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    if (!tail.empty()) WriteFile(h, tail.data(), (DWORD)tail.size(), &w, nullptr);
    LARGE_INTEGER end; end.QuadPart = (LONGLONG)size;
    SetFilePointerEx(h, end, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    CloseHandle(h);
    return path;
}

struct TempFileGuard {
    std::wstring path;
    ~TempFileGuard() { DeleteFileW(path.c_str()); }
};
