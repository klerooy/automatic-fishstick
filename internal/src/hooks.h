#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

namespace hooks {
bool install();
void uninstall();
bool is_initialized();
HWND get_hwnd();
} // namespace hooks