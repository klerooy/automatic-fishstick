#pragma once
#include <string>

struct Config {
    // ESP
    bool esp_enabled = true;
    bool show_box = true;
    bool show_name = true;
    bool show_health = true;
    bool show_distance = true;
    bool show_weapon = true;
    bool show_skeleton = true;
    bool show_head = true;
    bool show_self = false;
    bool show_npcs = true;
    float max_distance = 300.0f; // meters
    float line_width = 2.0f;

    // Aim
    bool aim_enabled = false;
    bool aim_show_fov = true;
    float aim_fov = 100.0f; // pixels
    int aim_key = 0x12; // ALT
    int aim_smooth = 30; // 1-100
    int aim_bone = 0; // 0=head,1=neck,2=chest,3=pelvis
    float aim_cal = 100.0f;
    bool aim_require_fire = false;

    // Trigger
    bool trigger_enabled = false;
    int trigger_key = 0; // 0 = same as aim
    int trigger_delay = 80;
    float trigger_fov = 10.0f;

    // Misc
    bool menu_open = true;
    int menu_key = 0x2D; // INSERT

    // Colors (RGBA)
    unsigned int col_enemy = 0xFFFF4444;
    unsigned int col_team = 0xFF44FF44;
    unsigned int col_npc = 0xFFFFFF44;
    unsigned int col_dead = 0x88888888;
};

Config& cfg();
void config_load();
void config_save();