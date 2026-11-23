#include "config.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

static Config g_cfg;
Config& cfg() { return g_cfg; }

static void cfg_path(char* out, size_t n) {
    GetModuleFileNameA(nullptr, out, (DWORD)n);
    char* slash = strrchr(out, '\\');
    if (slash) *(slash + 1) = 0;
    strcat_s(out, n, "rr_internal.cfg");
}

void config_save() {
    char path[MAX_PATH];
    cfg_path(path, MAX_PATH);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "esp_enabled=%d\n", cfg().esp_enabled);
    fprintf(f, "show_box=%d\n", cfg().show_box);
    fprintf(f, "show_name=%d\n", cfg().show_name);
    fprintf(f, "show_health=%d\n", cfg().show_health);
    fprintf(f, "show_distance=%d\n", cfg().show_distance);
    fprintf(f, "show_weapon=%d\n", cfg().show_weapon);
    fprintf(f, "show_skeleton=%d\n", cfg().show_skeleton);
    fprintf(f, "show_head=%d\n", cfg().show_head);
    fprintf(f, "show_self=%d\n", cfg().show_self);
    fprintf(f, "max_distance=%.0f\n", cfg().max_distance);
    fprintf(f, "line_width=%.1f\n", cfg().line_width);
    fprintf(f, "aim_enabled=%d\n", cfg().aim_enabled);
    fprintf(f, "aim_fov=%.0f\n", cfg().aim_fov);
    fprintf(f, "aim_key=%d\n", cfg().aim_key);
    fprintf(f, "aim_smooth=%d\n", cfg().aim_smooth);
    fprintf(f, "aim_bone=%d\n", cfg().aim_bone);
    fprintf(f, "aim_cal=%.0f\n", cfg().aim_cal);
    fprintf(f, "trigger_enabled=%d\n", cfg().trigger_enabled);
    fprintf(f, "trigger_delay=%d\n", cfg().trigger_delay);
    fprintf(f, "trigger_fov=%.0f\n", cfg().trigger_fov);
    fclose(f);
}

void config_load() {
    char path[MAX_PATH];
    cfg_path(path, MAX_PATH);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        int val = atoi(eq + 1);
        float fval = (float)atof(eq + 1);
        if (!strcmp(line, "esp_enabled")) cfg().esp_enabled = val;
        else if (!strcmp(line, "show_box")) cfg().show_box = val;
        else if (!strcmp(line, "show_name")) cfg().show_name = val;
        else if (!strcmp(line, "show_health")) cfg().show_health = val;
        else if (!strcmp(line, "show_distance")) cfg().show_distance = val;
        else if (!strcmp(line, "show_weapon")) cfg().show_weapon = val;
        else if (!strcmp(line, "show_skeleton")) cfg().show_skeleton = val;
        else if (!strcmp(line, "show_head")) cfg().show_head = val;
        else if (!strcmp(line, "show_self")) cfg().show_self = val;
        else if (!strcmp(line, "max_distance")) cfg().max_distance = fval;
        else if (!strcmp(line, "line_width")) cfg().line_width = fval;
        else if (!strcmp(line, "aim_enabled")) cfg().aim_enabled = val;
        else if (!strcmp(line, "aim_fov")) cfg().aim_fov = fval;
        else if (!strcmp(line, "aim_key")) cfg().aim_key = val;
        else if (!strcmp(line, "aim_smooth")) cfg().aim_smooth = val;
        else if (!strcmp(line, "aim_bone")) cfg().aim_bone = val;
        else if (!strcmp(line, "aim_cal")) cfg().aim_cal = fval;
        else if (!strcmp(line, "trigger_enabled")) cfg().trigger_enabled = val;
        else if (!strcmp(line, "trigger_delay")) cfg().trigger_delay = val;
        else if (!strcmp(line, "trigger_fov")) cfg().trigger_fov = fval;
    }
    fclose(f);
}