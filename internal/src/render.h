#pragma once
#include <windows.h>
#include <d3d11.h>

namespace render {
void init(HWND hwnd, ID3D11Device* dev, ID3D11DeviceContext* ctx);
void shutdown();
void begin_frame(HWND hwnd);
void draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv);
void end_frame(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv);
} // namespace render