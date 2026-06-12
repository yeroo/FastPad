#pragma once

// Menu / accelerator command IDs, shared by app_window.cpp (WM_COMMAND
// dispatch, menu construction) and main.cpp (accelerator table) so the two
// can never drift apart.
namespace fastpad {

enum : unsigned int {
    IDM_OPEN = 101, IDM_EXIT = 102, IDM_SAVE = 103, IDM_SAVEAS = 104,
    IDM_FIND = 208, IDM_FIND_NEXT = 209, IDM_FIND_PREV = 210,
    IDM_GOTO = 201,
    IDM_UNDO = 202, IDM_REDO = 203,
    IDM_CUT = 204, IDM_COPY = 205, IDM_PASTE = 206, IDM_SELECTALL = 207,
    IDM_ABOUT = 301,
    IDM_REGISTER = 310, IDM_UNREGISTER = 311,
    IDM_ENC_UTF8 = 400, IDM_ENC_UTF16LE = 401, IDM_ENC_UTF16BE = 402,
    IDM_SAVEAS_UTF8 = 403, IDM_SAVEAS_UTF16LE = 404,
    IDM_SAVEAS_LF = 405, IDM_SAVEAS_CRLF = 406,
    // View > Font size: BASE+0..9 map onto kFontSizes in app_window.cpp
    // (8, 10, 12, 14, 16, 18, 20, 24, 28, 36), occupying IDs 500-509.
    // IDM_VIEW_WRAP = 510 sits immediately after; next free view id is 511.
    // Do not add an 11th font size without moving WRAP.
    IDM_VIEW_FONTSIZE_BASE = 500,
    IDM_VIEW_WRAP = 510,
    IDM_ENC_CP_BASE = 1000,
};

} // namespace fastpad
