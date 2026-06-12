#include "core/encoding.h"
#include <algorithm>

namespace fastpad {

// Strict UTF-8 validation; returns false on any malformed sequence. A
// truncated sequence at the very end of the sample is tolerated.
static bool valid_utf8(const uint8_t* p, size_t n) {
    size_t i = 0;
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
        i += need + 1;
    }
    return true;
}

EncodingInfo detect_encoding(const uint8_t* s, size_t n) {
    if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) return {EncodingKind::Utf8, 65001, 3};
    if (n >= 2 && s[0] == 0xFF && s[1] == 0xFE) return {EncodingKind::Utf16LE, 1200, 2};
    if (n >= 2 && s[0] == 0xFE && s[1] == 0xFF) return {EncodingKind::Utf16BE, 1201, 2};

    if (n >= 4) {
        size_t evenNul = 0, oddNul = 0;
        size_t scan = (n < 4096) ? n : 4096;
        for (size_t i = 0; i < scan; ++i)
            if (s[i] == 0) ((i & 1) ? oddNul : evenNul)++;
        if (oddNul > scan / 8 && evenNul <= scan / 64) return {EncodingKind::Utf16LE, 1200, 0};
        if (evenNul > scan / 8 && oddNul <= scan / 64) return {EncodingKind::Utf16BE, 1201, 0};
    }
    if (valid_utf8(s, n)) return {EncodingKind::Utf8, 65001, 0};
    return {EncodingKind::Ansi, GetACP(), 0};
}

size_t decode_window(const EncodingInfo& enc, const uint8_t* bytes, size_t len, std::wstring& out) {
    out.clear();
    if (len == 0) return 0;

    if (enc.kind == EncodingKind::Utf16LE || enc.kind == EncodingKind::Utf16BE) {
        size_t usable = len & ~(size_t)1;
        out.reserve(usable / 2);
        for (size_t i = 0; i + 1 < len; i += 2) {
            wchar_t ch = (enc.kind == EncodingKind::Utf16LE)
                ? (wchar_t)(bytes[i] | (bytes[i + 1] << 8))
                : (wchar_t)((bytes[i] << 8) | bytes[i + 1]);
            out.push_back(ch);
        }
        return usable;
    }

    UINT cp = (enc.kind == EncodingKind::Utf8) ? 65001 : enc.codepage;
    for (size_t back = 0; back <= 3 && back < len; ++back) {
        size_t tryLen = len - back;
        int need = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, (LPCCH)bytes, (int)tryLen, nullptr, 0);
        if (need > 0) {
            out.resize((size_t)need);
            MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, (LPCCH)bytes, (int)tryLen, out.data(), need);
            return tryLen;
        }
    }
    int need = MultiByteToWideChar(cp, 0, (LPCCH)bytes, (int)len, nullptr, 0);
    if (need <= 0) { out.assign(len, L'\xFFFD'); return len; }
    out.resize((size_t)need);
    MultiByteToWideChar(cp, 0, (LPCCH)bytes, (int)len, out.data(), need);
    return len;
}

std::vector<uint8_t> encode_text(const EncodingInfo& enc, const std::wstring& text) {
    std::vector<uint8_t> out;
    if (text.empty()) return out;
    if (enc.kind == EncodingKind::Utf16LE || enc.kind == EncodingKind::Utf16BE) {
        out.reserve(text.size() * 2);
        for (wchar_t ch : text) {
            uint8_t lo = (uint8_t)(ch & 0xFF), hi = (uint8_t)(ch >> 8);
            if (enc.kind == EncodingKind::Utf16LE) { out.push_back(lo); out.push_back(hi); }
            else { out.push_back(hi); out.push_back(lo); }
        }
        return out;
    }
    UINT cp = (enc.kind == EncodingKind::Utf8) ? 65001 : enc.codepage;
    int need = WideCharToMultiByte(cp, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return out;
    out.resize((size_t)need);
    WideCharToMultiByte(cp, 0, text.c_str(), (int)text.size(), (LPSTR)out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring encoding_name(const EncodingInfo& e) {
    switch (e.kind) {
        case EncodingKind::Utf8:    return e.bomBytes ? L"UTF-8 BOM" : L"UTF-8";
        case EncodingKind::Utf16LE: return L"UTF-16 LE";
        case EncodingKind::Utf16BE: return L"UTF-16 BE";
        case EncodingKind::Ansi: {
            CPINFOEXW info{};
            if (GetCPInfoExW(e.codepage, 0, &info)) return info.CodePageName;
            wchar_t buf[64];
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
