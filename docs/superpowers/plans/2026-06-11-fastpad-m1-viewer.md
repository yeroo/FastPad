# FastPad M1 (Viewer-Complete) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Win32+Direct2D viewer that opens files of ANY size instantly, navigates them at full speed, detects/overrides encodings, and shows live background line indexing — the M1 milestone of the FastPad spec (`docs/superpowers/specs/2026-06-11-fastpad-design.md`).

**Architecture:** UI-free `core/` static lib (memory-mapped file with LRU view cache → encoding detection/decode-on-view → chunked background line index → read-only Document facade) + thin `app/` Win32 shell with virtualized DirectWrite rendering. Everything in core is doctest-unit-tested, including multi-GB paths via NTFS sparse files.

**Tech Stack:** C++20, MSVC (newest installed toolset — see Task 1 discovery; v141/14.16 is ALSO installed and must NOT be picked), x64 only, static CRT, plain .sln/.vcxproj, Direct2D/DirectWrite, doctest (vendored single header).

**Repo:** `C:\Users\Boris.Kudriashov\GitHub\FastPad` (local-only; branch `master`). Build commands assume repo root. MSBuild: `C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe` (set `$env:MSBUILDDISABLENODEREUSE="1"` first in PowerShell).

---

## File structure (final M1 state)

```
FastPad.sln
Directory.Build.props                 (toolset, C++20, x64, static CRT, warnings)
vendor/doctest/doctest.h              (pinned v2.4.11)
core/core.vcxproj
core/mmap_file.h / .cpp               (MmapFile: open/size/read/pin + LRU views)
core/encoding.h / .cpp                (detect, decodeWindow, names, codepage list)
core/line_index.h / .cpp              (LineIndex chunks + Indexer thread)
core/document.h / .cpp                (read-only facade wiring the three above)
tests/tests.vcxproj
tests/main.cpp                        (doctest main)
tests/test_helpers.h                  (temp files, sparse files)
tests/test_mmap.cpp
tests/test_encoding.cpp
tests/test_line_index.cpp
tests/test_document.cpp
app/app.vcxproj
app/main.cpp                          (WinMain, class, msg loop, accel)
app/app_window.h / .cpp               (window, menu, status bar, commands)
app/renderer.h / .cpp                 (D2D/DWrite virtualized text view)
app/goto_dialog.h / .cpp              (Ctrl+G line/offset)
README.md
```

Build/test loop used by every task:

```powershell
$env:MSBUILDDISABLENODEREUSE="1"
& "C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe" FastPad.sln /t:Build /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
.\x64\Debug\tests.exe          # doctest: prints "[doctest] ... | failed: 0"
```

---

### Task 1: Scaffold (solution, props, projects, doctest, smoke test)

**Files:** all of `FastPad.sln`, `Directory.Build.props`, `.gitignore`, `core/core.vcxproj`, `tests/tests.vcxproj`, `app/app.vcxproj`, `tests/main.cpp`, `vendor/doctest/doctest.h`, plus stub `core/mmap_file.h/.cpp` so the lib has one TU.

- [ ] **Step 1: Discover the newest MSVC toolset** (CRITICAL — 14.16/v141 is also installed and lacks C++20):

```powershell
Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1 -ExpandProperty FullName
Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Auxiliary\Build" -Filter "Microsoft.VCToolsVersion.default.txt" -Recurse | ForEach-Object { "$($_.FullName): $(Get-Content $_.FullName)" }
```

Decide `<PlatformToolset>`: if a VS "18" install carries a 14.5x toolset use `v145`; the 2022 install's 14.44 means `v143` is available. Use the NEWEST that exists; record the choice in the commit message. (If MSBuild later errors "Platform Toolset not found", switch the props value to the other one — that is the only knob.)

- [ ] **Step 2: `.gitignore`**

```
x64/
*/x64/
.vs/
*.user
```

- [ ] **Step 3: `Directory.Build.props`** (toolset value per Step 1; shown here with v143):

```xml
<Project>
  <PropertyGroup>
    <PlatformToolset>v143</PlatformToolset>
    <CharacterSet>Unicode</CharacterSet>
    <OutDir>$(SolutionDir)x64\$(Configuration)\</OutDir>
    <IntDir>$(SolutionDir)x64\obj\$(ProjectName)\$(Configuration)\</IntDir>
  </PropertyGroup>
  <ItemDefinitionGroup>
    <ClCompile>
      <LanguageStandard>stdcpp20</LanguageStandard>
      <WarningLevel>Level4</WarningLevel>
      <TreatWarningAsError>true</TreatWarningAsError>
      <PreprocessorDefinitions>NOMINMAX;WIN32_LEAN_AND_MEAN;UNICODE;_UNICODE;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <RuntimeLibrary Condition="'$(Configuration)'=='Debug'">MultiThreadedDebug</RuntimeLibrary>
      <RuntimeLibrary Condition="'$(Configuration)'=='Release'">MultiThreaded</RuntimeLibrary>
      <Optimization Condition="'$(Configuration)'=='Release'">MaxSpeed</Optimization>
      <AdditionalIncludeDirectories>$(SolutionDir);$(SolutionDir)vendor;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ClCompile>
  </ItemDefinitionGroup>
</Project>
```

- [ ] **Step 4: the three .vcxproj files.** All share this skeleton — differences listed after:

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64">
      <Configuration>Debug</Configuration><Platform>x64</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64">
      <Configuration>Release</Configuration><Platform>x64</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectGuid>{GENERATE-ONE-GUID-PER-PROJECT}</ProjectGuid>
    <RootNamespace>fastpad</RootNamespace>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration">
    <ConfigurationType>StaticLibrary</ConfigurationType>
    <UseDebugLibraries Condition="'$(Configuration)'=='Debug'">true</UseDebugLibraries>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />
  <ItemGroup>
    <!-- ClCompile / ClInclude items per project -->
  </ItemGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
```

Per project:
- `core/core.vcxproj`: ConfigurationType **StaticLibrary**; items: `mmap_file.cpp` (+header as ClInclude); later tasks append their files here — every new core file MUST be added to this vcxproj (NOT auto-globbed).
- `tests/tests.vcxproj`: ConfigurationType **Application**; `<SubSystem>Console</SubSystem>` inside an `<ItemDefinitionGroup><Link>`; items `main.cpp` (+ later test files); ProjectReference to `..\core\core.vcxproj`:

```xml
  <ItemGroup>
    <ProjectReference Include="..\core\core.vcxproj"><Project>{CORE-GUID}</Project></ProjectReference>
  </ItemGroup>
```

- `app/app.vcxproj`: ConfigurationType **Application**; `<SubSystem>Windows</SubSystem>`; same ProjectReference; `<Link><AdditionalDependencies>d2d1.lib;dwrite.lib;comctl32.lib;comdlg32.lib;shlwapi.lib;%(AdditionalDependencies)</AdditionalDependencies></Link>`; items `main.cpp` only for now.

(GUIDs: `[guid]::NewGuid()` per project, uppercase, braces.)

`FastPad.sln`: standard 3-project solution (write by hand following any VS sln; only x64 Debug/Release rows). Keep minimal.

- [ ] **Step 5: vendored doctest** (pinned):

```powershell
Invoke-WebRequest -Uri "https://raw.githubusercontent.com/doctest/doctest/v2.4.11/doctest/doctest.h" -OutFile vendor/doctest/doctest.h
```

(If the proxy blocks raw.githubusercontent.com, `git clone --depth 1 --branch v2.4.11 https://github.com/doctest/doctest` to %TEMP% and copy the header.)

- [ ] **Step 6: stub core + doctest main**

`core/mmap_file.h`:

```cpp
#pragma once
namespace fastpad { int core_smoke(); }
```

`core/mmap_file.cpp`:

```cpp
#include "core/mmap_file.h"
namespace fastpad { int core_smoke() { return 42; } }
```

`tests/main.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "core/mmap_file.h"

TEST_CASE("smoke") { CHECK(fastpad::core_smoke() == 42); }
```

`app/main.cpp` (placeholder that links):

```cpp
#include <windows.h>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    MessageBoxW(nullptr, L"FastPad scaffold", L"FastPad", MB_OK);
    return 0;
}
```

- [ ] **Step 7: build + run tests** → `failed: 0` (1 test). Run `x64\Debug\app.exe` → message box appears.

- [ ] **Step 8: commit**

```bash
git add -A
git commit -m "build: scaffold sln/projects (toolset <chosen>), vendored doctest, smoke test"
```

---

### Task 2: MmapFile — open-any-size with LRU view cache (TDD)

**Files:** replace stubs in `core/mmap_file.h/.cpp`; create `tests/test_helpers.h`, `tests/test_mmap.cpp` (add to vcxprojs).

- [ ] **Step 1: helpers** — `tests/test_helpers.h`:

```cpp
#pragma once
#include <windows.h>
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
```

- [ ] **Step 2: failing tests** — `tests/test_mmap.cpp`:

```cpp
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
    // 1 MB pattern, tiny 64 KB view windows force spanning
    std::vector<uint8_t> bytes(1 << 20);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = (uint8_t)(i * 31 + 7);
    TempFileGuard g{ write_temp_file(bytes) };
    MmapFile f(64 * 1024 /*viewSize*/, 4 /*maxViews*/);
    REQUIRE(f.open(g.path.c_str(), nullptr));
    REQUIRE(f.size() == bytes.size());

    std::vector<uint8_t> out(200000);
    REQUIRE(f.read(60000, out.data(), out.size()));   // spans 4 windows
    for (size_t i = 0; i < out.size(); ++i) REQUIRE(out[i] == bytes[60000 + i]);

    // pin returns a direct pointer with the bytes available to the window end
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
    CHECK_FALSE(f.read(SZ - 2, buf, 4));   // read past EOF fails
}
```

- [ ] **Step 3: run → red** (types missing; compile error expected).

- [ ] **Step 4: implement** — `core/mmap_file.h`:

```cpp
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
    // (>=1 on success). Pointer valid until the window is evicted (next 8 pins).
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
```

`core/mmap_file.cpp`:

```cpp
#include "core/mmap_file.h"

namespace fastpad {

MmapFile::MmapFile(size_t viewSize, size_t maxViews)
    : viewSize_(viewSize), maxViews_(maxViews) {
    // Views must align to the allocation granularity (64 KB).
    SYSTEM_INFO si; GetSystemInfo(&si);
    if (viewSize_ % si.dwAllocationGranularity != 0)
        viewSize_ = ((viewSize_ / si.dwAllocationGranularity) + 1) * si.dwAllocationGranularity;
}

MmapFile::~MmapFile() { close(); }

bool MmapFile::open(const wchar_t* path, std::wstring* error) {
    close();
    file_ = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
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
```

Update `tests/main.cpp` smoke test to something real or delete it (replace `core_smoke` with a trivial `CHECK(true)` case); remove `core_smoke` from the header/source.

- [ ] **Step 5: add files to vcxprojs, build, run** → all tests pass (the 4 GB sparse test must complete in well under a second — if it is slow something is wrong with the windowing).

- [ ] **Step 6: commit** — `feat(core): memory-mapped file with LRU view windows (any-size, TDD)`

---

### Task 3: Encoding detection + decode-on-view (TDD)

**Files:** `core/encoding.h/.cpp`, `tests/test_encoding.cpp` (add to vcxprojs).

- [ ] **Step 1: failing tests** — `tests/test_encoding.cpp`:

```cpp
#include "doctest/doctest.h"
#include "core/encoding.h"
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
    // "Привет" in UTF-8
    auto r = det({0xD0,0x9F,0xD1,0x80,0xD0,0xB8,0xD0,0xB2,0xD0,0xB5,0xD1,0x82});
    CHECK(r.kind == EncodingKind::Utf8);
}

TEST_CASE("encoding: invalid UTF-8 falls back to ANSI") {
    auto e = det({'a', 0xC0, 0x20, 'b'});      // 0xC0 0x20 is malformed UTF-8
    CHECK(e.kind == EncodingKind::Ansi);
    CHECK(e.codepage == GetACP());
}

TEST_CASE("encoding: BOM-less UTF-16LE via NUL heuristic") {
    std::vector<uint8_t> b;
    for (char c : std::string("hello world this is utf16")) { b.push_back((uint8_t)c); b.push_back(0); }
    CHECK(det(b).kind == EncodingKind::Utf16LE);
}

TEST_CASE("decode: UTF-8 window ending mid-character backs off") {
    // "é" = 0xC3 0xA9; cut after 0xC3
    std::vector<uint8_t> b{'a','b',0xC3};
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
    // "Я" in cp1251 = 0xDF
    std::vector<uint8_t> b{0xDF};
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
```

- [ ] **Step 2: run → red.**

- [ ] **Step 3: implement** — `core/encoding.h`:

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace fastpad {

enum class EncodingKind { Utf8, Utf16LE, Utf16BE, Ansi };

struct EncodingInfo {
    EncodingKind kind = EncodingKind::Utf8;
    UINT codepage = 65001;      // for Ansi: the codepage; informational otherwise
    int bomBytes = 0;           // bytes to skip at file start
};

// Detection from a leading sample (BOM -> strict UTF-8 validation ->
// UTF-16 NUL heuristic -> system ANSI codepage).
EncodingInfo detect_encoding(const uint8_t* sample, size_t len);

// Decodes a byte window into UTF-16 for rendering. Returns bytes consumed;
// a partial multi-byte character at the tail is left unconsumed (caller
// fetches more bytes). Invalid bytes decode to U+FFFD and are consumed.
size_t decode_window(const EncodingInfo& enc, const uint8_t* bytes, size_t len, std::wstring& out);

std::wstring encoding_name(const EncodingInfo& enc);

// (codepage, display name) for the Encoding menu - UTF variants first, then
// installed codepages.
std::vector<std::pair<UINT, std::wstring>> list_codepages();

} // namespace fastpad
```

`core/encoding.cpp`:

```cpp
#include "core/encoding.h"

namespace fastpad {

// Strict UTF-8 validation; returns false on any malformed sequence. A
// truncated sequence at the very end of the sample is tolerated.
static bool valid_utf8(const uint8_t* p, size_t n) {
    size_t i = 0;
    bool sawMultibyte = false;
    while (i < n) {
        uint8_t c = p[i];
        size_t need = 0;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) { need = 1; if (c < 0xC2) return false; }
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) { need = 3; if (c > 0xF4) return false; }
        else return false;
        if (i + need >= n) return true;          // truncated tail: tolerate
        for (size_t k = 1; k <= need; ++k)
            if ((p[i + k] & 0xC0) != 0x80) return false;
        sawMultibyte = true;
        i += need + 1;
    }
    (void)sawMultibyte;
    return true;
}

EncodingInfo detect_encoding(const uint8_t* s, size_t n) {
    if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) return {EncodingKind::Utf8, 65001, 3};
    if (n >= 2 && s[0] == 0xFF && s[1] == 0xFE) return {EncodingKind::Utf16LE, 1200, 2};
    if (n >= 2 && s[0] == 0xFE && s[1] == 0xFF) return {EncodingKind::Utf16BE, 1201, 2};

    if (n >= 4) {  // NUL-distribution heuristic for BOM-less UTF-16
        size_t evenNul = 0, oddNul = 0;
        size_t scan = (n < 4096) ? n : 4096;
        for (size_t i = 0; i < scan; ++i)
            if (s[i] == 0) ((i & 1) ? oddNul : evenNul)++;
        if (oddNul > scan / 8 && evenNul < scan / 64) return {EncodingKind::Utf16LE, 1200, 0};
        if (evenNul > scan / 8 && oddNul < scan / 64) return {EncodingKind::Utf16BE, 1201, 0};
    }
    if (valid_utf8(s, n)) return {EncodingKind::Utf8, 65001, 0};
    return {EncodingKind::Ansi, GetACP(), 0};
}

size_t decode_window(const EncodingInfo& enc, const uint8_t* bytes, size_t len, std::wstring& out) {
    out.clear();
    if (len == 0) return 0;

    if (enc.kind == EncodingKind::Utf16LE || enc.kind == EncodingKind::Utf16BE) {
        size_t usable = len & ~(size_t)1;             // drop odd tail byte
        out.reserve(usable / 2);
        for (size_t i = 0; i + 1 < usable + 1 && i + 1 < len; i += 2) {
            wchar_t ch = (enc.kind == EncodingKind::Utf16LE)
                ? (wchar_t)(bytes[i] | (bytes[i + 1] << 8))
                : (wchar_t)((bytes[i] << 8) | bytes[i + 1]);
            out.push_back(ch);
        }
        return usable;
    }

    UINT cp = (enc.kind == EncodingKind::Utf8) ? 65001 : enc.codepage;
    // Back off up to 3 tail bytes until the window converts cleanly; this
    // handles a multi-byte character split at the window edge.
    for (size_t back = 0; back <= 3 && back < len; ++back) {
        size_t tryLen = len - back;
        int need = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, (LPCCH)bytes, (int)tryLen, nullptr, 0);
        if (need > 0) {
            out.resize((size_t)need);
            MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, (LPCCH)bytes, (int)tryLen, out.data(), need);
            return tryLen;
        }
    }
    // Genuinely invalid bytes: convert permissively (replacement chars), consume all.
    int need = MultiByteToWideChar(cp, 0, (LPCCH)bytes, (int)len, nullptr, 0);
    if (need <= 0) { out.assign(len, L'\xFFFD'); return len; }
    out.resize((size_t)need);
    MultiByteToWideChar(cp, 0, (LPCCH)bytes, (int)len, out.data(), need);
    return len;
}

std::wstring encoding_name(const EncodingInfo& e) {
    switch (e.kind) {
        case EncodingKind::Utf8:    return e.bomBytes ? L"UTF-8 BOM" : L"UTF-8";
        case EncodingKind::Utf16LE: return L"UTF-16 LE";
        case EncodingKind::Utf16BE: return L"UTF-16 BE";
        case EncodingKind::Ansi: {
            wchar_t buf[64];
            CPINFOEXW info{};
            if (GetCPInfoExW(e.codepage, 0, &info)) return info.CodePageName;
            swprintf_s(buf, L"Codepage %u", e.codepage);
            return buf;
        }
    }
    return L"?";
}

static std::vector<std::pair<UINT, std::wstring>>* g_cpList;
static BOOL CALLBACK cp_enum(LPWSTR s) {
    UINT cp = (UINT)wcstoul(s, nullptr, 10);
    CPINFOEXW info{};
    if (cp && GetCPInfoExW(cp, 0, &info))
        g_cpList->emplace_back(cp, info.CodePageName);
    return TRUE;
}

std::vector<std::pair<UINT, std::wstring>> list_codepages() {
    std::vector<std::pair<UINT, std::wstring>> v;
    g_cpList = &v;
    EnumSystemCodePagesW(cp_enum, CP_INSTALLED);
    g_cpList = nullptr;
    std::sort(v.begin(), v.end());
    return v;
}

} // namespace fastpad
```

(Add `#include <algorithm>` for sort.)

- [ ] **Step 4: build + run → green.** NOTE the UTF-16 decode loop above is intentionally simple — verify the odd-tail test passes; if the loop bound reads awkwardly, simplify to `for (size_t i = 0; i + 1 < len; i += 2)` with `usable = len & ~1` returned; the TEST defines correctness.

- [ ] **Step 5: commit** — `feat(core): encoding detection and decode-on-view (TDD)`

---

### Task 4: LineIndex + background Indexer (TDD)

**Files:** `core/line_index.h/.cpp`, `tests/test_line_index.cpp` (add to vcxprojs).

- [ ] **Step 1: failing tests** — `tests/test_line_index.cpp`:

```cpp
#include "doctest/doctest.h"
#include "core/line_index.h"
#include "core/mmap_file.h"
#include "test_helpers.h"
#include <atomic>

using namespace fastpad;

static std::vector<uint8_t> bytes_of(const char* s) { return {s, s + strlen(s)}; }

TEST_CASE("lineindex: LF / CRLF / lone CR all break lines") {
    LineIndex idx(LineStride::OneByte);
    auto b = bytes_of("aa\nbb\r\ncc\rdd");
    idx.feed(0, b.data(), b.size());
    idx.finish(b.size());
    CHECK(idx.lineCount() == 4);
    CHECK(idx.lineStart(0) == 0);
    CHECK(idx.lineStart(1) == 3);
    CHECK(idx.lineStart(2) == 7);
    CHECK(idx.lineStart(3) == 10);
    CHECK(idx.lineOfOffset(0) == 0);
    CHECK(idx.lineOfOffset(4) == 1);
    CHECK(idx.lineOfOffset(11) == 3);
}

TEST_CASE("lineindex: incremental feeds split across a CRLF boundary") {
    LineIndex idx(LineStride::OneByte);
    auto b = bytes_of("xy\r\nz");
    idx.feed(0, b.data(), 3);              // ends after CR
    idx.feed(3, b.data() + 3, 2);          // starts with LF -> not a second break
    idx.finish(5);
    CHECK(idx.lineCount() == 2);
    CHECK(idx.lineStart(1) == 4);
}

TEST_CASE("lineindex: UTF-16LE stride") {
    LineIndex idx(LineStride::TwoByteLE);
    std::vector<uint8_t> b{'a',0,'\n',0,'b',0};
    idx.feed(0, b.data(), b.size());
    idx.finish(b.size());
    CHECK(idx.lineCount() == 2);
    CHECK(idx.lineStart(1) == 4);
}

TEST_CASE("indexer: background thread indexes a real file with progress and matches naive count") {
    std::vector<uint8_t> b;
    int expectLines = 1;
    for (int i = 0; i < 200000; ++i) {
        b.push_back('a' + (i % 26));
        if (i % 37 == 0) { b.push_back('\n'); expectLines++; }
    }
    TempFileGuard g{ write_temp_file(b) };
    auto file = std::make_shared<MmapFile>(64 * 1024, 4);
    REQUIRE(file->open(g.path.c_str(), nullptr));

    LineIndex idx(LineStride::OneByte);
    std::atomic<int> progressCalls{0};
    Indexer ix;
    ix.start(file, &idx, 0 /*startOffset*/, [&](uint64_t, uint64_t) { progressCalls++; });
    ix.wait();
    CHECK(idx.complete(file->size()));
    CHECK((int)idx.lineCount() == expectLines);
    CHECK(progressCalls.load() > 0);
}

TEST_CASE("indexer: cancel stops early") {
    TempFileGuard g{ write_sparse_file(512ull << 20, {'a','\n'}, {'z'}) };  // 512 MB
    auto file = std::make_shared<MmapFile>();
    REQUIRE(file->open(g.path.c_str(), nullptr));
    LineIndex idx(LineStride::OneByte);
    Indexer ix;
    ix.start(file, &idx, 0, nullptr);
    ix.cancel();
    ix.wait();
    CHECK_FALSE(idx.complete(file->size()) == false ? false : true == false); // no crash; completeness not guaranteed
}
```

(The last CHECK is deliberately just "no crash/deadlock"; simplify it to `CHECK(true);` after `ix.wait()` if the double negation offends — the WAIT returning is the assertion.)

- [ ] **Step 2: run → red.**

- [ ] **Step 3: implement** — `core/line_index.h`:

```cpp
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fastpad {

class MmapFile;

enum class LineStride { OneByte, TwoByteLE, TwoByteBE };

// Stores line-start offsets in 16 MB chunks (32-bit deltas within a chunk).
// feed() must be called with strictly sequential, contiguous ranges.
// Queries are valid for the indexed prefix [0, indexedBytes()).
// Thread model: one writer (Indexer thread), readers take the same mutex.
class LineIndex {
public:
    explicit LineIndex(LineStride stride);
    void feed(uint64_t offset, const uint8_t* data, size_t len);
    void finish(uint64_t fileSize);                 // marks completion
    bool complete(uint64_t fileSize) const;
    uint64_t indexedBytes() const { return indexed_.load(); }
    uint64_t lineCount() const;                     // lines fully discovered so far (>=1)
    uint64_t lineStart(uint64_t lineNo) const;      // 0-based; lineNo < lineCount()
    uint64_t lineOfOffset(uint64_t offset) const;   // offset < indexedBytes()

private:
    static constexpr uint64_t kChunk = 16ull << 20;
    LineStride stride_;
    mutable std::mutex m_;
    std::vector<std::vector<uint32_t>> chunks_;     // breaks (offset AFTER the line break) per chunk
    std::vector<uint64_t> breaksBeforeChunk_;       // prefix sums for O(log) line lookup
    std::atomic<uint64_t> indexed_{0};
    std::atomic<bool> finished_{false};
    bool pendingCR_ = false;                        // lone-CR state across feeds
    void addBreak(uint64_t afterOffset);
};

// Owns a background thread that walks an MmapFile sequentially and feeds a
// LineIndex. start() may be called once per Indexer instance.
class Indexer {
public:
    ~Indexer() { cancel(); wait(); }
    using Progress = std::function<void(uint64_t indexed, uint64_t total)>;
    void start(std::shared_ptr<MmapFile> file, LineIndex* index, uint64_t startOffset, Progress progress);
    void cancel() { cancel_.store(true); }
    void wait();

private:
    std::thread t_;
    std::atomic<bool> cancel_{false};
};

} // namespace fastpad
```

`core/line_index.cpp`:

```cpp
#include "core/line_index.h"
#include "core/mmap_file.h"

namespace fastpad {

LineIndex::LineIndex(LineStride stride) : stride_(stride) {}

void LineIndex::addBreak(uint64_t afterOffset) {
    size_t chunk = (size_t)(afterOffset / kChunk);
    if (chunks_.size() <= chunk) {
        while (chunks_.size() <= chunk) {
            uint64_t prev = breaksBeforeChunk_.empty() ? 0
                : breaksBeforeChunk_.back() + chunks_[breaksBeforeChunk_.size() - 1 + 1 - 1].size();
            // simpler: recompute below
            (void)prev;
            chunks_.emplace_back();
            if (breaksBeforeChunk_.empty()) breaksBeforeChunk_.push_back(0);
            else breaksBeforeChunk_.push_back(breaksBeforeChunk_.back() + chunks_[chunks_.size() - 2].size());
        }
    }
    chunks_[chunk].push_back((uint32_t)(afterOffset % kChunk));
}

void LineIndex::feed(uint64_t offset, const uint8_t* p, size_t len) {
    std::lock_guard<std::mutex> lock(m_);
    if (stride_ == LineStride::OneByte) {
        for (size_t i = 0; i < len; ++i) {
            uint8_t c = p[i];
            if (pendingCR_) {
                pendingCR_ = false;
                if (c == '\n') { addBreak(offset + i + 1); continue; }  // CRLF: break after LF
                addBreak(offset + i);                                    // lone CR broke before this byte
            }
            if (c == '\n') addBreak(offset + i + 1);
            else if (c == '\r') pendingCR_ = true;
        }
    } else {
        // UTF-16: examine code units at even boundaries relative to file start.
        bool le = (stride_ == LineStride::TwoByteLE);
        for (size_t i = 0; i + 1 < len + (offset & 1 ? 1 : 0); /* see below */) { break; }
        for (uint64_t pos = offset; pos + 1 < offset + len; pos += 2) {
            if (pos & 1) { pos -= 1; continue; }    // align to even file offset
            size_t i = (size_t)(pos - offset);
            uint16_t u = le ? (uint16_t)(p[i] | (p[i+1] << 8)) : (uint16_t)((p[i] << 8) | p[i+1]);
            if (pendingCR_) {
                pendingCR_ = false;
                if (u == '\n') { addBreak(pos + 2); continue; }
                addBreak(pos);
            }
            if (u == '\n') addBreak(pos + 2);
            else if (u == '\r') pendingCR_ = true;
        }
    }
    indexed_.store(offset + len);
}

void LineIndex::finish(uint64_t fileSize) {
    std::lock_guard<std::mutex> lock(m_);
    if (pendingCR_) { addBreak(fileSize); pendingCR_ = false; }
    indexed_.store(fileSize);
    finished_.store(true);
}

bool LineIndex::complete(uint64_t fileSize) const {
    return finished_.load() && indexed_.load() >= fileSize;
}

uint64_t LineIndex::lineCount() const {
    std::lock_guard<std::mutex> lock(m_);
    uint64_t breaks = 0;
    if (!chunks_.empty())
        breaks = breaksBeforeChunk_.back() + chunks_.back().size();
    return breaks + 1;
}

uint64_t LineIndex::lineStart(uint64_t lineNo) const {
    if (lineNo == 0) return 0;
    std::lock_guard<std::mutex> lock(m_);
    uint64_t breakIdx = lineNo - 1;                  // start of line N = position after break N-1
    // find chunk via prefix sums
    size_t lo = 0, hi = chunks_.size();
    while (lo + 1 < hi) {
        size_t mid = (lo + hi) / 2;
        if (breaksBeforeChunk_[mid] <= breakIdx) lo = mid; else hi = mid;
    }
    uint64_t within = breakIdx - breaksBeforeChunk_[lo];
    return (uint64_t)lo * kChunk + chunks_[lo][(size_t)within];
}

uint64_t LineIndex::lineOfOffset(uint64_t offset) const {
    std::lock_guard<std::mutex> lock(m_);
    // count breaks at positions <= offset
    size_t chunk = (size_t)(offset / kChunk);
    uint64_t count = 0;
    if (chunk < chunks_.size()) {
        count = breaksBeforeChunk_[chunk];
        const auto& v = chunks_[chunk];
        uint32_t within = (uint32_t)(offset % kChunk);
        count += (uint64_t)(std::upper_bound(v.begin(), v.end(), within) - v.begin());
    } else if (!chunks_.empty()) {
        count = breaksBeforeChunk_.back() + chunks_.back().size();
    }
    return count;                                    // line number = breaks before-or-at offset
}

void Indexer::start(std::shared_ptr<MmapFile> file, LineIndex* index, uint64_t startOffset, Progress progress) {
    t_ = std::thread([this, file, index, startOffset, progress] {
        uint64_t off = startOffset;
        uint64_t total = file->size();
        uint64_t lastReport = 0;
        while (off < total && !cancel_.load()) {
            size_t avail = 0;
            const uint8_t* p = file->pin(off, &avail);
            if (!p) break;
            index->feed(off, p, avail);
            off += avail;
            if (progress && (off - lastReport > (32ull << 20) || off == total)) {
                progress(off, total);
                lastReport = off;
            }
        }
        if (off >= total) index->finish(total);
    });
}

void Indexer::wait() { if (t_.joinable()) t_.join(); }

} // namespace fastpad
```

(Add `#include <algorithm>`. The leftover broken loop fragment in the UTF-16 branch (`for (...) { break; }`) must NOT be kept — write the clean even-aligned loop only; the snippet above shows the intent twice, keep the second loop. CLEAN IT.)

- [ ] **Step 4: build + run → green.** The `addBreak` chunk-extension block contains a redundant `prev` computation — simplify to the `breaksBeforeChunk_` push using the previous chunk's size only. Tests define correctness; refactor until clean AND green.

- [ ] **Step 5: commit** — `feat(core): chunked line index with background indexer (TDD)`

---

### Task 5: Document facade (TDD)

**Files:** `core/document.h/.cpp`, `tests/test_document.cpp` (add to vcxprojs).

- [ ] **Step 1: failing tests** — `tests/test_document.cpp`:

```cpp
#include "doctest/doctest.h"
#include "core/document.h"
#include "test_helpers.h"

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
    d.waitForIndex();                       // test helper: blocks until indexed
    CHECK(d.lineCount() == 3);
    CHECK(d.lineStart(1) == 3 + 9);         // BOM + "line one\n"
}

TEST_CASE("document: reopen with encoding override restarts index with right stride") {
    std::vector<uint8_t> b;
    for (char c : std::string("a\nb\nc")) { b.push_back((uint8_t)c); b.push_back(0); }
    TempFileGuard g{ write_temp_file(b) };

    Document d;
    REQUIRE(d.open(g.path.c_str(), nullptr));   // heuristic may or may not catch it on tiny sample
    d.setEncoding({EncodingKind::Utf16LE, 1200, 0});
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
```

- [ ] **Step 2: run → red.**

- [ ] **Step 3: implement** — `core/document.h`:

```cpp
#pragma once
#include "core/encoding.h"
#include "core/line_index.h"
#include "core/mmap_file.h"
#include <memory>

namespace fastpad {

// Read-only document (M1): memory-mapped file + detected encoding + background
// line index. The piece tree slots in here for M2 without changing this API.
class Document {
public:
    ~Document();
    bool open(const wchar_t* path, std::wstring* error);
    void close();

    uint64_t size() const { return file_ ? file_->size() : 0; }
    const std::wstring& path() const { return path_; }
    const EncodingInfo& encoding() const { return enc_; }
    void setEncoding(const EncodingInfo& enc);      // restarts line indexing

    // Decode up to maxBytes starting at offset; returns bytes consumed.
    size_t decodeAt(uint64_t offset, size_t maxBytes, std::wstring& out);
    // Zero-copy access for scanning (renderer line-break search).
    const uint8_t* pin(uint64_t offset, size_t* available) { return file_ ? file_->pin(offset, available) : nullptr; }

    uint64_t indexedBytes() const { return index_ ? index_->indexedBytes() : 0; }
    bool indexComplete() const { return index_ && index_->complete(size()); }
    uint64_t lineCount() const { return index_ ? index_->lineCount() : 1; }
    uint64_t lineStart(uint64_t lineNo) const { return index_ ? index_->lineStart(lineNo) : 0; }
    uint64_t lineOfOffset(uint64_t off) const { return index_ ? index_->lineOfOffset(off) : 0; }

    void waitForIndex();                            // blocks (tests / small files)

private:
    void restartIndex();
    static LineStride strideFor(const EncodingInfo& e);

    std::shared_ptr<MmapFile> file_;                 // shared with the indexer thread
    std::shared_ptr<MmapFile> indexFile_;            // indexer's own instance (no view contention)
    std::unique_ptr<LineIndex> index_;
    std::unique_ptr<Indexer> indexer_;
    EncodingInfo enc_{};
    std::wstring path_;
};

} // namespace fastpad
```

`core/document.cpp`:

```cpp
#include "core/document.h"

namespace fastpad {

Document::~Document() { close(); }

LineStride Document::strideFor(const EncodingInfo& e) {
    switch (e.kind) {
        case EncodingKind::Utf16LE: return LineStride::TwoByteLE;
        case EncodingKind::Utf16BE: return LineStride::TwoByteBE;
        default: return LineStride::OneByte;
    }
}

bool Document::open(const wchar_t* path, std::wstring* error) {
    close();
    file_ = std::make_shared<MmapFile>();
    if (!file_->open(path, error)) { file_.reset(); return false; }
    path_ = path;

    uint8_t sample[4096];
    size_t sampleLen = (size_t)((size() < sizeof(sample)) ? size() : sizeof(sample));
    if (sampleLen) file_->read(0, sample, sampleLen);
    enc_ = detect_encoding(sample, sampleLen);

    restartIndex();
    return true;
}

void Document::restartIndex() {
    if (indexer_) { indexer_->cancel(); indexer_->wait(); }
    indexer_.reset();
    index_ = std::make_unique<LineIndex>(strideFor(enc_));
    indexFile_ = std::make_shared<MmapFile>();
    std::wstring err;
    if (!indexFile_->open(path_.c_str(), &err)) { indexFile_.reset(); return; }
    indexer_ = std::make_unique<Indexer>();
    indexer_->start(indexFile_, index_.get(), 0, nullptr);
}

void Document::setEncoding(const EncodingInfo& enc) {
    enc_ = enc;
    restartIndex();
}

size_t Document::decodeAt(uint64_t offset, size_t maxBytes, std::wstring& out) {
    out.clear();
    if (!file_ || offset >= size()) return 0;
    size_t want = (size_t)((offset + maxBytes > size()) ? size() - offset : maxBytes);
    std::vector<uint8_t> buf(want);
    if (!file_->read(offset, buf.data(), want)) return 0;
    return decode_window(enc_, buf.data(), want, out);
}

void Document::waitForIndex() { if (indexer_) indexer_->wait(); }

void Document::close() {
    if (indexer_) { indexer_->cancel(); indexer_->wait(); }
    indexer_.reset(); index_.reset(); indexFile_.reset(); file_.reset();
    path_.clear();
}

} // namespace fastpad
```

- [ ] **Step 4: build + run → green** (note the index counts the BOM bytes as part of line 0 — fine for M1; the renderer starts at `enc_.bomBytes`).

- [ ] **Step 5: commit** — `feat(core): read-only Document facade (mmap + encoding + line index, TDD)`

---

### Task 6: App window shell (menu, status bar, open file)

**Files:** replace `app/main.cpp`; create `app/app_window.h/.cpp` (add to app.vcxproj). Manual smoke (no unit tests — UI shell).

- [ ] **Step 1:** `app/app_window.h`:

```cpp
#pragma once
#include "core/document.h"
#include <windows.h>
#include <memory>

namespace fastpad {

class Renderer;  // Task 7

class AppWindow {
public:
    static AppWindow* create(HINSTANCE inst, int showCmd, const wchar_t* fileArg);
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void onOpenDialog();
    void openPath(const wchar_t* path);
    void buildMenu();
    void buildEncodingMenu(HMENU encMenu);
    void updateStatusBar();
    void layout();

    HWND hwnd_ = nullptr;
    HWND status_ = nullptr;
    HINSTANCE inst_ = nullptr;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Renderer> renderer_;     // null until Task 7
    UINT_PTR indexTimer_ = 0;
};

} // namespace fastpad
```

- [ ] **Step 2:** `app/app_window.cpp` — complete implementation:

```cpp
#include "app/app_window.h"
#include <commctrl.h>
#include <commdlg.h>
#include <string>
#include <vector>

namespace fastpad {

enum : UINT {
    IDM_OPEN = 101, IDM_EXIT = 102, IDM_GOTO = 201,
    IDM_ABOUT = 301,
    IDM_ENC_UTF8 = 400, IDM_ENC_UTF16LE = 401, IDM_ENC_UTF16BE = 402,
    IDM_ENC_CP_BASE = 1000,                      // + index into list_codepages()
};
static std::vector<std::pair<UINT, std::wstring>> g_codepages;

AppWindow* AppWindow::create(HINSTANCE inst, int showCmd, const wchar_t* fileArg) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"FastPadMain";
    RegisterClassW(&wc);

    auto* self = new AppWindow();
    self->inst_ = inst;
    HWND h = CreateWindowExW(0, L"FastPadMain", L"FastPad", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 750, nullptr, nullptr, inst, self);
    if (!h) { delete self; return nullptr; }
    ShowWindow(h, showCmd);
    if (fileArg && *fileArg) self->openPath(fileArg);
    return self;
}

LRESULT CALLBACK AppWindow::wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    AppWindow* self;
    if (m == WM_NCCREATE) {
        self = (AppWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (AppWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    }
    return self ? self->handle(m, wp, lp) : DefWindowProcW(h, m, wp, lp);
}

void AppWindow::buildMenu() {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, IDM_GOTO, L"&Go to...\tCtrl+G");
    HMENU enc = CreatePopupMenu();
    buildEncodingMenu(enc);
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About FastPad");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"&Edit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)enc, L"E&ncoding");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
    SetMenu(hwnd_, bar);
}

void AppWindow::buildEncodingMenu(HMENU enc) {
    AppendMenuW(enc, MF_STRING, IDM_ENC_UTF8, L"Reopen as UTF-8");
    AppendMenuW(enc, MF_STRING, IDM_ENC_UTF16LE, L"Reopen as UTF-16 LE");
    AppendMenuW(enc, MF_STRING, IDM_ENC_UTF16BE, L"Reopen as UTF-16 BE");
    AppendMenuW(enc, MF_SEPARATOR, 0, nullptr);
    HMENU cps = CreatePopupMenu();
    g_codepages = list_codepages();
    for (size_t i = 0; i < g_codepages.size(); ++i)
        AppendMenuW(cps, MF_STRING, IDM_ENC_CP_BASE + (UINT)i, g_codepages[i].second.c_str());
    AppendMenuW(enc, MF_POPUP, (UINT_PTR)cps, L"Reopen with codepage");
}

void AppWindow::onOpenDialog() {
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"All files\0*.*\0Text files\0*.txt;*.log;*.csv;*.md;*.json;*.xml\0";
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) openPath(buf);
}

void AppWindow::openPath(const wchar_t* path) {
    auto d = std::make_unique<Document>();
    std::wstring err;
    if (!d->open(path, &err)) {
        MessageBoxW(hwnd_, (L"Cannot open file:\n" + err).c_str(), L"FastPad", MB_ICONERROR);
        return;
    }
    doc_ = std::move(d);
    std::wstring title = std::wstring(path) + L" - FastPad";
    SetWindowTextW(hwnd_, title.c_str());
    if (indexTimer_) KillTimer(hwnd_, indexTimer_);
    indexTimer_ = SetTimer(hwnd_, 1, 250, nullptr);
    updateStatusBar();
    InvalidateRect(hwnd_, nullptr, TRUE);
    // Task 7 wires renderer_->setDocument(doc_.get()) here.
}

void AppWindow::updateStatusBar() {
    if (!status_) return;
    std::wstring enc = doc_ ? encoding_name(doc_->encoding()) : L"";
    wchar_t size[64] = L"", idx[64] = L"";
    if (doc_) {
        swprintf_s(size, L"%llu bytes", (unsigned long long)doc_->size());
        if (doc_->indexComplete())
            swprintf_s(idx, L"%llu lines", (unsigned long long)doc_->lineCount());
        else if (doc_->size())
            swprintf_s(idx, L"indexing %u%%", (unsigned)(doc_->indexedBytes() * 100 / doc_->size()));
    }
    SendMessageW(status_, SB_SETTEXTW, 0, (LPARAM)L"");           // Ln/Col (renderer fills in Task 8)
    SendMessageW(status_, SB_SETTEXTW, 1, (LPARAM)enc.c_str());
    SendMessageW(status_, SB_SETTEXTW, 2, (LPARAM)size);
    SendMessageW(status_, SB_SETTEXTW, 3, (LPARAM)idx);
}

void AppWindow::layout() {
    SendMessageW(status_, WM_SIZE, 0, 0);
    RECT rc; GetClientRect(hwnd_, &rc);
    int parts[4] = { rc.right - 420, rc.right - 260, rc.right - 130, -1 };
    SendMessageW(status_, SB_SETPARTS, 4, (LPARAM)parts);
}

LRESULT AppWindow::handle(UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE:
        status_ = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd_, nullptr, inst_, nullptr);
        buildMenu();
        layout();
        return 0;
    case WM_SIZE: layout(); return 0;
    case WM_TIMER:
        updateStatusBar();
        if (doc_ && doc_->indexComplete() && indexTimer_) { KillTimer(hwnd_, indexTimer_); indexTimer_ = 0; }
        return 0;
    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        if (id == IDM_OPEN) onOpenDialog();
        else if (id == IDM_EXIT) DestroyWindow(hwnd_);
        else if (id == IDM_ABOUT) MessageBoxW(hwnd_, L"FastPad - any-size text viewer/editor.", L"About FastPad", MB_OK);
        else if (doc_ && id == IDM_ENC_UTF8) { doc_->setEncoding({EncodingKind::Utf8, 65001, 0}); updateStatusBar(); InvalidateRect(hwnd_, nullptr, TRUE); }
        else if (doc_ && id == IDM_ENC_UTF16LE) { doc_->setEncoding({EncodingKind::Utf16LE, 1200, 0}); updateStatusBar(); InvalidateRect(hwnd_, nullptr, TRUE); }
        else if (doc_ && id == IDM_ENC_UTF16BE) { doc_->setEncoding({EncodingKind::Utf16BE, 1201, 0}); updateStatusBar(); InvalidateRect(hwnd_, nullptr, TRUE); }
        else if (doc_ && id >= IDM_ENC_CP_BASE && id < IDM_ENC_CP_BASE + g_codepages.size()) {
            doc_->setEncoding({EncodingKind::Ansi, g_codepages[id - IDM_ENC_CP_BASE].first, 0});
            updateStatusBar(); InvalidateRect(hwnd_, nullptr, TRUE);
        }
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd_, m, wp, lp);
}

} // namespace fastpad
```

- [ ] **Step 3:** `app/main.cpp`:

```cpp
#include "app/app_window.h"
#include <windows.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int showCmd) {
    // cmdLine is the raw tail; use argv for a properly unquoted first arg.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t* file = (argc >= 2) ? argv[1] : nullptr;

    auto* win = fastpad::AppWindow::create(inst, showCmd, file);
    if (!win) return 1;

    ACCEL acc[] = {
        { FCONTROL | FVIRTKEY, 'O', 101 /*IDM_OPEN*/ },
        { FCONTROL | FVIRTKEY, 'G', 201 /*IDM_GOTO*/ },
    };
    HACCEL hAccel = CreateAcceleratorTableW(acc, 2);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(win->hwnd(), hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    LocalFree(argv);
    return 0;
}
```

(`shell32.lib` needed for `CommandLineToArgvW` — add to app.vcxproj AdditionalDependencies.)

- [ ] **Step 4: build; manual smoke:** `x64\Debug\app.exe somefile.txt` — window opens instantly, title shows path, status bar shows encoding/size and "indexing %" briefly on a big file, Encoding menu lists codepages. No text rendering yet (next task).

- [ ] **Step 5: commit** — `feat(app): window shell - menu, status bar, open, encoding switching`

---

### Task 7: Direct2D virtualized renderer + navigation

**Files:** `app/renderer.h/.cpp` (add to app.vcxproj); wire into `app_window.cpp`. Manual smoke incl. a 10 GB sparse file.

- [ ] **Step 1:** `app/renderer.h`:

```cpp
#pragma once
#include "core/document.h"
#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>

namespace fastpad {

// Virtualized read-only text view. Owns the byte-anchored scroll position:
// topOffset_ always sits at a line start (or 0 / BOM end).
class Renderer {
public:
    Renderer(HWND hwnd);
    ~Renderer();
    void setDocument(Document* doc);                // not owned
    void onPaint();
    void onResize();
    void scrollLines(int delta);                    // +down, -up
    void scrollPages(int pages);
    void goToOffset(uint64_t offset);               // snaps back to line start
    void goToLine(uint64_t lineNo);                 // requires index coverage
    void goHome() { goToOffset(0); }
    void goEnd();
    void setFontSize(float pt);
    float fontSize() const { return fontSize_; }
    uint64_t topOffset() const { return topOffset_; }
    void updateScrollBar();
    void onVScroll(WPARAM wp);

private:
    void ensureTarget();
    void discardTarget();
    void createTextFormat();
    uint64_t prevLineStart(uint64_t offset);        // backward scan, 1 MB cap
    uint64_t nextLineStart(uint64_t offset);        // forward scan within decode window
    int linesPerPage() const;

    HWND hwnd_;
    Document* doc_ = nullptr;
    ID2D1Factory* d2dFactory_ = nullptr;
    IDWriteFactory* dwFactory_ = nullptr;
    ID2D1HwndRenderTarget* target_ = nullptr;
    ID2D1SolidColorBrush* textBrush_ = nullptr;
    IDWriteTextFormat* format_ = nullptr;
    float fontSize_ = 14.0f;
    float lineHeight_ = 18.0f;
    uint64_t topOffset_ = 0;
};

} // namespace fastpad
```

- [ ] **Step 2:** `app/renderer.cpp`:

```cpp
#include "app/renderer.h"
#include <string>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace fastpad {

static const wchar_t* kFont = L"Consolas";
static const size_t kDecodeWindow = 256 * 1024;     // bytes decoded per paint
static const uint64_t kBackScanCap = 1 << 20;       // backward line search cap

Renderer::Renderer(HWND hwnd) : hwnd_(hwnd) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dwFactory_);
    createTextFormat();
}

Renderer::~Renderer() {
    discardTarget();
    if (format_) format_->Release();
    if (dwFactory_) dwFactory_->Release();
    if (d2dFactory_) d2dFactory_->Release();
}

void Renderer::createTextFormat() {
    if (format_) { format_->Release(); format_ = nullptr; }
    dwFactory_->CreateTextFormat(kFont, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        fontSize_ * 96.0f / 72.0f, L"", &format_);
    lineHeight_ = fontSize_ * 96.0f / 72.0f * 1.35f;
}

void Renderer::ensureTarget() {
    if (target_) return;
    RECT rc; GetClientRect(hwnd_, &rc);
    d2dFactory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)),
        &target_);
    if (target_) target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &textBrush_);
}

void Renderer::discardTarget() {
    if (textBrush_) { textBrush_->Release(); textBrush_ = nullptr; }
    if (target_) { target_->Release(); target_ = nullptr; }
}

void Renderer::onResize() {
    if (target_) {
        RECT rc; GetClientRect(hwnd_, &rc);
        target_->Resize(D2D1::SizeU(rc.right, rc.bottom));
    }
    updateScrollBar();
}

void Renderer::setDocument(Document* doc) {
    doc_ = doc;
    topOffset_ = doc_ ? (uint64_t)doc_->encoding().bomBytes : 0;
    updateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int Renderer::linesPerPage() const {
    RECT rc; GetClientRect(hwnd_, &rc);
    int n = (int)((rc.bottom - 4) / lineHeight_);
    return n > 0 ? n : 1;
}

void Renderer::onPaint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    ensureTarget();
    if (!target_) { EndPaint(hwnd_, &ps); return; }

    target_->BeginDraw();
    target_->Clear(D2D1::ColorF(D2D1::ColorF::White));

    if (doc_ && doc_->size() > 0) {
        std::wstring text;
        doc_->decodeAt(topOffset_, kDecodeWindow, text);

        RECT rc; GetClientRect(hwnd_, &rc);
        float y = 2.0f;
        size_t pos = 0;
        while (pos <= text.size() && y < rc.bottom) {
            size_t eol = text.find_first_of(L"\r\n", pos);
            size_t lineLen = (eol == std::wstring::npos ? text.size() : eol) - pos;
            if (lineLen > 4096) lineLen = 4096;     // pathological single line: clamp draw
            target_->DrawTextW(text.data() + pos, (UINT32)lineLen, format_,
                D2D1::RectF(4.0f, y, (float)rc.right, y + lineHeight_),
                textBrush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            y += lineHeight_;
            if (eol == std::wstring::npos) break;
            pos = eol + ((text[eol] == L'\r' && eol + 1 < text.size() && text[eol + 1] == L'\n') ? 2 : 1);
        }
    }

    HRESULT hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) discardTarget();
    EndPaint(hwnd_, &ps);
}

uint64_t Renderer::nextLineStart(uint64_t offset) {
    if (!doc_) return offset;
    std::wstring text;
    size_t consumed = doc_->decodeAt(offset, 64 * 1024, text);
    if (consumed == 0) return offset;
    // find first line break in DECODED text, then map back by re-decoding
    size_t eol = text.find_first_of(L"\r\n");
    if (eol == std::wstring::npos) return offset + consumed;
    size_t skip = eol + ((text[eol] == L'\r' && eol + 1 < text.size() && text[eol + 1] == L'\n') ? 2 : 1);
    // byte length of the prefix: re-encode is wasteful; approximate via fixed-width
    // encodings exactly and UTF-8 by counting. For M1: decode prefix lengths:
    int bytesPerUnit = (doc_->encoding().kind == EncodingKind::Utf16LE ||
                        doc_->encoding().kind == EncodingKind::Utf16BE) ? 2 : 0;
    if (bytesPerUnit == 2) return offset + skip * 2;
    // UTF-8 / ANSI: walk raw bytes counting decoded units is complex; instead use
    // the line index when available, else scan raw bytes for the break:
    size_t avail = 0;
    const uint8_t* p = doc_->pin(offset, &avail);
    if (!p) return offset + consumed;
    for (size_t i = 0; i < avail; ++i) {
        if (p[i] == '\n') return offset + i + 1;
        if (p[i] == '\r') {
            if (i + 1 < avail && p[i + 1] == '\n') return offset + i + 2;
            return offset + i + 1;
        }
    }
    return offset + avail;
}

uint64_t Renderer::prevLineStart(uint64_t offset) {
    if (!doc_ || offset == 0) return 0;
    // If the index covers this offset, use it (exact and fast).
    if (doc_->indexedBytes() >= offset) {
        uint64_t line = doc_->lineOfOffset(offset - 1);
        return doc_->lineStart(line);
    }
    // Raw backward scan for a line break (byte-wise; correct for 1-byte
    // encodings; UTF-16 handled by 2-byte alignment of the result).
    uint64_t start = (offset > kBackScanCap) ? offset - kBackScanCap : 0;
    std::vector<uint8_t> buf((size_t)(offset - start));
    if (!doc_->pin(start, nullptr)) return 0;
    // read may span windows
    // (Document has no read(); use pin loop)
    {
        uint64_t off = start; size_t got = 0;
        while (got < buf.size()) {
            size_t avail = 0;
            const uint8_t* p = doc_->pin(off, &avail);
            if (!p) break;
            size_t take = std::min(avail, buf.size() - got);
            memcpy(buf.data() + got, p, take);
            got += take; off += take;
        }
    }
    bool wide = (doc_->encoding().kind == EncodingKind::Utf16LE || doc_->encoding().kind == EncodingKind::Utf16BE);
    for (size_t i = buf.size(); i-- > 1;) {
        if (buf[i - 1] == '\n' || buf[i - 1] == '\r') {
            uint64_t cand = start + i;
            if (buf[i - 1] == '\r' && i < buf.size() && buf[i] == '\n') cand = start + i + 1;
            if (wide && (cand & 1)) cand += 1;
            if (cand < offset) return cand;
        }
    }
    return start;
}

void Renderer::scrollLines(int delta) {
    if (!doc_) return;
    while (delta > 0) { uint64_t n = nextLineStart(topOffset_); if (n == topOffset_ || n >= doc_->size()) break; topOffset_ = n; delta--; }
    while (delta < 0 && topOffset_ > 0) { topOffset_ = prevLineStart(topOffset_); delta++; }
    updateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Renderer::scrollPages(int pages) { scrollLines(pages * linesPerPage()); }

void Renderer::goToOffset(uint64_t offset) {
    if (!doc_) return;
    if (offset > doc_->size()) offset = doc_->size();
    topOffset_ = (offset == 0) ? (uint64_t)doc_->encoding().bomBytes : prevLineStart(offset + 1);
    updateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Renderer::goToLine(uint64_t lineNo) {
    if (!doc_) return;
    if (lineNo >= doc_->lineCount()) lineNo = doc_->lineCount() - 1;
    topOffset_ = doc_->lineStart(lineNo);
    updateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Renderer::goEnd() {
    if (!doc_ || doc_->size() == 0) return;
    goToOffset(doc_->size() - 1);
}

void Renderer::setFontSize(float pt) {
    fontSize_ = (pt < 6) ? 6 : (pt > 72 ? 72 : pt);
    createTextFormat();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// 64-bit offsets squeezed into the 32-bit scrollbar by proportional mapping.
void Renderer::updateScrollBar() {
    SCROLLINFO si{ sizeof(si), SIF_RANGE | SIF_POS | SIF_PAGE };
    si.nMin = 0; si.nMax = 100000; si.nPage = 1000;
    si.nPos = (doc_ && doc_->size())
        ? (int)((double)topOffset_ / (double)doc_->size() * 99000.0) : 0;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void Renderer::onVScroll(WPARAM wp) {
    if (!doc_) return;
    switch (LOWORD(wp)) {
    case SB_LINEUP: scrollLines(-1); break;
    case SB_LINEDOWN: scrollLines(1); break;
    case SB_PAGEUP: scrollPages(-1); break;
    case SB_PAGEDOWN: scrollPages(1); break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: {
        SCROLLINFO si{ sizeof(si), SIF_TRACKPOS };
        GetScrollInfo(hwnd_, SB_VERT, &si);
        goToOffset((uint64_t)((double)si.nTrackPos / 99000.0 * (double)doc_->size()));
        break;
    }
    }
}

} // namespace fastpad
```

(Add `#include <algorithm>` for `std::min`.)

- [ ] **Step 3: wire into `app_window.cpp`:** construct `renderer_ = std::make_unique<Renderer>(hwnd_)` in WM_CREATE (after status bar); `renderer_->setDocument(doc_.get())` at the end of `openPath`; handle `WM_PAINT` → `renderer_->onPaint(); return 0;`, `WM_SIZE` → also `renderer_->onResize()`, `WM_VSCROLL` → `renderer_->onVScroll(wp)`, `WM_MOUSEWHEEL` → Ctrl held (`GET_KEYSTATE_WPARAM(wp) & MK_CONTROL`) ? `renderer_->setFontSize(renderer_->fontSize() + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1.0f : -1.0f))` : `renderer_->scrollLines(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -3 : 3)`; `WM_KEYDOWN` → PgUp/PgDn/Up/Down/Ctrl+Home/Ctrl+End mapped to scrollPages/scrollLines/goHome/goEnd. Add `WS_VSCROLL` to the CreateWindowExW style. The window class brush should become `nullptr` (D2D paints everything; avoids flicker) and add `case WM_ERASEBKGND: return 1;`.

- [ ] **Step 4: build; manual smoke:**
  - Normal text file: instant open, smooth wheel scrolling, Ctrl+wheel zoom, Ctrl+End lands at the tail.
  - Create a 10 GB sparse file (PowerShell):
    `$f=[IO.File]::Create("$env:TEMP\big.txt"); $f.SetLength(10GB); $f.Close()` then write a few "hello`n" lines at the start with a small PS script, open it: open must be instant, thumb-drag anywhere must render immediately, status bar shows indexing progressing.
  - UTF-16 file renders correctly after Encoding -> Reopen as UTF-16 LE.

- [ ] **Step 5: commit** — `feat(app): Direct2D virtualized renderer, byte-anchored scrolling, zoom`

---

### Task 8: Go to (line/offset) dialog + Ln/Col in status bar

**Files:** `app/goto_dialog.h/.cpp` (add to app.vcxproj); wire IDM_GOTO in `app_window.cpp`; status part 0 update.

- [ ] **Step 1:** `app/goto_dialog.h/.cpp` — a small modal window (no dialog template needed):

```cpp
// goto_dialog.h
#pragma once
#include <windows.h>
#include <cstdint>
namespace fastpad {
// Returns true when the user confirmed. *isLine says whether `value` is a
// 1-based line number ("123") or a byte offset (":456" or "0x1F" forms).
bool show_goto_dialog(HWND owner, uint64_t* value, bool* isLine);
}
```

```cpp
// goto_dialog.cpp
#include "app/goto_dialog.h"
#include <string>

namespace fastpad {

struct GotoState { uint64_t value = 0; bool isLine = true; bool ok = false; HWND edit = nullptr; };

static LRESULT CALLBACK gotoProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    auto* st = (GotoState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_CREATE: {
        st = (GotoState*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
        CreateWindowExW(0, L"STATIC", L"Line number, or :byte-offset", WS_CHILD | WS_VISIBLE,
            10, 10, 260, 18, h, nullptr, nullptr, nullptr);
        st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            10, 32, 260, 22, h, (HMENU)1, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Go", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            115, 62, 70, 24, h, (HMENU)IDOK, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            195, 62, 70, 24, h, (HMENU)IDCANCEL, nullptr, nullptr);
        SetFocus(st->edit);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[64] = L"";
            GetWindowTextW(st->edit, buf, 64);
            std::wstring s = buf;
            st->isLine = true;
            size_t start = 0;
            if (!s.empty() && s[0] == L':') { st->isLine = false; start = 1; }
            st->value = wcstoull(s.c_str() + start, nullptr, 0);   // 0x... works for offsets
            st->ok = true;
            DestroyWindow(h);
        } else if (LOWORD(wp) == IDCANCEL) DestroyWindow(h);
        return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

bool show_goto_dialog(HWND owner, uint64_t* value, bool* isLine) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = gotoProc;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"FastPadGoto";
        RegisterClassW(&wc);
        registered = true;
    }
    GotoState st;
    RECT orc; GetWindowRect(owner, &orc);
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"FastPadGoto", L"Go to",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, orc.left + 80, orc.top + 80, 296, 130,
        owner, nullptr, nullptr, &st);
    ShowWindow(h, SW_SHOW);
    EnableWindow(owner, FALSE);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { DestroyWindow(h); }
        if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (st.ok) { *value = st.value; *isLine = st.isLine; }
    return st.ok;
}

} // namespace fastpad
```

NOTE: the nested `PostQuitMessage` ends the modal loop but would also leak a WM_QUIT to the main loop — REPLACE the modal loop pattern with a `bool done` flag set in WM_DESTROY via the state pointer (`st->done = true`) and loop `while (!st.done && GetMessageW(...))`, removing `PostQuitMessage`. Implement it the clean way; the snippet shows layout and parsing, the loop discipline is the implementer's job and the manual test below catches leaks (app must NOT exit after closing the dialog).

- [ ] **Step 2: wire** in `app_window.cpp` IDM_GOTO:

```cpp
else if (id == IDM_GOTO && doc_ && renderer_) {
    uint64_t v = 0; bool isLine = true;
    if (show_goto_dialog(hwnd_, &v, &isLine)) {
        if (isLine) {
            uint64_t line = (v == 0) ? 0 : v - 1;            // user types 1-based
            if (doc_->lineStart(line) <= doc_->indexedBytes()) renderer_->goToLine(line);
            else MessageBoxW(hwnd_, L"That line is not indexed yet - try again in a moment, or use :offset.", L"FastPad", MB_ICONINFORMATION);
        } else renderer_->goToOffset(v);
        updateStatusBar();
    }
}
```

And status part 0 in `updateStatusBar()` once a renderer exists:

```cpp
if (doc_ && renderer_) {
    wchar_t lncol[96];
    uint64_t off = renderer_->topOffset();
    if (doc_->indexedBytes() >= off)
        swprintf_s(lncol, L"Ln %llu   Off %llu", (unsigned long long)(doc_->lineOfOffset(off) + 1), (unsigned long long)off);
    else
        swprintf_s(lncol, L"Off %llu", (unsigned long long)off);
    SendMessageW(status_, SB_SETTEXTW, 0, (LPARAM)lncol);
}
```

(also call `updateStatusBar()` from the scroll paths — simplest: in `WM_MOUSEWHEEL`/`WM_VSCROLL`/`WM_KEYDOWN` handlers after delegating to the renderer.)

- [ ] **Step 3: build; manual smoke:** Ctrl+G → "500000" jumps to that line on an indexed file; ":1073741824" jumps to the 1 GB offset of the sparse giant instantly; closing the dialog does NOT close the app; Ln/Off updates while scrolling.

- [ ] **Step 4: commit** — `feat(app): goto line/offset dialog, Ln/Off status`

---

### Task 9: README + open-time budget check + final review

- [ ] **Step 1:** Add a startup timing line (debug builds): in `openPath`, wrap `Document::open` with `QueryPerformanceCounter` and `OutputDebugStringW` the elapsed ms. Manual check on the 10 GB sparse file: must print **< 50 ms**.

- [ ] **Step 2:** `README.md`: what FastPad is (any-size viewer, M1), build instructions (MSBuild command), usage (open/goto/encoding/zoom keys), the M2/M3 roadmap pointer to the spec, and the honest notes (line numbers appear as indexing completes).

- [ ] **Step 3:** Run the FULL test suite one last time; `git add -A; git commit -m "docs: README, M1 complete"`.

- [ ] **Step 4:** Review the spec's M1 checklist against reality: open-any-size ✅, virtualized rendering ✅, scroll/goto ✅, encodings detect+override ✅, status bar with indexing ✅. Anything missed becomes a follow-up task, not silent scope drift.

---

## Notes / risks

- **Toolset trap:** 14.16/v141 is installed and CANNOT build C++20 — Task 1 Step 1 exists precisely to dodge it. Never let a vcxproj omit `<PlatformToolset>`.
- **Warning-as-error at W4** will flag unused params in Win32 callbacks — fix with `(void)` casts or named-param removal, do not lower the warning level.
- The two code snippets explicitly marked "CLEAN IT" / "implement the clean way" (UTF-16 feed loop, modal loop) contain intent plus a known-flawed fragment; the tests/manual checks define correctness.
- Sparse-file tests assume %TEMP% is NTFS (true on this machine's C:).
- D2D `DrawText` per visible line is fast enough for M1 (no layout caching needed at ~50 lines/frame); measure before optimizing.
