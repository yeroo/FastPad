#pragma once
#include <windows.h>

namespace fastpad {

// The standard UI font (Segoe UI on modern Windows), taken from the system's
// non-client message font. Win32 child controls otherwise fall back to the
// legacy "System" bitmap font, which looks dated. Created once and kept for
// the process lifetime (intentionally not freed).
inline HFONT ui_font() {
    static HFONT font = nullptr;
    if (!font) {
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            font = CreateFontIndirectW(&ncm.lfMessageFont);
        if (!font)
            font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }
    return font;
}

// Apply ui_font() to every child control of `dlg`. Call from WM_CREATE after
// the children have been created.
inline void apply_ui_font(HWND dlg) {
    EnumChildWindows(dlg, [](HWND child, LPARAM f) -> BOOL {
        SendMessageW(child, WM_SETFONT, (WPARAM)f, TRUE);
        return TRUE;
    }, (LPARAM)ui_font());
}

} // namespace fastpad
