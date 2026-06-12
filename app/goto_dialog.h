#pragma once
#include <windows.h>
#include <cstdint>
namespace fastpad {
// Returns true when the user confirmed. *isLine: `value` is a 1-based line
// number ("123") vs a byte offset (":456" / ":0x1F").
bool show_goto_dialog(HWND owner, uint64_t* value, bool* isLine);
}
