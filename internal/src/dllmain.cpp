#include <windows.h>
#include <thread>
#include "hooks.h"
#include "game.h"
#include "config.h"

static HMODULE g_module = nullptr;
static volatile bool g_unload = false;

static void main_thread(HMODULE mod) {
    // Wait for GameAssembly.dll to load (game may still be initializing)
    for (int i = 0; i < 600; ++i) {
        if (GetModuleHandleW(L"GameAssembly.dll")) break;
        Sleep(100);
    }
    if (!GetModuleHandleW(L"GameAssembly.dll")) {
        FreeLibraryAndExitThread(mod, 1);
        return;
    }

    config_load();
    game::init();

    if (!hooks::install()) {
        game::shutdown();
        FreeLibraryAndExitThread(mod, 2);
        return;
    }

    // Wait for END key or process exit
    while (!g_unload) {
        if (GetAsyncKeyState(VK_END) & 0x8000) {
            Sleep(200); // debounce
            break;
        }
        Sleep(50);
    }

    config_save();
    hooks::uninstall();
    game::shutdown();
    Sleep(100);
    FreeLibraryAndExitThread(mod, 0);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = hinst;
        DisableThreadLibraryCalls(hinst);
        CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            main_thread((HMODULE)p);
            return 0;
        }, hinst, 0, nullptr);
    }
    if (reason == DLL_PROCESS_DETACH) {
        g_unload = true;
    }
    return TRUE;
}