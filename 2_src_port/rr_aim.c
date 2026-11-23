/* rr_aim.c - Select target points and convert projected errors to mouse counts.
   Selection uses snapshot data; the caller decides whether to send input.
*/
#include "rr_aim.h"
#include "rr_recoil.h"
#include "rr_antiaim.h"
#include "rr_cham_anchors.h"
#include "rr_body_joints.h"
#include "menu_bridge.h"
#include <math.h>
#include <windows.h>
#include <string.h>
#include "rr_los.h"

#define RR_AIM_PI 3.14159265358979f
#define RR_TAN30  0.57735026919f   /* tan(30 deg): reference half-FOV */
#define RR_STEP_DT_MAX 50u

#ifndef RR_AIM_NOW
#define RR_AIM_NOW() GetTickCount64()
#endif

/* A corpse keeps its roster entry and transform for a while. Known zero health
 * is a corpse; the death flag can lag a tick. A failed read (-1) stays a
 * candidate so one bad pointer cannot empty the target list. */
static int aim_alive(const RR_PLAYER* p) {
    if (p->dead) return 0;
    if (p->health == 0) return 0;
    return 1;
}

/* Knocked/prone classification, ordered by authority:
 *   1. replicated KnockdownModel flag - survives rig loss, authoritative
 *   2. stance name token             - secondary, some builds fold prone here
 *   3. real HEAD joint height        - fallback only, never the estimated head
 * Snapshot head.y is an estimate (1.75 standing / 1.05 crouched) whenever no rig
 * exists, so reading it would classify every knocked player as standing. */
static int is_downed(const RR_PLAYER* p) {
    if (!p || !p->valid) return 0;
    if (p->knockdown >= 0) return p->knockdown;
    if (p->stance_prone >= 0) return p->stance_prone;
    if (p->nbones < 1) return 0;
    if (!(p->full_rig || p->cham_valid || p->nbones >= 4)) return 0;
    const int idx = p->bone_group[RR_AIM_HEAD];
    if (idx < 0 || idx >= p->nbones) return 0;
    const float head_y = p->bone[idx][1];
    if (!isfinite(head_y)) return 0;
    const float h = head_y - p->pos.y;
    return isfinite(h) && h >= 0.0f && h < 0.55f;
}

static float aim_response_ms(int smooth) {
    const float alpha = fmaxf(0.01f, fminf(1.0f, smooth / 100.0f));
    return alpha >= 1.0f ? 0.0f : -16.0f / logf(1.0f - alpha);
}

static float aim_gain(float response_ms, unsigned elapsed_ms) {
    const float dt = (float)(elapsed_ms > RR_STEP_DT_MAX ? RR_STEP_DT_MAX : elapsed_ms);
    return response_ms > 0.0f ? 1.0f - expf(-dt / response_ms) : 1.0f;
}

int rr_aim_motion_point(RR_V3 point, RR_V3 position, RR_V3 previous,
                        unsigned sample_ms, unsigned age_ms, int smooth,
                        unsigned elapsed_ms, RR_V3* out) {
    if (!out || sample_ms < 4 || sample_ms > 250 || age_ms > 120) return 0;
    const float vx = (position.x - previous.x) * 1000.0f / sample_ms;
    const float vz = (position.z - previous.z) * 1000.0f / sample_ms;
    if (!isfinite(vx) || !isfinite(vz)) return 0;
    const float speed = hypotf(vx, vz);
    if (!(speed >= 0.15f && speed <= 25.0f)) return 0;
    const float gain = aim_gain(aim_response_ms(smooth), elapsed_ms);
    /* Verified analytically and numerically: the discrete follow trails by
     * dt/gain, and the snapshot itself is age_ms old. Both terms add.
     * Residual measured 0.000000 m across periods 8/16/33 ms, smooth 10..99,
     * ages 0/8/16/30 ms. Passing age_ms = 0 was the previous defect. */
    float lead_ms = (gain > 0.0f
        ? (float)(elapsed_ms > RR_STEP_DT_MAX ? RR_STEP_DT_MAX : elapsed_ms) / gain
        : 0.0f) + (float)age_ms;
    lead_ms = fmaxf(0.0f, fminf(lead_ms, 220.0f));
    const float s = lead_ms * 0.001f;
    *out = (RR_V3){ point.x + vx * s, point.y, point.z + vz * s };
    return 1;
}

/* Motion history. Stamped with the SNAPSHOT PUBLICATION time, not the worker
 * time, so the velocity sample interval matches the data it describes. Updated
 * after selection, so the next frame sees a real delta instead of zero. */
typedef struct {
    uint64_t addr;
    int32_t  id;
    RR_V3    pos, vel;
    unsigned pub_at;
    int      samples;
} RR_AIM_TRACK;
#define RR_TRACK_MAX 128
static RR_AIM_TRACK g_track[RR_TRACK_MAX];
static int g_track_count;

static void track_update(const RR_PLAYER* p) {
    int idx = -1;
    for (int j = 0; j < g_track_count; ++j)
        if (g_track[j].addr == p->addr && g_track[j].id == p->id) { idx = j; break; }
    if (idx < 0) {
        if (g_track_count >= RR_TRACK_MAX) return;
        idx = g_track_count++;
        memset(&g_track[idx], 0, sizeof(g_track[idx]));
    }
    RR_AIM_TRACK* t = &g_track[idx];
    t->addr = p->addr; t->id = p->id;
    const unsigned pub = p->snap_pub_at;
    const unsigned dt = pub - t->pub_at;
    if (t->samples == 0 || dt < 4 || dt > 250) {
        t->pos = p->pos; t->vel = (RR_V3){0}; t->pub_at = pub; t->samples = 1;
        return;
    }
    const float dx = p->pos.x - t->pos.x, dy = p->pos.y - t->pos.y, dz = p->pos.z - t->pos.z;
    if (sqrtf(dx*dx + dy*dy + dz*dz) > 15.0f) {   /* teleport */
        t->pos = p->pos; t->vel = (RR_V3){0}; t->pub_at = pub; t->samples = 1;
        return;
    }
    const float inv = 1000.0f / (float)dt;
    const float a = t->samples >= 3 ? 0.35f : 0.6f;
    t->vel.x = t->vel.x*(1-a) + dx*inv*a;
    t->vel.y = t->vel.y*(1-a) + dy*inv*a;
    t->vel.z = t->vel.z*(1-a) + dz*inv*a;
    t->pos = p->pos; t->pub_at = pub;
    if (t->samples < 10) ++t->samples;
}

static int predict_motion(const RR_PLAYER* p, int smooth, unsigned elapsed_ms,
                          unsigned age_ms, RR_V3* point) {
    for (int i = 0; i < g_track_count; ++i) {
        if (g_track[i].addr != p->addr || g_track[i].id != p->id) continue;
        const RR_AIM_TRACK* t = &g_track[i];
        if (t->samples < 2) return 0;
        const float speed = hypotf(t->vel.x, t->vel.z);
        if (!(speed >= 0.15f && speed <= 25.0f)) return 0;
        const float gain = aim_gain(aim_response_ms(smooth), elapsed_ms);
        float lead = (gain > 0.0f
            ? (float)(elapsed_ms > RR_STEP_DT_MAX ? RR_STEP_DT_MAX : elapsed_ms) / gain
            : 0.0f) + (float)age_ms;
        lead = fmaxf(0.0f, fminf(lead, 220.0f));
        const float s = lead * 0.001f;
        point->x += t->vel.x * s;
        point->z += t->vel.z * s;
        return 1;
    }
    return 0;
}

RR_AIM_PICK rr_aim_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                        int width, int height, const BR_Config* config,
                        uint64_t locked_addr, int32_t locked_id,
                        int (*clear)(RR_V3, RR_V3), unsigned now64,
                        unsigned elapsed_ms, unsigned age_ms) {
    RR_AIM_PICK best = {0}, fallback = {0};
    if (!players || count <= 0 || !camera || !camera->valid || !config ||
        width < 16 || height < 16) return best;
    if (count > RR_MAX_PLAYERS) count = RR_MAX_PLAYERS;
    (void)now64;

    const float cx = width * 0.5f, cy = height * 0.5f;
    const float radius = (float)(config->aim_fov > 0 ? config->aim_fov : 150);

    /* Release the lock when the holder is downed/dead, or when any living
     * upright candidate sits inside the acquisition ring. Without this the
     * identity filter below removes every competitor before scoring, the
     * downed fallback is returned, and the crosshair glues to a knocked body
     * for as long as the key is held. This was the reported bug. */
    if (locked_addr) {
        int drop = 0;
        for (int i = 0; i < count; ++i) {
            const RR_PLAYER* p = &players[i];
            if (!p->addr || !p->valid || p->local || p->is_vehicle || p->is_loot ||
                p->hidden_in_object) continue;
            if (p->addr == locked_addr && p->id == locked_id) {
                if (!aim_alive(p) || is_downed(p)) { drop = 1; break; }
                continue;
            }
            if (!aim_alive(p) || is_downed(p)) continue;
            RR_V3 pt; float sx, sy;
            if (!rr_aim_point(p, config->aa_bone, camera, width, height, &pt) ||
                !game_w2s(camera, &pt, width, height, &sx, &sy)) continue;
            const float ddx = sx - cx, ddy = sy - cy;
            if (ddx*ddx + ddy*ddy <= radius*radius) { drop = 1; break; }
        }
        if (drop) { locked_addr = 0; locked_id = 0; }
    }

    const float ring = locked_addr ? radius * 2.0f : radius;
    const float limit = ring * ring;
    const float max_dist = (float)config->aa_dist;
    const int group = config->aa_bone;
    const int want_clan   = config->aim_targets & RR_REL_CLAN;
    const int want_police = config->aim_targets & RR_REL_POLICE;
    const int want_party  = config->aim_targets & RR_REL_PARTY;
    float best_pri = INFINITY, best_d2 = INFINITY;
    float fb_pri = INFINITY, fb_d2 = INFINITY;

    for (int i = 0; i < count; ++i) {
        const RR_PLAYER* p = &players[i];
        if (locked_addr && (p->addr != locked_addr || p->id != locked_id)) continue;
        if (!p->addr || !p->valid || p->local || !aim_alive(p) ||
            p->is_vehicle || p->is_loot || p->hidden_in_object) continue;
        if (!rr_relationship_allowed(p->relationships, want_clan, want_police, want_party))
            continue;
        if (!isfinite(p->dist) || (max_dist > 0.0f && p->dist > max_dist)) continue;

        RR_V3 point; float sx, sy;
        if (!rr_aim_point(p, group, camera, width, height, &point) ||
            !game_w2s(camera, &point, width, height, &sx, &sy)) continue;
        predict_motion(p, config->aim_smooth, elapsed_ms, age_ms, &point);
        if (!game_w2s(camera, &point, width, height, &sx, &sy)) continue;
        float dx = sx - cx, dy = sy - cy, d2 = dx*dx + dy*dy;
        if (!isfinite(d2) || d2 > limit) continue;

        if (clear && !clear(camera->pos, point)) {
            if (!rr_aim_visible_point(p, group, camera, width, height, &point, clear) ||
                !game_w2s(camera, &point, width, height, &sx, &sy)) continue;
            dx = sx - cx; dy = sy - cy; d2 = dx*dx + dy*dy;
            if (!isfinite(d2) || d2 > limit) continue;
        }

        /* Class offset dominates any pixel distance inside the widest ring
         * (fov 300 -> ring 600 -> d2 360000): an upright target always wins. */
        const int downed = is_downed(p);
        const float pri = (downed ? 1.0e7f : 0.0f)
                        + (config->aa_closest_first ? p->dist : d2);
        if (downed) {
            if (pri < fb_pri || (pri == fb_pri && d2 < fb_d2)) {
                fallback = (RR_AIM_PICK){p, point, dx, dy};
                fb_pri = pri; fb_d2 = d2;
            }
        } else {
            if (pri < best_pri || (pri == best_pri && d2 < best_d2)) {
                best = (RR_AIM_PICK){p, point, dx, dy};
                best_pri = pri; best_d2 = d2;
            }
        }
        if (locked_addr) break;
    }
    return best.player ? best : fallback;
}

static int aim_axis(float error, float gain, float limit, float flick_cap,
                    float* carry, unsigned char* inside) {
    const float dz = *inside ? 0.50f : 0.35f;
    if (fabsf(error) < dz) { *carry = 0.0f; *inside = 1; return 0; }
    *inside = 0;
    if (error * *carry < 0.0f) *carry = 0.0f;
    const float need = fabsf(error) * 0.5f;
    if (need > limit) limit = need < flick_cap ? need : flick_cap;
    const float step = fmaxf(-limit, fminf(limit, error * gain + *carry));
    const int counts = (int)step;
    *carry = step - counts;
    return counts;
}

void rr_aim_step(RR_AIM_STEER* state, float dx, float dy, float fov, int height,
                 int smooth, int cal, unsigned elapsed_ms, int firing,
                 int* mx, int* my) {
    *mx = *my = 0;
    if (!state || !isfinite(dx) || !isfinite(dy) ||
        !(fov > 10.0f && fov < 170.0f) || height < 16) {
        if (state) *state = (RR_AIM_STEER){0};
        return;
    }
    const float projection = (1080.0f / height) *
                             (tanf(fov * RR_AIM_PI / 360.0f) / RR_TAN30);
    const float calib = (cal > 0 ? (float)cal : 100.0f) / 100.0f;
    const float scale = projection * calib;
    const float limit = fmaxf(8.0f, 32.0f * calib);
    const float flick_cap = 160.0f * calib;
    const float response = aim_response_ms(smooth);
    const float gain = aim_gain(response, elapsed_ms);
    const float vertical_gain = (firing && response > 0.0f)
        ? aim_gain(response * 0.8f, elapsed_ms) : gain;
    *mx = aim_axis(dx * scale, gain, limit, flick_cap, &state->carry_x, &state->inside_x);
    *my = aim_axis(dy * scale, vertical_gain, limit, flick_cap, &state->carry_y, &state->inside_y);
}

int rr_aim_bone_in_group(int group, int bone) {
    if (bone < 0 || bone >= RR_CHAM_BONES) return 0;
    switch (group) {
    case RR_AIM_HEAD:   return bone >= RR_RIG_M_HEAD_1_JNT && bone <= RR_RIG_M_TONGUE_3_JNT;
    case RR_AIM_NECK:   return bone == RR_RIG_M_NECK_JNT ||
                               bone == RR_RIG_L_ARM_0_JNT || bone == RR_RIG_R_ARM_0_JNT;
    case RR_AIM_CHEST:  return bone == RR_RIG_M_SPINE_2_JNT || bone == RR_RIG_M_SPINE_3_JNT;
    case RR_AIM_PELVIS: return bone == RR_RIG_M_SPINE_1_JNT ||
                               bone == RR_RIG_L_LEG_1_JNT || bone == RR_RIG_R_LEG_1_JNT;
    case RR_AIM_ARMS:   return (bone >= RR_RIG_L_ARM_0_JNT && bone <= RR_RIG_L_ARM_3_JNT) ||
                               (bone >= RR_RIG_R_ARM_0_JNT && bone <= RR_RIG_R_ARM_3_JNT) ||
                               bone == RR_RIG_L_ARMTWIST_JNT || bone == RR_RIG_R_ARMTWIST_JNT;
    case RR_AIM_LEGS:   return (bone >= RR_RIG_L_LEG_1_JNT && bone <= RR_RIG_L_LEG_3_JNT) ||
                               (bone >= RR_RIG_R_LEG_1_JNT && bone <= RR_RIG_R_LEG_3_JNT);
    case RR_AIM_FACE:   return bone >= RR_RIG_L_EYELIDUP_JNT && bone <= RR_RIG_M_TONGUE_3_JNT;
    case RR_AIM_HANDS:  return (bone >= RR_RIG_L_ARM_3_JNT && bone <= RR_RIG_L_THUMB_3_JNT) ||
                               (bone >= RR_RIG_R_ARM_3_JNT && bone <= RR_RIG_R_THUMB_3_JNT);
    case RR_AIM_FEET:   return (bone >= RR_RIG_L_LEG_3_JNT && bone <= RR_RIG_L_FOOT_2_JNT) ||
                               (bone >= RR_RIG_R_LEG_3_JNT && bone <= RR_RIG_R_FOOT_2_JNT);
    case RR_AIM_CLOSEST: return rr_rig_is_body_joint(bone);
    default: return 0;
    }
}

static int point_near_origin(RR_V3 point, RR_V3 origin) {
    const float dx = point.x - origin.x, dy = point.y - origin.y, dz = point.z - origin.z;
    return isfinite(dx) && isfinite(dy) && isfinite(dz) &&
           dx*dx + dy*dy + dz*dz <= 36.0f;
}

static int point_in_front(const RR_CAMERA* cam, RR_V3 point) {
    const float d = (point.x - cam->pos.x) * cam->fwd.x +
                    (point.y - cam->pos.y) * cam->fwd.y +
                    (point.z - cam->pos.z) * cam->fwd.z;
    return d > 0.05f;
}

static int full_rig_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                          int width, int height, RR_V3* out,
                          int (*clear)(RR_V3, RR_V3)) {
    if (!cam || !cam->valid || width < 1 || height < 1 || p->nbones != RR_CHAM_BONES)
        return 0;
    const float cx = width * 0.5f, cy = height * 0.5f;
    int found = 0;
    float best = INFINITY;
    for (int bone = 0; bone < RR_CHAM_BONES; ++bone) {
        if (!rr_aim_bone_in_group(group, bone)) continue;
        const RR_V3 point = {p->bone[bone][0], p->bone[bone][1], p->bone[bone][2]};
        if (!point_near_origin(point, p->pos) || !point_in_front(cam, point)) continue;
        float x, y;
        if (!game_w2s(cam, &point, width, height, &x, &y) ||
            !isfinite(x) || !isfinite(y)) continue;
        const float sx = x - cx, sy = y - cy;
        const float score = sx*sx + sy*sy;
        if (!(score < best) || (clear && !clear(cam->pos, point))) continue;
        *out = point; best = score; found = 1;
    }
    return found;
}

static int mesh_anchor(const RR_PLAYER* p, int index, RR_V3* out) {
    if (!p->cham_valid || index < 0 || index >= 12) return 0;
    const RR_CHAM_ANCHOR* a = &rr_cham_anchors[index];
    const float* m = p->cham_skin[a->bone];
    out->x = m[0]*a->p[0] + m[1]*a->p[1] + m[2]*a->p[2] + m[3];
    out->y = m[4]*a->p[0] + m[5]*a->p[1] + m[6]*a->p[2] + m[7];
    out->z = m[8]*a->p[0] + m[9]*a->p[1] + m[10]*a->p[2] + m[11];
    return point_near_origin(*out, p->pos);
}

static RR_V3 group_point(const RR_PLAYER* p, int group) {
    RR_V3 joint;
    if (mesh_anchor(p, group, &joint)) return joint;
    const int idx = p->bone_group[group];
    if (p->nbones > 0 && p->nbones <= RR_MAX_BONES && idx >= 0 && idx < p->nbones)
        return (RR_V3){p->bone[idx][0], p->bone[idx][1], p->bone[idx][2]};
    static const float standing[4] = {1.65f, 1.48f, 1.20f, 0.88f};
    static const float crouched[4] = {1.00f, 0.90f, 0.72f, 0.48f};
    RR_V3 v = p->pos;
    v.y += p->crouch == 1 ? crouched[group] : standing[group];
    return v;
}

static int limb_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                      int width, int height, RR_V3* out,
                      int (*clear)(RR_V3, RR_V3)) {
    const int start = group == RR_AIM_ARMS ? 4 : 8;
    const float cx = width * 0.5f, cy = height * 0.5f;
    int found = 0;
    float best = INFINITY;
    for (int side = 0; side < 2; ++side) {
        RR_V3 a, b;
        if (!mesh_anchor(p, start + side*2, &a) ||
            !mesh_anchor(p, start + side*2 + 1, &b)) continue;
        const RR_V3 mid = {(a.x+b.x)*0.5f, (a.y+b.y)*0.5f, (a.z+b.z)*0.5f};
        if (!point_in_front(cam, mid)) continue;
        float x, y;
        if (!game_w2s(cam, &mid, width, height, &x, &y)) continue;
        const float dx = x - cx, dy = y - cy;
        const float dist = dx*dx + dy*dy;
        if (dist < best && (!clear || clear(cam->pos, mid))) {
            *out = mid; best = dist; found = 1;
        }
    }
    return found;
}

int rr_aim_visible_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                         int width, int height, RR_V3* out,
                         int (*clear)(RR_V3, RR_V3)) {
    if (!p || !out || (clear && !cam)) return 0;
    if (group < 0 || group >= RR_AIM_GROUP_COUNT) group = RR_AIM_HEAD;
    if (p->full_rig) return full_rig_point(p, group, cam, width, height, out, clear);
    if (group >= RR_AIM_FACE) return 0;
    if (group >= RR_AIM_ARMS) return limb_point(p, group, cam, width, height, out, clear);
    if (group != RR_AIM_CLOSEST) {
        *out = group_point(p, group);
        return !clear || clear(cam->pos, *out);
    }
    int found = 0;
    float best = INFINITY;
    for (int i = 0; i < 6; ++i) {
        RR_V3 point; float x, y;
        if (i < 4) point = group_point(p, i);
        else if (!limb_point(p, i + 1, cam, width, height, &point, clear)) continue;
        if (!game_w2s(cam, &point, width, height, &x, &y)) continue;
        const float dx = x - width*0.5f, dy = y - height*0.5f;
        const float d = dx*dx + dy*dy;
        if (d < best && (i >= 4 || !clear || clear(cam->pos, point))) {
            *out = point; best = d; found = 1;
        }
    }
    return found;
}

int rr_aim_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                 int width, int height, RR_V3* out) {
    return rr_aim_visible_point(p, group, cam, width, height, out, NULL);
}

static volatile LONG g_aim_active;
static RR_AIM_STEER g_input_steer;
static RR_TRIGGER_STATE g_trigger_state;
static RR_RECOIL_STATE g_recoil_state;
static RR_ANTIAIM_STATE g_antiaim_state;
static ULONGLONG g_input_at;
static uint64_t g_aim_lock_addr;
static int32_t  g_aim_lock_id;
static ULONGLONG g_aim_lock_at;

static int aim_clear_path(RR_V3 start, RR_V3 end) {
    /* UNKNOWN means the geometry worker has not captured the scene yet.
     * Treating it as blocked killed every pick during load and scene changes. */
    return rr_los_check(start, end) != RR_LOS_BLOCKED;
}

RR_AIM_PICK rr_trigger_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                            int width, int height, const BR_Config* config,
                            int (*clear)(RR_V3, RR_V3)) {
    RR_AIM_PICK best = {0};
    if (!players || count <= 0 || !camera || !camera->valid || !config ||
        width < 16 || height < 16) return best;
    if (count > RR_MAX_PLAYERS) count = RR_MAX_PLAYERS;

    const float cx = width * 0.5f, cy = height * 0.5f;
    float tolerance = (float)(config->trigger_fov > 0 ? config->trigger_fov : 10);
    if (tolerance > 80.0f) tolerance = 80.0f;
    const float tol2 = tolerance * tolerance;
    float best_d2 = tol2, best_dist = INFINITY;

    const float max_dist = (float)config->aa_dist;
    const int want_clan   = config->trigger_targets & RR_REL_CLAN;
    const int want_police = config->trigger_targets & RR_REL_POLICE;
    const int want_party  = config->trigger_targets & RR_REL_PARTY;
    const int last_group  = config->trigger_head_only ? RR_AIM_HEAD : RR_AIM_PELVIS;

    for (int i = 0; i < count; ++i) {
        const RR_PLAYER* p = &players[i];
        /* A knocked body is never a trigger target. No fallback branch exists. */
        if (!p->addr || !p->valid || p->local || !aim_alive(p) || is_downed(p) ||
            p->hidden_in_object || p->is_vehicle || p->is_loot ||
            !isfinite(p->dist) || (max_dist > 0.0f && p->dist > max_dist) ||
            !rr_relationship_allowed(p->relationships, want_clan, want_police, want_party))
            continue;

        /* Any sampled joint inside the tolerance counts. Testing only the single
         * best-scoring joint went silent whenever the head sat behind cover. */
        int found = 0;
        RR_V3 bp = {0};
        float bsx = 0, bsy = 0, bj = INFINITY;
        for (int group = RR_AIM_HEAD; group <= last_group; ++group) {
            RR_V3 point; float sx, sy;
            if (!rr_aim_point(p, group, camera, width, height, &point) ||
                !game_w2s(camera, &point, width, height, &sx, &sy)) continue;
            const float dx = sx - cx, dy = sy - cy;
            const float d2 = dx*dx + dy*dy;
            if (!isfinite(d2) || d2 > tol2) continue;
            if (clear && !clear(camera->pos, point)) continue;
            if (d2 < bj) { bj = d2; bp = point; bsx = sx; bsy = sy; found = 1; }
        }
        if (!found) continue;

        const float dx = bsx - cx, dy = bsy - cy;
        const float d2 = dx*dx + dy*dy;
        if (d2 < best_d2 - 0.25f || (d2 <= best_d2 + 0.25f && p->dist < best_dist)) {
            best = (RR_AIM_PICK){p, bp, dx, dy};
            best_d2 = d2; best_dist = p->dist;
        }
    }
    return best;
}

int rr_trigger_ready(RR_TRIGGER_STATE* state, uint64_t addr, int32_t id,
                     unsigned now, int delay_ms) {
    if (!state) return 0;
    const unsigned delay = delay_ms < 0 ? 0u : delay_ms > 300 ? 300u : (unsigned)delay_ms;
    const unsigned grace = 150u;
    const unsigned interval = 16u;   /* cadence independent of reaction delay */

    if (!addr) {
        if (state->tracked && !state->lost_at) state->lost_at = now;
        if (state->tracked && now - state->lost_at < grace) return 0;
        memset(state, 0, sizeof(*state));
        return 0;
    }
    if (!state->tracked || state->addr != addr || state->id != id) {
        memset(state, 0, sizeof(*state));
        state->tracked = 1; state->addr = addr; state->id = id; state->acquired_at = now;
    }
    state->lost_at = 0;
    if (now - state->acquired_at < delay) return 0;
    if (state->fired && now - state->last_shot < interval) return 0;
    state->last_shot = now;
    state->fired = 1;
    return 1;
}

static void aim_reset_worker_state(void) {
    memset(&g_input_steer, 0, sizeof(g_input_steer));
    memset(&g_trigger_state, 0, sizeof(g_trigger_state));
    memset(g_track, 0, sizeof(g_track));
    g_track_count = 0;
    rr_recoil_reset(&g_recoil_state);
    rr_antiaim_reset(&g_antiaim_state);
    g_aim_lock_addr = 0;
    g_aim_lock_id = 0;
    g_aim_lock_at = 0;
    g_input_at = 0;
}

static void send_mouse_move(int dx, int dy) {
    INPUT move = {0};
    move.type = INPUT_MOUSE;
    move.mi.dwFlags = MOUSEEVENTF_MOVE;
    move.mi.dx = dx; move.mi.dy = dy;
    SendInput(1, &move, sizeof(move));
}

void rr_aim_update(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                   int width, int height) {
    InterlockedExchange(&g_aim_active, 0);
    BR_Config* config = br_get();
    if (!config || !players || count <= 0 || !camera || !camera->valid ||
        width < 16 || height < 16 || config->menu_open) {
        aim_reset_worker_state();
        return;
    }
    if (count > RR_MAX_PLAYERS) count = RR_MAX_PLAYERS;

    const ULONGLONG now64 = RR_AIM_NOW();
    const unsigned now = (unsigned)now64;
    unsigned elapsed = g_input_at ? (unsigned)(now64 - g_input_at) : 16u;
    if (!elapsed) elapsed = 1u;
    g_input_at = now64;

    /* Snapshot age from the publication stamp the reader wrote. Bounded so a
     * stale or unset stamp cannot produce an absurd lead. */
    unsigned age_ms = 0;
    for (int i = 0; i < count; ++i) {
        if (!players[i].valid || !players[i].snap_pub_at) continue;
        const unsigned a = now - players[i].snap_pub_at;
        if (a <= 120) { age_ms = a; break; }
    }

    const int held = (GetAsyncKeyState(config->aim_key) & 0x8000) != 0;
    const int firing = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

    int local_dead = 0;
    for (int i = 0; i < count; ++i)
        if (players[i].local) { local_dead = players[i].dead; break; }

    if (config->aim_enabled && held && !local_dead &&
        (!config->aim_require_fire || firing)) {
        /* 400 ms holds against crosshair jitter between two upright bodies but
         * cannot outrank a fresh living target: the picker also drops the lock
         * whenever a competitor enters the ring. */
        const int lock_fresh = g_aim_lock_id && (now64 - g_aim_lock_at) < 400;
        RR_AIM_PICK pick = rr_aim_pick(players, count, camera, width, height, config,
                                       lock_fresh ? g_aim_lock_addr : 0,
                                       lock_fresh ? g_aim_lock_id : 0,
                                       config->aa_los_on ? aim_clear_path : NULL,
                                       (unsigned)now64, elapsed, age_ms);
        if (!pick.player && lock_fresh)
            pick = rr_aim_pick(players, count, camera, width, height, config,
                               0, 0, config->aa_los_on ? aim_clear_path : NULL,
                               (unsigned)now64, elapsed, age_ms);
        if (pick.player) {
            g_aim_lock_addr = pick.player->addr;
            g_aim_lock_id = pick.player->id;
            g_aim_lock_at = now64;
            int mx, my;
            rr_aim_step(&g_input_steer, pick.dx, pick.dy, camera->fov, height,
                        config->aim_smooth, config->aa_aim_cal, elapsed, firing, &mx, &my);
            if (mx || my) {
                INPUT input = {0};
                input.type = INPUT_MOUSE;
                input.mi.dwFlags = MOUSEEVENTF_MOVE;
                input.mi.dx = mx; input.mi.dy = my;
                if (SendInput(1, &input, sizeof(input)) == 1)
                    InterlockedExchange(&g_aim_active, 1);
            }
        } else {
            g_aim_lock_addr = 0; g_aim_lock_id = 0;
            memset(&g_input_steer, 0, sizeof(g_input_steer));
        }
    } else {
        g_aim_lock_addr = 0; g_aim_lock_id = 0;
        memset(&g_input_steer, 0, sizeof(g_input_steer));
    }

    /* History AFTER selection, stamped with publication time. */
    for (int i = 0; i < count; ++i) {
        const RR_PLAYER* p = &players[i];
        if (p->valid && !p->local && !p->is_vehicle && !p->is_loot && p->snap_pub_at)
            track_update(p);
    }

    const int trigger_held = config->trigger_key
        ? (GetAsyncKeyState(config->trigger_key) & 0x8000) != 0 : held;
    RR_AIM_PICK trigger = {0};
    if (config->trigger_enabled && trigger_held && !local_dead)
        trigger = rr_trigger_pick(players, count, camera, width, height, config,
                                  config->trigger_los ? aim_clear_path : NULL);
    if (rr_trigger_ready(&g_trigger_state,
                         trigger.player ? trigger.player->addr : 0,
                         trigger.player ? trigger.player->id : 0,
                         now, config->trigger_delay)) {
        INPUT click[2] = {0};
        click[0].type = click[1].type = INPUT_MOUSE;
        click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        if (SendInput(2, click, sizeof(INPUT)) == 1)
            SendInput(1, &click[1], sizeof(INPUT));
    }

    const int aim_active = InterlockedCompareExchange(&g_aim_active, 0, 0) != 0;
    if (config->norecoil_enabled) {
        int weapon = RR_WCLASS_OTHER;
        for (int i = 0; i < count; ++i)
            if (players[i].local) { weapon = rr_recoil_class(players[i].weapon); break; }
        int raw_x = 0, raw_y = 0;
        rr_raw_mouse_delta(&raw_x, &raw_y);
        int rmx = 0, rmy = 0;
        if (rr_recoil_tick(&g_recoil_state, config->norecoil_mode, config->norecoil_strength,
                           config->norecoil_horizontal, firing, weapon, &camera->fwd,
                           camera->fov, height, config->aa_aim_cal, raw_x, raw_y,
                           now, &rmx, &rmy))
            send_mouse_move(rmx, rmy);
    } else {
        rr_recoil_reset(&g_recoil_state);
    }

    const int antiaim_held = config->antiaim_key
        ? (GetAsyncKeyState(config->antiaim_key) & 0x8000) != 0 : 1;
    if (config->antiaim_enabled && antiaim_held && !firing && !aim_active) {
        int amx = 0, amy = 0;
        rr_antiaim_tick(&g_antiaim_state, config->antiaim_mode, config->antiaim_speed,
                        config->antiaim_range, now, &amx, &amy);
        if (amx || amy) send_mouse_move(amx, amy);
    } else {
        rr_antiaim_reset(&g_antiaim_state);
    }
}

int rr_aim_is_active(void) {
    return InterlockedCompareExchange(&g_aim_active, 0, 0) != 0;
}