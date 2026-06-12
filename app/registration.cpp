#include "app/registration.h"
#include <windows.h>
#include <shlobj.h>

namespace fastpad {

namespace {

constexpr const wchar_t* kAppKey =
    L"Software\\Classes\\Applications\\FastPad.exe";
constexpr const wchar_t* kProgIdKey = L"Software\\Classes\\FastPad.Document";
constexpr const wchar_t* kCapabilitiesKey = L"Software\\FastPad\\Capabilities";
constexpr const wchar_t* kRegisteredAppsKey =
    L"Software\\RegisteredApplications";

// File types offered in Open With (mapped to the FastPad.Document ProgID).
constexpr const wchar_t* kExtensions[] = {
    L".txt", L".log", L".csv", L".md", L".json", L".xml",
    L".ini", L".yaml", L".yml",
};

bool setStringValue(const wchar_t* subkey, const wchar_t* name,
                    const std::wstring& data, std::wstring* err) {
    const LSTATUS s = RegSetKeyValueW(HKEY_CURRENT_USER, subkey, name, REG_SZ,
        data.c_str(), (DWORD)((data.size() + 1) * sizeof(wchar_t)));
    if (s != ERROR_SUCCESS) {
        if (err)
            *err = L"Registry write failed (error " + std::to_wstring(s) +
                   L") at HKCU\\" + subkey;
        return false;
    }
    return true;
}

std::wstring exePath() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, n);
}

} // namespace

bool register_open_with(std::wstring* err) {
    const std::wstring command = L"\"" + exePath() + L"\" \"%1\"";
    const std::wstring appCmdKey = std::wstring(kAppKey) + L"\\shell\\open\\command";
    const std::wstring progIdCmdKey = std::wstring(kProgIdKey) + L"\\shell\\open\\command";
    const std::wstring assocKey = std::wstring(kCapabilitiesKey) + L"\\FileAssociations";

    if (!setStringValue(appCmdKey.c_str(), nullptr, command, err)) return false;
    if (!setStringValue(kAppKey, L"FriendlyAppName", L"FastPad", err)) return false;
    if (!setStringValue(progIdCmdKey.c_str(), nullptr, command, err)) return false;
    if (!setStringValue(kProgIdKey, L"FriendlyTypeName", L"FastPad Document", err)) return false;
    if (!setStringValue(kCapabilitiesKey, L"ApplicationName", L"FastPad", err)) return false;
    if (!setStringValue(kCapabilitiesKey, L"ApplicationDescription",
                        L"FastPad - any-size text editor", err)) return false;
    for (const wchar_t* ext : kExtensions)
        if (!setStringValue(assocKey.c_str(), ext, L"FastPad.Document", err)) return false;
    // This value is what Default-apps enumerates; write it LAST so a partial
    // failure above never leaves a dangling RegisteredApplications entry.
    if (!setStringValue(kRegisteredAppsKey, L"FastPad",
                        L"Software\\FastPad\\Capabilities", err)) return false;

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool unregister_open_with(std::wstring* err) {
    bool ok = true;
    auto note = [&](LSTATUS s, const wchar_t* what) {
        if (s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND) return;
        ok = false;
        if (err && err->empty())
            *err = L"Registry delete failed (error " + std::to_wstring(s) +
                   L") at HKCU\\" + what;
    };
    // Remove the RegisteredApplications pointer FIRST: is_registered() (and
    // Default apps) goes dark immediately even if a tree delete fails below.
    note(RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegisteredAppsKey, L"FastPad"),
         kRegisteredAppsKey);
    note(RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\FastPad"), L"Software\\FastPad");
    note(RegDeleteTreeW(HKEY_CURRENT_USER, kAppKey), kAppKey);
    note(RegDeleteTreeW(HKEY_CURRENT_USER, kProgIdKey), kProgIdKey);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

bool is_registered() {
    return RegGetValueW(HKEY_CURRENT_USER, kRegisteredAppsKey, L"FastPad",
        RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

} // namespace fastpad
