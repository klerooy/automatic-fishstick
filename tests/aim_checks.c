/* Offline-only: the actual aim module is compiled with input/clock/LOS mocks. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "rr_game.h"
#include "menu_bridge.h"
#include "rr_esp_style.h"
#include "rr_label_layout.h"
#include "rr_desync.h"
static SHORT keys[256];
static DWORD fake_now;
static int los_result = 1, send_calls, packet_count, send_result = -1;
static INPUT packets[64];
static BR_Config config;
static SHORT WINAPI fake_key(int vk) { return vk >= 0 && vk < 256 ? keys[vk] : 0; }
static DWORD WINAPI fake_clock(void) { return fake_now; }
static UINT WINAPI fake_send(UINT count, LPINPUT input, int size) {
    assert(size == sizeof(INPUT) && count <= 2 && packet_count + count < 64);
    memcpy(packets + packet_count, input, count * sizeof(INPUT));
    packet_count += count; ++send_calls;
    return send_result < 0 ? count : (UINT)send_result;
}
#define SendInput fake_send
#define GetAsyncKeyState fake_key
#define timeGetTime fake_clock
/* rr_aim.c reads a 64-bit tick in production so the follow filter cannot wrap;
 * the harness drives the same value from fake_now for deterministic timing. */
#define RR_AIM_NOW() ((ULONGLONG)fake_now)
#include "rr_recoil.c"
#include "rr_antiaim.c"
#include "rr_aim.c"
int rr_raw_mouse_delta(int* dx, int* dy) { if (dx) *dx = 0; if (dy) *dy = 0; return 0; }
BR_Config* br_get(void) { return &config; }
int rr_los_check(RR_V3 from, RR_V3 to) { (void)from; (void)to; return los_result; }
int game_w2s(const RR_CAMERA* camera, const RR_V3* point, int w, int h, float* x, float* y) {
    if (!camera || !camera->valid || !isfinite(point->x) || !isfinite(point->y) ||
        !isfinite(point->z) || point->z <= camera->pos.z) return 0;
    *x = w * 0.5f + point->x * 100.0f; *y = h * 0.5f - point->y * 100.0f;
    return 1;
}
static int clear_path(RR_V3 from, RR_V3 to) { return rr_los_check(from,to) == RR_LOS_CLEAR; }
static RR_PLAYER targets[2];
static RR_CAMERA camera;
static void fixture(void) {
    memset(&config,0,sizeof(config)); memset(targets,0,sizeof(targets)); memset(keys,0,sizeof(keys));
    memset(&camera,0,sizeof(camera));
    camera.valid=1; camera.fov=60; camera.fwd.z=2; camera.right.x=1; camera.up.y=1;
    config.aim_key=VK_RBUTTON; config.aim_fov=100; config.aim_smooth=30; config.aa_aim_cal=100;
    config.trigger_delay=80; los_result=RR_LOS_CLEAR; fake_now=1000; send_result=-1;
    send_calls=packet_count=0;
    for(int i=0;i<2;++i) {
        targets[i].valid=1; targets[i].addr=0x1000+i*0x100; targets[i].id=i+1;
        targets[i].dist=10; targets[i].pos=(RR_V3){0,0,10};
        targets[i].health=-1;   /* unknown: the filter must keep it          */
        targets[i].nbones=2; targets[i].bone_group[0]=0; targets[i].bone_group[2]=1;
        targets[i].bone[0][2]=10; targets[i].bone[1][2]=10;
    }
    rr_aim_update(NULL,0,NULL,1920,1080);
}
/* HOST_GATE_UNDER_TEST */

int main(void) {
    fixture();
    targets[0].bone[0][0]=0.3f; targets[1].bone[0][0]=0.8f;
    RR_AIM_PICK pick=rr_aim_pick(targets,2,&camera,1920,1080,&config,0,0,NULL);
    assert(pick.player==&targets[0]);
    targets[0].relationships=RR_REL_CLAN|RR_REL_POLICE;
    config.aim_targets=RR_REL_CLAN;
    pick=rr_aim_pick(targets,2,&camera,1920,1080,&config,0,0,NULL); assert(pick.player==&targets[1]);
    config.aim_targets=RR_REL_CLAN|RR_REL_POLICE;
    pick=rr_aim_pick(targets,2,&camera,1920,1080,&config,0,0,NULL); assert(pick.player==&targets[0]);
    /* A zero-health body sits closer to the crosshair and still loses: the pick
     * walks past it to the living target behind. */
    targets[0].bone[0][0]=0.1f; targets[0].health=0;
    targets[1].bone[0][0]=0.6f; targets[1].health=100;
    pick=rr_aim_pick(targets,2,&camera,1920,1080,&config,0,0,NULL); assert(pick.player==&targets[1]);
    targets[1].health=-1;
    pick=rr_aim_pick(targets,2,&camera,1920,1080,&config,0,0,NULL); assert(pick.player==&targets[1]);
    targets[1].health=0;
    pick=rr_aim_pick(targets,2,&camera,1920,1080,&config,0,0,NULL); assert(!pick.player);
    targets[0].health=-1; targets[1].health=-1;
    /* A held lock widens the ring to twice the FOV radius, so a target that
     * drifts just outside acquisition keeps the steering; without the lock the
     * same point is ignored. */
    fixture();
    config.aim_fov=100;
    targets[0].bone[0][0]=1.2f;
    assert(!rr_aim_pick(targets,1,&camera,1920,1080,&config,0,0,NULL).player);
    assert(rr_aim_pick(targets,1,&camera,1920,1080,&config,
                       targets[0].addr,targets[0].id,NULL).player==&targets[0]);
    fixture();
    targets[0].bone[0][0]=0.3f; targets[1].bone[0][0]=0.8f;
    config.aim_enabled=1; keys[VK_RBUTTON]=(SHORT)0x8000;
    rr_aim_update(targets,2,&camera,1920,1080); assert(send_calls==1&&rr_aim_is_active());
    assert(packets[0].mi.dwFlags==MOUSEEVENTF_MOVE&&packets[0].mi.dx>0&&packets[0].mi.dx<30);
    keys[VK_RBUTTON]=0; rr_aim_update(targets,2,&camera,1920,1080); assert(send_calls==1&&!rr_aim_is_active());
    keys[VK_RBUTTON]=(SHORT)0x8000; config.menu_open=1;
    rr_aim_update(targets,2,&camera,1920,1080); assert(send_calls==1&&!rr_aim_is_active());
    config.menu_open=0; config.aa_los_on=1; los_result=RR_LOS_UNKNOWN;
    rr_aim_update(targets,2,&camera,1920,1080); assert(send_calls==1);
    los_result=RR_LOS_BLOCKED; rr_aim_update(targets,2,&camera,1920,1080); assert(send_calls==1);
    los_result=RR_LOS_CLEAR; send_result=0; fake_now+=16;
    rr_aim_update(targets,2,&camera,1920,1080); assert(send_calls==2&&!rr_aim_is_active());
    puts("AIM CHECKS OK: crosshair selection, living-target filter, relation masks, keys, menu, LOS, smoothing, failed input");

    fixture();
    config.trigger_fov=10;
    pick=rr_trigger_pick(targets,1,&camera,1920,1080,&config,clear_path); assert(pick.player==&targets[0]);
    /* Screen tolerance: 5 px off the crosshair is a hit at any range, 40 px is
     * not, and the same point fires once the tolerance is opened. */
    targets[0].bone[0][0]=targets[0].bone[1][0]=0.05f;
    assert(rr_trigger_pick(targets,1,&camera,1920,1080,&config,clear_path).player==&targets[0]);
    targets[0].bone[0][0]=targets[0].bone[1][0]=0.4f;
    assert(!rr_trigger_pick(targets,1,&camera,1920,1080,&config,clear_path).player);
    config.trigger_fov=60;
    assert(rr_trigger_pick(targets,1,&camera,1920,1080,&config,clear_path).player==&targets[0]);
    config.trigger_fov=10;
    targets[0].bone[0][0]=targets[0].bone[1][0]=0;
    /* A NULL clear function is the "line of sight off" path. */
    assert(rr_trigger_pick(targets,1,&camera,1920,1080,&config,NULL).player==&targets[0]);
    los_result=RR_LOS_UNKNOWN; assert(!rr_trigger_pick(targets,1,&camera,1920,1080,&config,clear_path).player);
    los_result=RR_LOS_BLOCKED; assert(!rr_trigger_pick(targets,1,&camera,1920,1080,&config,clear_path).player);
    los_result=RR_LOS_CLEAR;
    targets[0].bone[0][1]=NAN; targets[0].bone[1][1]=NAN;
    assert(!rr_trigger_pick(targets,1,&camera,1920,1080,&config,clear_path).player);
    fixture(); targets[0].local=1; assert(!rr_trigger_pick(targets,1,&camera,1920,1080,&config,clear_path).player);
    targets[0].local=0; targets[0].dead=1; assert(!rr_trigger_pick(targets,1,&camera,1920,1080,&config,clear_path).player);
    targets[0].dead=0; targets[0].health=0; assert(!rr_trigger_pick(targets,1,&camera,1920,1080,&config,clear_path).player);
    targets[0].health=-1;
    targets[0].hidden_in_object=1; assert(!rr_trigger_pick(targets,1,&camera,1920,1080,&config,clear_path).player);
    targets[0].hidden_in_object=0; targets[0].relationships=RR_REL_CLAN|RR_REL_POLICE;
    config.trigger_targets=RR_REL_CLAN; assert(!rr_trigger_pick(targets,1,&camera,1920,1080,&config,clear_path).player);
    config.trigger_targets=RR_REL_CLAN|RR_REL_POLICE; assert(rr_trigger_pick(targets,1,&camera,1920,1080,&config,clear_path).player);
    puts("TRIGGER GEOMETRY OK: crosshair tolerance, NaN, corpses, exclusions and optional CLEAR LOS");

    RR_TRIGGER_STATE state={0};
    assert(!rr_trigger_ready(&state,1,1,100,80)); assert(!rr_trigger_ready(&state,1,1,179,80));
    assert(rr_trigger_ready(&state,1,1,180,80)); assert(!rr_trigger_ready(&state,1,1,181,80));
    assert(rr_trigger_ready(&state,1,1,260,80)); assert(!rr_trigger_ready(&state,1,2,261,80));
    assert(!rr_trigger_ready(&state,0,0,300,80)); assert(!rr_trigger_ready(&state,1,2,301,80));
    rr_trigger_ready(&state,0,0,0,0); assert(rr_trigger_ready(&state,1,1,100,0));
    assert(!rr_trigger_ready(&state,1,1,101,0)); assert(rr_trigger_ready(&state,1,1,116,0));
    rr_trigger_ready(&state,0,0,0,80); assert(!rr_trigger_ready(&state,1,1,0xFFFFFFD0u,80));
    assert(rr_trigger_ready(&state,1,1,0x30u,80));
    puts("TRIGGER TIMING OK: acquisition delay, target changes/loss, click interval and clock wrap");

    fixture(); config.trigger_enabled=1; keys[VK_RBUTTON]=(SHORT)0x8000;
    rr_aim_update(targets,1,&camera,1920,1080); assert(send_calls==0);
    fake_now=1080; rr_aim_update(targets,1,&camera,1920,1080);
    assert(send_calls==1&&packet_count==2&&!rr_aim_is_active());
    assert(packets[0].mi.dwFlags==MOUSEEVENTF_LEFTDOWN&&packets[1].mi.dwFlags==MOUSEEVENTF_LEFTUP);
    keys[VK_RBUTTON]=0; fake_now=1090; rr_aim_update(targets,1,&camera,1920,1080);
    keys[VK_RBUTTON]=(SHORT)0x8000; fake_now=1100; rr_aim_update(targets,1,&camera,1920,1080); assert(send_calls==1);
    keys[VK_LBUTTON]=(SHORT)0x8000; fake_now=1180; rr_aim_update(targets,1,&camera,1920,1080); assert(send_calls==1);
    fixture(); config.trigger_enabled=1; config.trigger_key=VK_SHIFT; config.trigger_delay=0;
    keys[VK_SHIFT]=(SHORT)0x8000; send_result=1;
    rr_aim_update(targets,1,&camera,1920,1080);
    assert(send_calls==2&&packet_count==3&&packets[2].mi.dwFlags==MOUSEEVENTF_LEFTUP);
    config.menu_open=1; rr_aim_update(targets,1,&camera,1920,1080); assert(send_calls==2);
    puts("TRIGGER INPUT OK: independent enable/key, delay, manual-fire exclusion and partial-input release");
    fixture(); config.trigger_enabled=1; config.trigger_delay=0; keys[VK_RBUTTON]=(SHORT)0x8000;
    g_rcam=camera; g_nrender=1; g_render=targets; focus_allowed=0;
    aa_tick(1,fake_now); assert(send_calls==0);
    focus_allowed=1; aa_tick(1,fake_now-51); assert(send_calls==0);
    aa_tick(1,fake_now); assert(send_calls==1);
    config.menu_open=1; fake_now+=16; aa_tick(2,fake_now); assert(send_calls==1);
    config.menu_open=0; aa_tick(2,fake_now); assert(send_calls==2);
    puts("HOST INPUT GATES OK: no input without focus or fresh snapshot; opening menu resets state");
    assert(rr_weapon_style("dp9_m1").atlas_cell==0);
    assert(rr_weapon_style("rc556").atlas_cell==1&&rr_weapon_style("kl545").atlas_cell==2);
    assert(rr_weapon_style("shotgun_01").atlas_cell==3&&rr_weapon_style("unknown").atlas_cell==7);
    assert(rr_health_fraction(-1)==0&&rr_health_fraction(250)==1&&rr_health_fraction(50)==0.5f);
    assert(rr_health_color(0.3f)==RRGB(255,210,73)&&rr_health_color(0.6f)==RRGB(255,210,73));
    assert(rr_health_color(0.61f)==RRGB(94,230,126));
    assert(rr_esp_labels_visible(150,15000)&&!rr_esp_labels_visible(151,15000));
    assert(rr_esp_labels_visible(10000,0)&&!rr_esp_labels_visible(NAN,0));
    assert(rr_esp_role_color("cop")!=rr_esp_role_color("civilian")&&rr_esp_role_label("prisoner"));
    assert((rr_skeleton_fade(0xFFFFFFFF,200)&255)<(rr_skeleton_fade(0xFFFFFFFF,10)&255));
    char label[96],price[96];
    RR_PLAYER owner={0};
    rr_format_vehicle_owner(&owner,label,sizeof(label));assert(!strcmp(label,"Owner: unavailable"));
    owner.vehicle_owner_known=1;
    rr_format_vehicle_owner(&owner,label,sizeof(label));assert(!strcmp(label,"No player owner"));
    snprintf(owner.role,sizeof(owner.role),"cop");
    rr_format_vehicle_owner(&owner,label,sizeof(label));assert(!strcmp(label,"Owner role: [COP]"));
    owner.vehicle_owner_public_id=INT64_MAX;
    rr_format_vehicle_owner(&owner,label,sizeof(label));assert(!strcmp(label,"Owner ID: 9223372036854775807"));
    snprintf(owner.vehicle_owner_name,sizeof(owner.vehicle_owner_name),"Alice");
    rr_format_vehicle_owner(&owner,label,sizeof(label));assert(!strcmp(label,"Owner: Alice"));
    puts("VEHICLE OWNER LABEL CHECKS OK: nickname, full public ID, owner role and empty/unknown state");
    rr_format_weapon_label("dp9_m1",1,label,sizeof(label));assert(!strcmp(label,"DP9 MK 1"));
    rr_format_weapon_label("dp9_m1",0,label,sizeof(label));assert(!strcmp(label,"DP9"));
    rr_format_weapon_label("rc556_MK12",1,label,sizeof(label));assert(!strcmp(label,"RC556 MK 12"));
    rr_format_weapon_label("kl545",1,label,sizeof(label));assert(!strcmp(label,"KL545"));
    assert(rr_weapon_mk("dp9_m1_other",NULL)==-1&&rr_weapon_mk("dp9_m9999999",NULL)==-1);
    rr_format_loot_price(0,3,1200,500,price,sizeof(price));assert(!strcmp(price,"NPC $500/ea"));
    rr_format_loot_price(1,3,1200,500,price,sizeof(price));assert(!strcmp(price,"Base $1200/ea"));
    rr_format_loot_price(2,3,1200,500,price,sizeof(price));assert(strstr(price,"NPC $500")&&strstr(price,"Base $1200"));
    rr_format_loot_price(0,1,1200,0,price,sizeof(price));assert(!price[0]);
    assert(!rr_relationship_allowed(RR_REL_PARTY,1,1,0)&&rr_relationship_allowed(RR_REL_PARTY,0,0,1));
    puts("MK/PRICE LABEL CHECKS OK: explicit levels, toggle, malformed IDs, unit prices, source choices and unknown values");
    assert(rr_loot_weapon_cell("AK-47")==1&&rr_loot_weapon_cell("M4A1 Carbine")==1);
    assert(rr_loot_weapon_cell("Sniper Rifle")==5&&rr_loot_weapon_cell("12 Gauge Shotgun")==3);
    assert(rr_loot_weapon_cell("MP5 SMG")==4&&rr_loot_weapon_cell("Combat Knife")==6);
    assert(rr_loot_weapon_cell("Combat Helmet")==-1&&rr_loot_weapon_cell("Gold Bar")==-1);
    assert(rr_loot_weapon_cell(NULL)==-1&&rr_loot_weapon_cell("")==-1);
    puts("LOOT WEAPON CHECKS OK: display-name tokens, class priority, whole-token matching and non-weapon items");
    {
        RR_LABEL_LAYOUT layout;
        float y;
        rr_label_reset(&layout);
        assert(layout.count == 0);
        y = 100.0f; assert(rr_label_place(&layout, 50.0f, &y, 80.0f, 16.0f, 18.0f) && y == 100.0f);
        /* The same corner steps one line down, then one line up, instead of
         * stacking two names on top of each other. */
        y = 100.0f; assert(rr_label_place(&layout, 50.0f, &y, 80.0f, 16.0f, 18.0f) && y == 118.0f);
        y = 100.0f; assert(rr_label_place(&layout, 50.0f, &y, 80.0f, 16.0f, 18.0f) && y == 82.0f);
        /* A free column to the side is accepted at the wanted line. */
        y = 100.0f; assert(rr_label_place(&layout, 400.0f, &y, 80.0f, 16.0f, 18.0f) && y == 100.0f);
        /* Six lines fit inside the search window; the seventh label is dropped. */
        rr_label_reset(&layout);
        for (int i = 0; i < 6; ++i) {
            float ty = 0.0f;
            assert(rr_label_place(&layout, 10.0f, &ty, 60.0f, 14.0f, 16.0f) && ty == (float)(i * 16));
        }
        y = 0.0f; assert(!rr_label_place(&layout, 10.0f, &y, 60.0f, 14.0f, 16.0f));
        /* A reserved anchor (player column, radar panel) blocks the area without
         * being moved by a later placement. */
        rr_label_reset(&layout);
        assert(rr_label_reserve(&layout, 200.0f, 90.0f, 120.0f, 60.0f) && layout.count == 1);
        assert(layout.rect[0].x == 200.0f && layout.rect[0].y == 90.0f);
        y = 100.0f; assert(rr_label_place(&layout, 210.0f, &y, 80.0f, 16.0f, 20.0f) && y == 60.0f);
        y = 100.0f; assert(rr_label_place(&layout, 900.0f, &y, 80.0f, 16.0f, 20.0f) && y == 100.0f);
        assert(!rr_label_reserve(&layout, 0.0f, 0.0f, 0.0f, 10.0f));
        puts("LABEL LAYOUT CHECKS OK: nearest free line, sideways space, dropped label when full, "
             "fixed anchors and reset");
    }
    {
        RR_DESYNC_STATE st, st2;
        assert(rr_desync_wrap_deg(0.0f) == 0.0f);
        assert(rr_desync_wrap_deg(90.0f) == 90.0f);
        assert(rr_desync_wrap_deg(180.0f) == -180.0f);
        assert(rr_desync_wrap_deg(190.0f) == -170.0f);
        assert(rr_desync_wrap_deg(-190.0f) == 170.0f);
        assert(rr_desync_wrap_deg(540.0f) == -180.0f);
        assert(rr_desync_wrap_deg(NAN) == 0.0f);
        assert(rr_desync_wrap_deg(INFINITY) == 0.0f);
        /* Spin advances 45 degrees per step and wraps back into range. */
        rr_desync_reset(&st, 7u);
        assert(rr_desync_step(&st, RR_DESYNC_SPIN, 100, 90, 1000) && st.offset_deg == 45.0f);
        assert(!rr_desync_step(&st, RR_DESYNC_SPIN, 100, 90, 1010));
        assert(rr_desync_step(&st, RR_DESYNC_SPIN, 100, 90, 1033) && st.offset_deg == 90.0f);
        assert(rr_desync_step(&st, RR_DESYNC_SPIN, 100, 90, 1066) && st.offset_deg == 135.0f);
        assert(rr_desync_step(&st, RR_DESYNC_SPIN, 100, 90, 1099) && st.offset_deg == -180.0f);
        /* Jitter alternates, static holds, random stays inside the range. */
        rr_desync_reset(&st, 7u);
        assert(rr_desync_step(&st, RR_DESYNC_JITTER, 100, 30, 0) && st.offset_deg == 30.0f);
        assert(rr_desync_step(&st, RR_DESYNC_JITTER, 100, 30, 34) && st.offset_deg == -30.0f);
        rr_desync_reset(&st, 7u);
        assert(rr_desync_step(&st, RR_DESYNC_STATIC, 50, 75, 0) && st.offset_deg == 75.0f);
        rr_desync_reset(&st, 99u);
        rr_desync_reset(&st2, 99u);
        for (int i = 0; i < 8; ++i) {
            unsigned at = (unsigned)(i * 34);
            assert(rr_desync_step(&st, RR_DESYNC_RANDOM, 100, 60, at));
            assert(rr_desync_step(&st2, RR_DESYNC_RANDOM, 100, 60, at));
            assert(st.offset_deg == st2.offset_deg);
            assert(st.offset_deg >= -60.0f && st.offset_deg <= 60.0f);
        }
        /* Rate mapping: 1 percent is 1 Hz, 100 percent is the 30 Hz ceiling. */
        rr_desync_reset(&st, 7u);
        assert(rr_desync_step(&st, RR_DESYNC_STATIC, 1, 10, 0));
        assert(!rr_desync_step(&st, RR_DESYNC_STATIC, 1, 10, 999));
        assert(rr_desync_step(&st, RR_DESYNC_STATIC, 1, 10, 1000));
        /* A disabled planner publishes nothing until it is stepped again. */
        rr_desync_reset(&st, 7u);
        assert(!st.started && st.offset_deg == 0.0f);
        puts("MODEL ANTI-AIM CHECKS OK: angle wrap, spin step, jitter/static/random ranges, "
             "rate mapping, determinism and reset");
    }
    puts("ESP STYLE CHECKS OK: weapon atlas mapping, health thresholds, role colors, distance fade, configurable/unlimited label range");

    fixture(); config.aim_enabled=1; config.aim_require_fire=1; keys[VK_RBUTTON]=(SHORT)0x8000;
    targets[0].bone[0][0]=0.4f; send_calls=packet_count=0;
    rr_aim_update(targets,1,&camera,1920,1080); assert(send_calls==0);
    keys[VK_LBUTTON]=(SHORT)0x8000; fake_now+=16;
    rr_aim_update(targets,1,&camera,1920,1080); assert(send_calls==1);
    puts("AIM FIRE GATE OK: require-fire suppresses steering until the left button is held");

    fixture(); config.trigger_head_only=1;
    targets[0].bone[0][0]=0.5f; targets[0].bone[1][0]=0.0f;
    assert(!rr_trigger_pick(targets,1,&camera,1920,1080,&config,clear_path).player);
    config.trigger_head_only=0;
    assert(rr_trigger_pick(targets,1,&camera,1920,1080,&config,clear_path).player==&targets[0]);
    puts("TRIGGER HEAD-ONLY OK: chest hits are ignored when the filter is set");

    fixture(); config.norecoil_enabled=1; config.norecoil_mode=RR_RECOIL_PATTERN;
    config.norecoil_strength=100; config.norecoil_horizontal=100;
    keys[VK_LBUTTON]=(SHORT)0x8000; send_calls=packet_count=0;
    for(int i=0;i<14;++i){ fake_now+=16; rr_aim_update(targets,1,&camera,1920,1080); }
    assert(send_calls>0 && packet_count>0);
    assert(packets[packet_count-1].mi.dwFlags==MOUSEEVENTF_MOVE&&packets[packet_count-1].mi.dy>0);
    assert(!rr_aim_is_active());
    puts("RECOIL PATTERN OK: fire button alone produces bounded downward mouse moves");

    fixture(); config.antiaim_enabled=1; config.antiaim_mode=RR_ANTIAIM_STATIC; config.antiaim_range=7;
    send_calls=packet_count=0;
    rr_aim_update(targets,1,&camera,1920,1080);
    assert(send_calls==1&&packets[0].mi.dx==7&&packets[0].mi.dy==0);
    rr_aim_update(targets,1,&camera,1920,1080); assert(send_calls==1);
    puts("ANTI-AIM STATIC OK: one offset is applied and then held");

    fixture(); config.antiaim_enabled=1; config.antiaim_mode=RR_ANTIAIM_SPIN; config.antiaim_range=5;
    keys[VK_LBUTTON]=(SHORT)0x8000; send_calls=0;
    rr_aim_update(targets,1,&camera,1920,1080); assert(send_calls==0);
    keys[VK_LBUTTON]=0; rr_aim_update(targets,1,&camera,1920,1080); assert(send_calls==1);
    puts("ANTI-AIM GATE OK: no view perturbation while a shot is being fired");

    puts("ALL AIM/TRIGGER CHECKS PASSED; SendInput was mocked throughout");
    return 0;
}
