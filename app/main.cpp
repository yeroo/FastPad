#include "app/app_window.h"
#include "app/commands.h"
#include <windows.h>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int showCmd) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t* file = (argc >= 2) ? argv[1] : nullptr;

    auto* win = fastpad::AppWindow::create(inst, showCmd, file);
    if (!win) return 1;

    using namespace fastpad;
    ACCEL acc[] = {
        { FCONTROL | FVIRTKEY, 'O', IDM_OPEN },
        { FCONTROL | FVIRTKEY, 'S', IDM_SAVE },
        { FCONTROL | FSHIFT | FVIRTKEY, 'S', IDM_SAVEAS },
        { FCONTROL | FVIRTKEY, 'F', IDM_FIND },
        { FVIRTKEY, VK_F3, IDM_FIND_NEXT },
        { FSHIFT | FVIRTKEY, VK_F3, IDM_FIND_PREV },
        { FCONTROL | FVIRTKEY, 'G', IDM_GOTO },
        { FCONTROL | FVIRTKEY, 'Z', IDM_UNDO },
        { FCONTROL | FVIRTKEY, 'Y', IDM_REDO },
        { FCONTROL | FVIRTKEY, 'X', IDM_CUT },
        { FCONTROL | FVIRTKEY, 'C', IDM_COPY },
        { FCONTROL | FVIRTKEY, 'V', IDM_PASTE },
        { FCONTROL | FVIRTKEY, 'A', IDM_SELECTALL },
    };
    HACCEL hAccel = CreateAcceleratorTableW(acc, (int)(sizeof(acc) / sizeof(acc[0])));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(win->hwnd(), hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    LocalFree(argv);
    return 0;
}
