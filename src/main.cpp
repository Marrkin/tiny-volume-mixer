#include "mixer_window.hpp"
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count = 0;
    auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    bool background = false;
    for (int i = 1; arguments && i < count; ++i) {
        if (wcscmp(arguments[i], L"--background") == 0) background = true;
    }
    LocalFree(arguments);

    const auto mutex = CreateMutexW(nullptr, FALSE, L"Local\\TinyVolumeMixer.Singleton.v1");
    if (!mutex) {
        MessageBoxW(nullptr, L"Não foi possível iniciar o aplicativo.", L"TinyVolumeMixer", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!background) {
            const auto existing = FindWindowW(tvm::kWindowClass, nullptr);
            if (existing) {
                DWORD process = 0;
                GetWindowThreadProcessId(existing, &process);
                AllowSetForegroundWindow(process);
            }
            PostMessageW(existing ? existing : HWND_BROADCAST,
                         RegisterWindowMessageW(tvm::kShowMessage), 0, 0);
        }
        CloseHandle(mutex);
        return 0;
    }

    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) { CloseHandle(mutex); return 1; }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    int result = 1;
    {
        tvm::MixerWindow app(instance);
        if (app.Create()) result = app.Run(background);
        else MessageBoxW(nullptr, L"Não foi possível criar a janela do mixer.",
                         L"TinyVolumeMixer", MB_OK | MB_ICONERROR);
    }
    CoUninitialize();
    CloseHandle(mutex);
    return result;
}
