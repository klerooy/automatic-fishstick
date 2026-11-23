#include "aim.h"
#include "config.h"
#include "w2s.h"
#include <windows.h>
#include <cmath>
#include <algorithm>

namespace aim {

static float carry_x = 0, carry_y = 0;
static uint64_t last_tick = 0;
static uintptr_t locked_addr = 0;
static uint64_t lock_time = 0;

// Trigger state
static uintptr_t trig_addr = 0;
static uint64_t trig_acquired = 0;
static bool trig_fired = false;
static uint64_t trig_last_shot = 0;

static float response_ms(int smooth) {
    float alpha = std::clamp(smooth / 100.0f, 0.01f, 1.0f);
    if (alpha >= 1.0f) return 0;
    return -16.0f / logf(1.0f - alpha);
}

static float gain(float resp, unsigned elapsed) {
    float dt = (float)std::min(elapsed, 50u);
    return resp > 0 ? 1.0f - expf(-dt / resp) : 1.0f;
}

static Vec3 aim_point(const Player& p, int bone) {
    if (p.bone_count > 0 && bone < p.bone_count)
        return p.bones[bone];
    // Fallback estimates
    static const float heights[] = {1.65f, 1.48f, 1.20f, 0.88f};
    static const float crouch_h[] = {1.00f, 0.90f, 0.72f, 0.48f};
    Vec3 v = p.pos;
    v.y += (p.crouch == 1 ? crouch_h[bone] : heights[bone]);
    return v;
}

static void send_mouse(int dx, int dy) {
    if (!dx && !dy) return;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

void tick(const Snapshot& snap, int width, int height) {
    uint64_t now = GetTickCount64();
    unsigned elapsed = last_tick ? (unsigned)(now - last_tick) : 16;
    if (!elapsed) elapsed = 1;
    last_tick = now;

    bool held = (GetAsyncKeyState(cfg().aim_key) & 0x8000) != 0;
    bool firing = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    float cx = width * 0.5f, cy = height * 0.5f;

    // --- Aim assist ---
    if (cfg().aim_enabled && held && (!cfg().aim_require_fire || firing)) {
        float best_d2 = INFINITY;
        const Player* best = nullptr;
        Vec3 best_pt{};
        float best_dx = 0, best_dy = 0;
        float radius = cfg().aim_fov;
        float ring = locked_addr ? radius * 2.0f : radius;

        for (int i = 0; i < snap.count; ++i) {
            const Player& p = snap.players[i];
            if (!p.valid || p.local || p.dead || p.health == 0) continue;
            if (locked_addr && p.addr != locked_addr) continue;

            Vec3 pt = aim_point(p, cfg().aim_bone);
            float sx, sy;
            if (!w2s(snap.cam, pt, width, height, sx, sy)) continue;
            float dx = sx - cx, dy = sy - cy;
            float d2 = dx*dx + dy*dy;
            if (d2 > ring * ring) continue;
            if (d2 < best_d2) {
                best_d2 = d2; best = &p; best_pt = pt;
                best_dx = dx; best_dy = dy;
            }
        }

        if (best) {
            locked_addr = best->addr;
            lock_time = now;

            // Projection scale
            float proj = (1080.0f / height) *
                (tanf(snap.cam.fov * 3.14159265f / 360.0f) / 0.57735027f);
            float calib = cfg().aim_cal / 100.0f;
            float scale = proj * calib;

            float resp = response_ms(cfg().aim_smooth);
            float g = gain(resp, elapsed);
            float limit = std::max(8.0f, 32.0f * calib);

            float ex = best_dx * scale, ey = best_dy * scale;
            // Deadzone
            if (fabsf(ex) < 0.35f) { carry_x = 0; ex = 0; }
            if (fabsf(ey) < 0.35f) { carry_y = 0; ey = 0; }

            float step_x = std::clamp(ex * g + carry_x, -limit, limit);
            float step_y = std::clamp(ey * g + carry_y, -limit, limit);
            int mx = (int)step_x, my = (int)step_y;
            carry_x = step_x - mx;
            carry_y = step_y - my;
            send_mouse(mx, my);
        } else {
            if (now - lock_time > 500) locked_addr = 0;
            carry_x = carry_y = 0;
        }
    } else {
        locked_addr = 0;
        carry_x = carry_y = 0;
    }

    // --- Triggerbot ---
    if (cfg().trigger_enabled && !firing) {
        int trig_key = cfg().trigger_key ? cfg().trigger_key : cfg().aim_key;
        bool trig_held = (GetAsyncKeyState(trig_key) & 0x8000) != 0;
        if (trig_held) {
            float tol = cfg().trigger_fov;
            float best_d2 = tol * tol;
            const Player* target = nullptr;

            for (int i = 0; i < snap.count; ++i) {
                const Player& p = snap.players[i];
                if (!p.valid || p.local || p.dead || p.health == 0) continue;
                for (int bone = 0; bone <= 3; ++bone) {
                    Vec3 pt = aim_point(p, bone);
                    float sx, sy;
                    if (!w2s(snap.cam, pt, width, height, sx, sy)) continue;
                    float dx = sx - cx, dy = sy - cy;
                    float d2 = dx*dx + dy*dy;
                    if (d2 < best_d2) { best_d2 = d2; target = &p; }
                }
            }

            uintptr_t taddr = target ? target->addr : 0;
            if (!taddr) { trig_addr = 0; trig_fired = false; }
            else {
                if (trig_addr != taddr) {
                    trig_addr = taddr;
                    trig_acquired = now;
                    trig_fired = false;
                }
                unsigned delay = (unsigned)std::clamp(cfg().trigger_delay, 0, 300);
                if ((unsigned)(now - trig_acquired) >= delay &&
                    (!trig_fired || (unsigned)(now - trig_last_shot) >= std::max(delay, 16u))) {
                    INPUT click[2]{};
                    click[0].type = click[1].type = INPUT_MOUSE;
                    click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                    click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                    SendInput(2, click, sizeof(INPUT));
                    trig_fired = true;
                    trig_last_shot = now;
                }
            }
        } else {
            trig_addr = 0; trig_fired = false;
        }
    }
}

} // namespace aim