#include "render.h"
#include "game.h"
#include "config.h"
#include "aim.h"
#include "w2s.h"
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <cstdio>
#include <algorithm>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace render {

static WNDPROC oWndProc = nullptr;
static bool g_imgui_init = false;

static LRESULT CALLBACK hkWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (cfg().menu_open && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return 1;
    return CallWindowProcW(oWndProc, hwnd, msg, wp, lp);
}

void init(HWND hwnd, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(dev, ctx);
    oWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
    g_imgui_init = true;
}

void shutdown() {
    if (!g_imgui_init) return;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_imgui_init = false;
}

void begin_frame(HWND hwnd) {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

static ImU32 to_imu(unsigned int c) {
    // Input: 0xAARRGGBB -> ImGui: 0xAABBGGRR
    unsigned a = (c >> 24) & 0xFF;
    unsigned r = (c >> 16) & 0xFF;
    unsigned g = (c >> 8) & 0xFF;
    unsigned b = c & 0xFF;
    return IM_COL32(r, g, b, a);
}

static void draw_menu() {
    if (!cfg().menu_open) return;
    ImGui::SetNextWindowSize(ImVec2(420, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("RR Internal", &cfg().menu_open, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("ESP")) {
                ImGui::Checkbox("Enabled", &cfg().esp_enabled);
                ImGui::Checkbox("Box", &cfg().show_box);
                ImGui::Checkbox("Name", &cfg().show_name);
                ImGui::Checkbox("Health", &cfg().show_health);
                ImGui::Checkbox("Distance", &cfg().show_distance);
                ImGui::Checkbox("Weapon", &cfg().show_weapon);
                ImGui::Checkbox("Skeleton", &cfg().show_skeleton);
                ImGui::Checkbox("Head dot", &cfg().show_head);
                ImGui::Checkbox("Show self", &cfg().show_self);
                ImGui::SliderFloat("Max dist (m)", &cfg().max_distance, 10, 1000);
                ImGui::SliderFloat("Line width", &cfg().line_width, 1, 4);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Aim")) {
                ImGui::Checkbox("Aim assist", &cfg().aim_enabled);
                ImGui::Checkbox("Show FOV", &cfg().aim_show_fov);
                ImGui::SliderFloat("FOV (px)", &cfg().aim_fov, 10, 300);
                ImGui::SliderInt("Smooth", &cfg().aim_smooth, 1, 100);
                ImGui::SliderInt("Bone", &cfg().aim_bone, 0, 3);
                ImGui::SliderFloat("Calibration", &cfg().aim_cal, 10, 500);
                ImGui::Checkbox("Require fire", &cfg().aim_require_fire);
                ImGui::Separator();
                ImGui::Checkbox("Triggerbot", &cfg().trigger_enabled);
                ImGui::SliderInt("Delay (ms)", &cfg().trigger_delay, 0, 300);
                ImGui::SliderFloat("Trigger FOV", &cfg().trigger_fov, 1, 80);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings")) {
                if (ImGui::Button("Save config")) config_save();
                ImGui::SameLine();
                if (ImGui::Button("Load config")) config_load();
                ImGui::Text("Game: %s", game::is_ready() ? "connected" : "searching...");
                ImGui::Text("Players: %d", game::get_snapshot().count);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

static void draw_esp(const Snapshot& snap, int vw, int vh) {
    if (!cfg().esp_enabled || !snap.cam.valid) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float cx = vw * 0.5f, cy = vh * 0.5f;

    // FOV circle
    if (cfg().aim_enabled && cfg().aim_show_fov)
        dl->AddCircle(ImVec2(cx, cy), cfg().aim_fov, IM_COL32(255,255,255,80), 64, 1.0f);

    for (int i = 0; i < snap.count; ++i) {
        const Player& p = snap.players[i];
        if (!p.valid) continue;
        if (p.local && !cfg().show_self) continue;
        if (cfg().max_distance > 0 && p.dist > cfg().max_distance) continue;

        float fx, fy, hx, hy;
        bool feet_ok = w2s(snap.cam, p.pos, vw, vh, fx, fy);
        bool head_ok = w2s(snap.cam, p.head, vw, vh, hx, hy);
        if (!feet_ok && !head_ok) continue;
        if (!std::isfinite(fx) || !std::isfinite(fy)) continue;

        float bh = fabsf(fy - hy);
        if (bh < 4) bh = 4;
        float bw = bh * 0.52f;
        float bx = (fx + hx) * 0.5f - bw * 0.5f;
        float by = std::min(hy, fy);

        ImU32 col = p.dead ? to_imu(cfg().col_dead) :
                    p.local ? to_imu(cfg().col_team) : to_imu(cfg().col_enemy);

        // Box
        if (cfg().show_box)
            dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), col, 0, 0, cfg().line_width);

        // Head dot
        if (cfg().show_head && head_ok) {
            float r = std::clamp(bw * 0.11f, 2.0f, 9.0f);
            dl->AddCircleFilled(ImVec2(hx, hy), r, col);
        }

        // Health bar
        if (cfg().show_health && p.health >= 0) {
            float ratio = std::clamp((float)p.health / (float)p.max_health, 0.0f, 1.0f);
            float bar_x = bx - 5, bar_w = 3;
            dl->AddRectFilled(ImVec2(bar_x, by), ImVec2(bar_x + bar_w, by + bh), IM_COL32(20,20,20,180));
            float fill = bh * ratio;
            ImU32 hc = ratio > 0.5f ? IM_COL32(0,255,0,220) : ratio > 0.25f ? IM_COL32(255,255,0,220) : IM_COL32(255,0,0,220);
            dl->AddRectFilled(ImVec2(bar_x, by + bh - fill), ImVec2(bar_x + bar_w, by + bh), hc);
        }

        // Skeleton
        if (cfg().show_skeleton && p.bone_count > 1) {
            for (int b = 0; b + 1 < p.bone_count; ++b) {
                float ax, ay, bx2, by2;
                if (w2s(snap.cam, p.bones[b], vw, vh, ax, ay) &&
                    w2s(snap.cam, p.bones[b+1], vw, vh, bx2, by2))
                    dl->AddLine(ImVec2(ax, ay), ImVec2(bx2, by2), col, cfg().line_width * 0.7f);
            }
        }

        // Labels
        float label_y = by - 14;
        if (cfg().show_name && !p.name.empty()) {
            ImVec2 ts = ImGui::CalcTextSize(p.name.c_str());
            dl->AddText(ImVec2(fx - ts.x * 0.5f, label_y), IM_COL32(255,255,255,230), p.name.c_str());
            label_y -= 13;
        }
        float bottom_y = by + bh + 2;
        if (cfg().show_distance) {
            char buf[32]; snprintf(buf, sizeof(buf), "%.0fm", p.dist);
            ImVec2 ts = ImGui::CalcTextSize(buf);
            dl->AddText(ImVec2(fx - ts.x * 0.5f, bottom_y), IM_COL32(200,200,200,200), buf);
            bottom_y += 12;
        }
        if (cfg().show_weapon && !p.weapon.empty()) {
            ImVec2 ts = ImGui::CalcTextSize(p.weapon.c_str());
            dl->AddText(ImVec2(fx - ts.x * 0.5f, bottom_y), IM_COL32(255,200,100,200), p.weapon.c_str());
            bottom_y += 12;
        }
        if (cfg().show_health && p.health >= 0) {
            char buf[32]; snprintf(buf, sizeof(buf), "HP:%d", p.health);
            ImVec2 ts = ImGui::CalcTextSize(buf);
            dl->AddText(ImVec2(fx - ts.x * 0.5f, bottom_y), IM_COL32(100,255,100,200), buf);
        }
    }
}

void draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv) {
    // Hotkeys
    static bool ins_was = false;
    bool ins_now = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (ins_now && !ins_was) cfg().menu_open = !cfg().menu_open;
    ins_was = ins_now;

    static bool end_was = false;
    bool end_now = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
    if (end_now && !end_was) { /* request unload */ PostQuitMessage(0); }
    end_was = end_now;

    Snapshot snap = game::get_snapshot();
    RECT rc; GetClientRect(hooks::get_hwnd(), &rc);
    int vw = rc.right, vh = rc.bottom;

    draw_esp(snap, vw, vh);
    draw_menu();

    // Aim tick (runs every frame when game focused)
    if (!cfg().menu_open && snap.cam.valid && snap.count > 0)
        aim::tick(snap, vw, vh);
}

void end_frame(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv) {
    ImGui::Render();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

} // namespace render