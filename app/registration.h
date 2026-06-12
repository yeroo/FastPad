#pragma once
#include <string>

namespace fastpad {

// Per-user "Open With" shell registration. Writes ONLY under HKCU
// (Classes\Applications, a FastPad.Document ProgID, Capabilities +
// RegisteredApplications) - never elevates, never touches HKLM. Making
// FastPad the DEFAULT handler still requires the user to pick it once in
// Settings > Default apps; Windows does not let apps self-assign defaults.
bool register_open_with(std::wstring* err);
bool unregister_open_with(std::wstring* err);
bool is_registered();

} // namespace fastpad
