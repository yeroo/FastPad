#include "app/settings.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <string>

namespace fastpad {

static std::wstring settings_path() {
    wchar_t buf[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH) == 0)
        return L"";
    return std::wstring(buf) + L"\\FastPad\\settings.txt";
}

AppSettings load_settings() {
    AppSettings s;
    const std::wstring path = settings_path();
    if (path.empty()) return s;

    HANDLE hf = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return s;

    char buf[512] = {};
    DWORD read = 0;
    ReadFile(hf, buf, sizeof(buf) - 1, &read, nullptr);
    CloseHandle(hf);

    // Parse key=value lines
    char* p = buf;
    while (*p) {
        char* nl = p;
        while (*nl && *nl != '\n' && *nl != '\r') ++nl;
        char line[512] = {};
        size_t len = (size_t)(nl - p);
        if (len > 0 && len < sizeof(line)) {
            memcpy(line, p, len);
            if (strncmp(line, "fontSize=", 9) == 0) {
                char* end = nullptr;
                float v = strtof(line + 9, &end);
                if (end && end != line + 9) {
                    // Clamp 6..72
                    if (v < 6.0f) v = 6.0f;
                    if (v > 72.0f) v = 72.0f;
                    s.fontSize = v;
                }
            } else if (strncmp(line, "wordWrap=", 9) == 0) {
                s.wordWrap = (line[9] == '1');
            }
        }
        p = nl;
        while (*p == '\n' || *p == '\r') ++p;
    }
    return s;
}

void save_settings(const AppSettings& s) {
    const std::wstring path = settings_path();
    if (path.empty()) return;

    // Ensure directory exists
    const std::wstring dir = path.substr(0, path.rfind(L'\\'));
    CreateDirectoryW(dir.c_str(), nullptr);

    char buf[128] = {};
    int n = sprintf_s(buf, sizeof(buf), "fontSize=%.6g\nwordWrap=%d\n",
                      (double)s.fontSize, s.wordWrap ? 1 : 0);
    if (n <= 0) return;

    HANDLE hf = CreateFileW(path.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(hf, buf, (DWORD)n, &written, nullptr);
    CloseHandle(hf);
}

} // namespace fastpad
