#pragma once
#include "app/settings.h"
#include "core/document.h"
#include <windows.h>
#include <memory>
#include <optional>
#include <string>

namespace fastpad {

class Renderer;  // Task 7

class AppWindow {
public:
    ~AppWindow();
    static AppWindow* create(HINSTANCE inst, int showCmd, const wchar_t* fileArg);
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK viewProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleView(UINT msg, WPARAM wp, LPARAM lp);
    void onOpenDialog();
    void openPath(const wchar_t* path);
    // Saves to targetPath (empty = the document's own path), optionally
    // transcoding and/or forcing line endings (L"\n" / L"\r\n"). Shows a modal
    // progress popup for documents > 64 MB. On success reopens the saved file
    // as the new pristine original. Returns true only when the save succeeded;
    // false on failure or cancel.
    bool saveFlow(const std::wstring& targetPath, std::optional<EncodingInfo> transcode,
                  std::optional<std::wstring> forceEol = {});
    void onSaveAsDialog(std::optional<EncodingInfo> transcode,    // GetSaveFileNameW -> saveFlow
                        std::optional<std::wstring> forceEol = {});
    void buildMenu();
    void buildEncodingMenu(HMENU encMenu);
    void updateStatusBar();
    void scheduleStatusUpdate();               // coalescing 80 ms one-shot timer (id 3)
    void setStatusPart(int part, const wchar_t* text);   // SB_SETTEXT only on change
    void showStatusMessage(const wchar_t* text);  // transient status note, auto-clears after 5 s
    void updateTitleDirty();
    void layout();

    // ---- editing helpers (all require doc_ && renderer_) ----
    void afterEdit(uint64_t newCaret);       // caret + repaint + status + title
    uint64_t eraseSelectionIfAny();          // returns the caret after the erase
    uint64_t lineEndOffset(uint64_t off);    // end of the line containing off, before the break
    uint64_t caretLineVertical(int dir);     // Up/Down target offset (byte-column approximation)
    bool copySelection();                    // selection -> CF_UNICODETEXT; false when refused/empty
    void pasteClipboard();
    void onChar(wchar_t wch);
    void onKeyDown(WPARAM vk);
    void onEditCommand(UINT id);

    HWND hwnd_ = nullptr;
    HWND view_ = nullptr;                    // child window hosting the text view
    HWND status_ = nullptr;
    HINSTANCE inst_ = nullptr;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Renderer> renderer_;
    static constexpr UINT_PTR kStatusTimer = 3; // one-shot debounce for scroll-driven status updates
    static constexpr UINT_PTR kTransientTimer = 4; // one-shot 5 s auto-clear for showStatusMessage
    UINT_PTR indexTimer_ = 0;
    std::wstring titleBase_;                 // window title without the dirty '*'
    std::wstring statusText_[4];             // last text sent per status part (flicker guard)
    bool mouseSelecting_ = false;            // captured left-button drag in progress
    bool saving_ = false;                    // re-entry guard: the progress popup pumps messages
    // Live copy of the persisted prefs: every save_settings call goes through
    // this so persisting one setting can never reset the others to defaults.
    AppSettings settings_;

    // Find state
    std::wstring findNeedle_;
    bool findMatchCase_ = false;

    void doFind(bool forward);  // core search dispatch
};

} // namespace fastpad
