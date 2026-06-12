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

// Decodes a byte window into UTF-16 for rendering. Returns bytes consumed.
// A partial multi-byte character at the tail is left unconsumed when the rest
// of the window is valid; if the window also contains invalid bytes, the
// permissive fallback consumes the whole window (invalid bytes become U+FFFD).
size_t decode_window(const EncodingInfo& enc, const uint8_t* bytes, size_t len, std::wstring& out);

// Encodes UTF-16 text into the document's byte encoding. Unencodable
// characters become the codepage default char ('?').
std::vector<uint8_t> encode_text(const EncodingInfo& enc, const std::wstring& text);

std::wstring encoding_name(const EncodingInfo& enc);

// (codepage, display name) pairs for the Encoding menu.
std::vector<std::pair<UINT, std::wstring>> list_codepages();

} // namespace fastpad
