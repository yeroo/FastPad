#pragma once
#include <windows.h>
#include <string>

namespace fastpad {

struct FindResult {
    std::wstring needle;
    bool matchCase = false;
    bool forward = true;   // true = Find Next, false = Find Prev
    bool ok = false;
};

// Owner-disabled modal find dialog. Pre-fills the edit with `initialNeedle`
// and the checkbox with `initialMatchCase`. Returns a FindResult; ok==false
// means the user cancelled.
FindResult show_find_dialog(HWND owner, const std::wstring& initialNeedle,
                            bool initialMatchCase);

} // namespace fastpad
