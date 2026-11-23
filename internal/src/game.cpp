#include "game.h"
#include "unity.h"
#include "offsets.h"
#include <windows.h>
#include <thread>
#include <atomic>

namespace game {

static std::mutex g_mtx;
static Snapshot g_snap;
static std::atomic<bool> g_running{false};
static std::thread g_worker;
static uintptr_t g_world = 0;
static uintptr_t g_wem = 0;
static uintptr_t g_cam_obj = 0;
static uintptr_t g_local = 0;
static bool g_il2cpp_ok = false;

static bool discover() {
    // Try il2cpp API first
    if (g_il2cpp_ok) {
        uintptr_t wm = unity::get_static_instance("Models.Models", "WorldModel");
        if (wm) {
            uintptr_t wem = unity::read<uintptr_t>(wm + off::world_entities);
            if (unity::valid_ptr(wem)) {
                g_world = wm;
                g_wem = wem;
            }
        }
        // Camera via BattleModel
        uintptr_t bm = unity::get_static_instance("Game.Battle", "BattleModel");
        if (bm) {
            uintptr_t cam = unity::read<uintptr_t>(bm + off::battle_camera);
            if (unity::valid_ptr(cam)) g_cam_obj = cam;
        }
    }

    // Fallback: static RVA
    if (!g_wem) {
        HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
        if (!ga) return false;
        uintptr_t base = reinterpret_cast<uintptr_t>(ga);
        uintptr_t sf = unity::read<uintptr_t>(base + off::sf_world_model);
        if (unity::valid_ptr(sf)) {
            uintptr_t wm = unity::read<uintptr_t>(sf);
            if (unity::valid_ptr(wm)) {
                g_world = wm;
                g_wem = unity::read<uintptr_t>(wm + off::world_entities);
            }
        }
        if (!g_cam_obj) {
            uintptr_t csf = unity::read<uintptr_t>(base + off::sf_battle_camera);
            if (unity::valid_ptr(csf)) {
                g_cam_obj = unity::read<uintptr_t>(csf);
            }
        }
    }
    return unity::valid_ptr(g_wem);
}

static void read_camera(Camera& cam) {
    cam.valid = false;
    if (!unity::valid_ptr(g_cam_obj)) return;
    __try {
        cam.pos = unity::read<Vec3>(g_cam_obj + off::cam_pos);
        cam.fwd = unity::read<Vec3>(g_cam_obj + off::cam_fwd);
        cam.right = unity::read<Vec3>(g_cam_obj + off::cam_right);
        cam.up = unity::read<Vec3>(g_cam_obj + off::cam_up);
        cam.rot = unity::read<Vec4>(g_cam_obj + off::cam_rot);
        // Validate basis vectors
        float fl = cam.fwd.x*cam.fwd.x + cam.fwd.y*cam.fwd.y + cam.fwd.z*cam.fwd.z;
        if (fl < 0.5f || fl > 2.0f) return;
        if (!std::isfinite(cam.pos.x) || !std::isfinite(cam.pos.y) || !std::isfinite(cam.pos.z)) return;
        cam.fov = 60.0f; // Default; game uses Cinemachine, actual FOV from camera component
        cam.valid = true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static void fill_player(Player& p, uintptr_t obj) {
    p.addr = obj;
    p.valid = false;
    __try {
        p.id = unity::read<int32_t>(obj + off::char_id);

        // Position from CharacterTransformModel
        uintptr_t tr = unity::read<uintptr_t>(obj + off::char_transform);
        if (unity::valid_ptr(tr)) {
            p.pos.x = unity::read<float>(tr + off::ctm_x);
            p.pos.y = unity::read<float>(tr + off::ctm_y);
            p.pos.z = unity::read<float>(tr + off::ctm_z);
            p.yaw = unity::read<float>(tr + off::ctm_yaw);
        }
        if (!std::isfinite(p.pos.x)) return;

        // Head estimate
        p.head = p.pos;
        p.head.y += 1.7f;

        // Dead flag
        uintptr_t dead_model = unity::read<uintptr_t>(obj + off::char_isdead);
        if (unity::valid_ptr(dead_model)) {
            uintptr_t dead_data = unity::read<uintptr_t>(dead_model + 0x10);
            if (unity::valid_ptr(dead_data))
                p.dead = unity::read<uint8_t>(dead_data + off::prim_value) != 0;
        }

        // Health
        uintptr_t data = unity::read<uintptr_t>(obj + off::char_data);
        if (unity::valid_ptr(data)) {
            uintptr_t priv = unity::read<uintptr_t>(data + off::data_private);
            if (unity::valid_ptr(priv)) {
                uintptr_t hm = unity::read<uintptr_t>(priv + off::private_health);
                if (unity::valid_ptr(hm)) {
                    uintptr hd = unity::read<uintptr_t>(hm + off::health_value);
                    if (unity::valid_ptr(hd))
                        p.health = unity::read<int32_t>(hd + off::prim_value);
                }
            }
            // Role
            uintptr_t common = unity::read<uintptr_t>(data + off::data_common);
            if (unity::valid_ptr(common)) {
                uintptr_t role_ref = unity::read<uintptr_t>(common + off::common_role);
                if (unity::valid_ptr(role_ref)) {
                    uintptr_t role_val = unity::read<uintptr_t>(role_ref + off::ref_value);
                    if (unity::valid_ptr(role_val)) {
                        uintptr_t name_str = unity::read<uintptr_t>(role_val + off::desc_name);
                        if (unity::valid_ptr(name_str))
                            p.role = unity::read_string(name_str);
                    }
                }
            }
            // Armor
            uintptr_t pub = unity::read<uintptr_t>(data + off::data_public);
            if (unity::valid_ptr(pub)) {
                uintptr_t am = unity::read<uintptr_t>(pub + off::public_armor);
                if (unity::valid_ptr(am)) {
                    uintptr_t ad = unity::read<uintptr_t>(am + 0x10);
                    if (unity::valid_ptr(ad))
                        p.armor = unity::read<uint8_t>(ad + off::prim_value) ? 1 : 0;
                }
            }
        }

        // Crouch
        uintptr_t cm = unity::read<uintptr_t>(obj + off::char_crouch);
        if (unity::valid_ptr(cm)) {
            uintptr_t cd = unity::read<uintptr_t>(cm + off::crouch_bool);
            if (unity::valid_ptr(cd))
                p.crouch = unity::read<uint8_t>(cd + off::prim_value) ? 1 : 0;
        }
        if (p.crouch == 1) p.head.y = p.pos.y + 1.1f;

        // Weapon name
        uintptr_t wm = unity::read<uintptr_t>(obj + off::char_weapon);
        if (unity::valid_ptr(wm)) {
            uintptr_t item = unity::read<uintptr_t>(wm + off::weapon_item);
            if (unity::valid_ptr(item)) {
                uintptr_t desc = unity::read<uintptr_t>(item + 0x10);
                if (unity::valid_ptr(desc)) {
                    uintptr_t ns = unity::read<uintptr_t>(desc + off::desc_name);
                    if (unity::valid_ptr(ns))
                        p.weapon = unity::read_string(ns);
                }
            }
        }

        // InVehicle
        uintptr_t iv = unity::read<uintptr_t>(obj + off::char_inveh);
        if (unity::valid_ptr(iv)) {
            int vid = unity::read<int32_t>(iv + off::inveh_id);
            p.in_vehicle = (vid > 0);
        }

        // Nickname from slot data (via PrivateModel)
        uintptr_t priv_model = unity::read<uintptr_t>(obj + off::char_private);
        if (unity::valid_ptr(priv_model)) {
            // Name is typically at slot description; use id as fallback
        }
        if (p.name.empty()) p.name = "#" + std::to_string(p.id);

        p.valid = true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        p.valid = false;
    }
}

static void worker_loop() {
    while (g_running.load()) {
        if (!unity::valid_ptr(g_wem)) {
            discover();
            Sleep(500);
            continue;
        }

        Snapshot snap{};
        read_camera(snap.cam);

        uintptr_t dict = unity::read<uintptr_t>(g_wem + off::wem_battle_chars);
        auto entries = unity::dict_iterate(dict, MAX_PLAYERS);

        // Find local player via BattleEntitiesModel.Player
        uintptr_t local_addr = 0;
        if (g_il2cpp_ok) {
            uintptr_t bm = unity::get_static_instance("Game.Battle", "BattleModel");
            if (bm) {
                uintptr_t be = unity::read<uintptr_t>(bm + off::battle_entities);
                if (unity::valid_ptr(be)) {
                    uintptr_t player_ent = unity::read<uintptr_t>(be + off::battle_player);
                    if (unity::valid_ptr(player_ent)) {
                        // PlayerEntityModel -> ClientModel -> character
                        local_addr = unity::read<uintptr_t>(player_ent + 0x18);
                    }
                }
            }
        }

        int n = 0;
        for (auto& e : entries) {
            if (n >= MAX_PLAYERS) break;
            Player& p = snap.players[n];
            fill_player(p, e.value);
            if (!p.valid) continue;
            if (snap.cam.valid)
                p.dist = distance(snap.cam.pos, p.pos);
            if (e.value == local_addr || (g_local && e.value == g_local))
                p.local = true;
            ++n;
        }
        snap.count = n;
        snap.tick = GetTickCount64();

        std::lock_guard lk(g_mtx);
        g_snap = snap;
        if (!g_local && local_addr) g_local = local_addr;

        Sleep(8); // ~120 Hz snapshot rate
    }
}

bool init() {
    g_il2cpp_ok = unity::init_il2cpp();
    discover();
    g_running = true;
    g_worker = std::thread(worker_loop);
    return true;
}

void shutdown() {
    g_running = false;
    if (g_worker.joinable()) g_worker.join();
}

Snapshot get_snapshot() {
    std::lock_guard lk(g_mtx);
    return g_snap;
}

uintptr_t local_player() { return g_local; }
bool is_ready() { return unity::valid_ptr(g_wem); }

} // namespace game