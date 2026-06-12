#pragma once
#include "core/document.h"
#include <atomic>
#include <functional>
#include <optional>
#include <string>

namespace fastpad {

struct SaveOptions {
    std::wstring targetPath;                       // empty = document's own path
    std::optional<EncodingInfo> transcodeTo;       // nullopt = keep bytes as-is
    // L"\n" or L"\r\n": normalize every line break to this on save. Forces the
    // decode/encode path even without transcodeTo (effective target encoding =
    // transcodeTo.value_or(source encoding, including its BOM setting)).
    std::optional<std::wstring> forceEol;
};

// Streams the document (4 MB chunks) to "<target>.fptmp" in the target's
// directory, then atomically replaces the target (ReplaceFileW when it exists,
// MoveFileExW otherwise). Returns false + error on failure or cancel; the
// target is never left half-written. Synchronous - run from a worker if the UI
// must stay live (M2 ships it synchronous with a progress callback pumping).
bool save_document(Document& doc, const SaveOptions& opts,
                   const std::function<void(uint64_t done, uint64_t total)>& progress,
                   const std::atomic<bool>& cancel, std::wstring* error);

} // namespace fastpad
