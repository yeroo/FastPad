#include "core/save.h"
#include "core/encoding.h"
#include <algorithm>
#include <windows.h>

namespace fastpad {

namespace {

constexpr size_t kChunk = 4 * 1024 * 1024;
constexpr uint64_t kFreeSlack = 64ull * 1024 * 1024;

bool fail(std::wstring* error, const wchar_t* msg) {
    if (error) *error = msg;
    return false;
}

// Directory containing `path` ("." for bare names; "C:\" kept for roots).
std::wstring dir_of(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    if (pos == 2 && path[1] == L':') return path.substr(0, 3);
    if (pos == 0) return path.substr(0, 1);
    return path.substr(0, pos);
}

std::vector<uint8_t> bom_bytes(const EncodingInfo& enc) {
    if (enc.bomBytes <= 0) return {};
    switch (enc.kind) {
        case EncodingKind::Utf8:    return {0xEF, 0xBB, 0xBF};
        case EncodingKind::Utf16LE: return {0xFF, 0xFE};
        case EncodingKind::Utf16BE: return {0xFE, 0xFF};
        default:                    return {};
    }
}

bool write_all(HANDLE h, const uint8_t* p, size_t len) {
    while (len > 0) {
        DWORD chunk = (DWORD)std::min<size_t>(len, 1u << 30);
        DWORD written = 0;
        if (!WriteFile(h, p, chunk, &written, nullptr) || written == 0) return false;
        p += written;
        len -= written;
    }
    return true;
}

// Normalizes line breaks in `text` to `eol` (\r\n, lone \r, and \n all become
// one break). CHUNK-EDGE RULE: a trailing '\r' is held back via `pendingCR`
// (re-prepended to the next chunk) so a CRLF split across a chunk boundary
// converts exactly once; the caller flushes a still-pending '\r' as one break
// at EOF.
std::wstring normalize_eol(std::wstring text, const std::wstring& eol, bool& pendingCR) {
    if (pendingCR) {
        text.insert(text.begin(), L'\r');
        pendingCR = false;
    }
    if (!text.empty() && text.back() == L'\r') {
        text.pop_back();
        pendingCR = true;
    }
    std::wstring out;
    out.reserve(text.size() + eol.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;   // CRLF -> one break
            out += eol;
        } else if (c == L'\n') {
            out += eol;
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace

bool save_document(Document& doc, const SaveOptions& opts,
                   const std::function<void(uint64_t done, uint64_t total)>& progress,
                   const std::atomic<bool>& cancel, std::wstring* error) {
    const std::wstring target = opts.targetPath.empty() ? doc.path() : opts.targetPath;
    if (target.empty()) return fail(error, L"No file path to save to.");

    // Preflight: the temp copy transiently needs ~doc.size() on the target's
    // volume; require headroom so we never fill the disk to the brim.
    ULARGE_INTEGER freeBytes{};
    if (GetDiskFreeSpaceExW(dir_of(target).c_str(), &freeBytes, nullptr, nullptr) &&
        freeBytes.QuadPart < doc.size() + kFreeSlack) {
        return fail(error, L"Not enough free disk space to save.");
    }

    const std::wstring temp = target + L".fptmp";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail(error, L"Cannot create temporary save file.");

    // Any failure or cancel: close + delete temp; the target is never touched.
    auto abort = [&](const wchar_t* msg) {
        CloseHandle(h);
        DeleteFileW(temp.c_str());
        return fail(error, msg);
    };

    const uint64_t total = doc.size();

    if (!opts.transcodeTo && !opts.forceEol) {
        // Raw byte stream, 4 MB chunks.
        std::vector<uint8_t> buf(kChunk);
        uint64_t off = 0;
        while (off < total) {
            if (cancel.load(std::memory_order_relaxed)) return abort(L"Cancelled");
            size_t take = (size_t)std::min<uint64_t>(kChunk, total - off);
            if (!doc.read(off, buf.data(), take)) return abort(L"Read failed during save.");
            if (!write_all(h, buf.data(), take)) return abort(L"Write failed during save.");
            off += take;
            if (progress) progress(off, total);
        }
    } else {
        // Decode source chunks (carrying any partial multi-byte tail into the
        // next chunk), re-encode into the target encoding, prepend target BOM.
        // Effective target encoding = transcodeTo.value_or(source encoding):
        // a forceEol-only save keeps the source encoding INCLUDING its BOM
        // setting, so a BOM is re-emitted exactly when the source had one.
        if (cancel.load(std::memory_order_relaxed)) return abort(L"Cancelled");
        const EncodingInfo srcEnc = doc.encoding();
        const EncodingInfo dstEnc = opts.transcodeTo.value_or(srcEnc);
        const std::vector<uint8_t> bom = bom_bytes(dstEnc);
        if (!bom.empty() && !write_all(h, bom.data(), bom.size()))
            return abort(L"Write failed during save.");

        uint64_t off = std::min<uint64_t>((uint64_t)srcEnc.bomBytes, total);  // skip source BOM
        std::vector<uint8_t> buf;                    // carry tail + fresh bytes
        std::wstring text;
        bool pendingCR = false;                      // held-back trailing '\r' (chunk-edge carry)
        for (;;) {
            if (cancel.load(std::memory_order_relaxed)) return abort(L"Cancelled");
            size_t want = (kChunk > buf.size()) ? kChunk - buf.size() : 0;
            size_t take = (size_t)std::min<uint64_t>(want, total - off);
            if (take) {
                size_t old = buf.size();
                buf.resize(old + take);
                if (!doc.read(off, buf.data() + old, take)) return abort(L"Read failed during save.");
                off += take;
            }
            if (buf.empty()) break;
            text.clear();
            size_t consumed = decode_window(srcEnc, buf.data(), buf.size(), text);
            if (consumed == 0) {
                if (off < total) return abort(L"Decode made no progress during save.");
                text.assign(1, L'\xFFFD');           // truncated trailing char at EOF
                consumed = buf.size();
            }
            if (opts.forceEol) text = normalize_eol(std::move(text), *opts.forceEol, pendingCR);
            std::vector<uint8_t> encoded = encode_text(dstEnc, text);
            if (!encoded.empty() && !write_all(h, encoded.data(), encoded.size()))
                return abort(L"Write failed during save.");
            buf.erase(buf.begin(), buf.begin() + consumed);
            if (progress) progress(off - buf.size(), total);
        }
        if (pendingCR) {                             // flush a held '\r' as one break at EOF
            std::vector<uint8_t> encoded = encode_text(dstEnc, *opts.forceEol);
            if (!encoded.empty() && !write_all(h, encoded.data(), encoded.size()))
                return abort(L"Write failed during save.");
        }
    }

    if (!FlushFileBuffers(h)) return abort(L"Flush failed during save.");
    CloseHandle(h);

    BOOL ok;
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
        ok = ReplaceFileW(target.c_str(), temp.c_str(), nullptr, 0, nullptr, nullptr);
    } else {
        ok = MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    if (!ok) {
        DeleteFileW(temp.c_str());
        return fail(error, L"Could not replace the target file.");
    }
    return true;
}

} // namespace fastpad
