#pragma once
#include "core/document.h"
#include <atomic>
#include <functional>
#include <optional>

namespace fastpad {

struct SearchOptions {
    bool caseSensitive = true;
    bool forward = true;
    // Test hook: chunk size for the scan (default 4 MB).
    size_t chunkSize = 4u << 20;
};

// Finds the needle in the live document. Forward: first match with
// offset >= from. Backward: last match with offset < from. Returns the match's
// byte offset, or nullopt (not found / cancelled / empty needle).
// The needle is encoded into the document's encoding and matched on bytes.
// Case-insensitive folds ASCII A-Z only (safe in UTF-8: ASCII bytes never
// occur inside multibyte sequences; UTF-16: folds units 0x41..0x5A).
// UTF-16 documents: only matches at even offsets count (unit alignment).
// progress may be null; cancel checked per chunk.
std::optional<uint64_t> search_document(Document& doc, const std::wstring& needle,
    uint64_t from, const SearchOptions& opts,
    const std::function<void(uint64_t scanned, uint64_t total)>& progress,
    const std::atomic<bool>& cancel);

} // namespace fastpad
