#pragma once
#include "w2s.h"
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>

constexpr int MAX_PLAYERS = 128;
constexpr int MAX_BONES = 24;

struct Player {
    uintptr_t addr = 0;
    int32_t id = 0;
    Vec3 pos{};
    Vec3 head{};
    float dist = 0;
    float yaw = 0;
    int health = -1;
    int max_health = 100;
    int armor = -1;
    int crouch = -1;
    bool dead = false;
    bool local = false;
    bool in_vehicle = false;
    bool is_npc = false;
    std::string name;
    std::string role;
    std::string weapon;
    Vec3 bones[MAX_BONES]{};
    int bone_count = 0;
    bool valid = false;
};

struct Snapshot {
    Player players[MAX_PLAYERS];
    int count = 0;
    Camera cam;
    uint64_t tick = 0;
};

namespace game {

bool init();
void shutdown();
Snapshot get_snapshot();
uintptr_t local_player();
bool is_ready();

} // namespace game