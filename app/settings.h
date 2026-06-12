#pragma once
namespace fastpad {
// Minimal user prefs persisted to %APPDATA%\FastPad\settings.txt (key=value
// lines). Never throws; missing/corrupt file -> defaults.
struct AppSettings {
    float fontSize = 14.0f;
    bool wordWrap = false;
};
AppSettings load_settings();
void save_settings(const AppSettings& s);
}
