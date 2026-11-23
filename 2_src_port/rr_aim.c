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

/* Worker-period clamp: covers stalls down to 20 fps without letting a long
   pause produce a single huge correction. Verified: at 33 ms the old 24 ms
   clamp under-delivered gain (0.646 vs 0.761 configured). */
#define RR_STEP_DT_MAX 50u

/* Millisecond clock for elapsed-time maths. The default is the 64-bit tick so
 * a 49.7-day session cannot wrap the follow filter; the offline harness
 * substitutes a deterministic clock through this macro. */
#ifndef RR_AIM_NOW
#define RR_AIM_NOW() GetTickCount64()
#endif

/* A body keeps its roster entry and its replicated transform for a while, so it
 * still projects to a valid aim point. Health is the signal that survives that
 * window: a known zero is a corpse, and the death flag is a server event that
 * can lag the health field by a tick or two. A failed read is -1 and stays a
 * candidate, otherwise one bad pointer would empty the target list. */
static int aim_alive(const RR_PLAYER* p) {
    return !p->dead && p->health != 0;
}

RR_AIM_PICK rr_aim_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                        int width, int height, const BR_Config* config,
                        uint64_t locked_addr, int32_t locked_id,
                        int (*clear)(RR_V3, RR_V3)) {
    RR_AIM_PICK best = {0};
    if (!players || count <= 0 || !camera || !camera->valid || !config ||
        width < 16 || height < 16) return best;
    if (count > RR_MAX_PLAYERS) count = RR_MAX_PLAYERS;

    float best_priority = INFINITY, best_d2 = INFINITY;
    const float cx = width * 0.5f, cy = height * 0.5f;
    const float radius = (float)(config->aim_fov > 0 ? config->aim_fov : 150);
    /* A held lock widens the ring instead of removing it: a target that leaves
     * the screen is dropped and re-acquired on the next frame rather than
     * dragging the view after it. */
    const float ring = locked_addr ? radius * 2.0f : radius;
    const float limit = ring * ring;
    const float max_dist = (float)config->aa_dist;
    const int group = config->aa_bone;
    const int want_clan   = config->aim_targets & RR_REL_CLAN;
    const int want_police = config->aim_targets & RR_REL_POLICE;
    const int want_party  = config->aim_targets & RR_REL_PARTY;

    for (int i = 0; i < count; ++i) {
        const RR_PLAYER* p = &players[i];
        /* Cheapest discriminators first: identity, then flags, then floats. */
        if (locked_addr && (p->addr != locked_addr || p->id != locked_id)) continue;
        if (!p->addr || !p->valid || p->local || !aim_alive(p) ||
            p->is_vehicle || p->is_loot || p->hidden_in_object) continue;
        if (!rr_relationship_allowed(p->relationships, want_clan, want_police, want_party))
            continue;
        if (!isfinite(p->dist) || (max_dist > 0.0f && p->dist > max_dist)) continue;

        RR_V3 point;
        float sx, sy;
        if (!rr_aim_point(p, group, camera, width, height, &point) ||
            !game_w2s(camera, &point, width, height, &sx, &sy)) continue;
        float dx = sx - cx, dy = sy - cy;
        float d2 = dx * dx + dy * dy;
        if (!isfinite(d2) || d2 > limit) continue;

        /* Keep acquisition outside the FOV cheap. If the nearest member is
         * blocked, search the same area for another visible joint. */
        if (clear && !clear(camera->pos, point)) {
            if (!rr_aim_visible_point(p, group, camera, width, height, &point, clear) ||
                !game_w2s(camera, &point, width, height, &sx, &sy)) continue;
            dx = sx - cx; dy = sy - cy; d2 = dx * dx + dy * dy;
            if (!isfinite(d2) || d2 > limit) continue;
        }

        const float priority = config->aa_closest_first ? p->dist : d2;
        if (priority < best_priority || (priority == best_priority && d2 < best_d2)) {
            best = (RR_AIM_PICK){p, point, dx, dy};
            best_priority = priority;
            best_d2 = d2;
        }
        if (locked_addr) break;
    }
    return best;
}

/* Time constant of the exponential follow, in ms, for a 16 ms reference frame.
   smooth is the fraction of the error removed per reference frame. */
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
    if (!out || sample_ms < 8 || sample_ms > 120 || age_ms > 80) return 0;
    const float vx = (position.x - previous.x) * 1000.0f / sample_ms;
    const float vz = (position.z - previous.z) * 1000.0f / sample_ms;
    if (!isfinite(vx) || !isfinite(vz)) return 0;
    const float speed = hypotf(vx, vz);
    if (!(speed >= 0.2f && speed <= 20.0f)) return 0;

    /* The worker corrects discretely: per frame it removes gain*error, so a
     * constant-velocity target trails by v*(dt/gain - age) unless the aim
     * point is shifted by exactly that amount. Signed: when the snapshot age
     * already exceeds the follow lag, aim slightly behind the stale point.
     * Bounds: at most 200 ms ahead (direction-change error cap) and never
     * further back than the snapshot age itself.
     * Verified in simulation: zero steady-state residual for smooth 10..99
     * at worker periods 8/16/33 ms. */
    const float gain = aim_gain(aim_response_ms(smooth), elapsed_ms);
    float lead_ms = (gain > 0.0f ? (float)(elapsed_ms > RR_STEP_DT_MAX ?
                                           RR_STEP_DT_MAX : elapsed_ms) / gain : 0.0f)
                    - (float)age_ms;
    lead_ms = fmaxf(-(float)age_ms, fminf(lead_ms, 200.0f));
    const float seconds = lead_ms * 0.001f;
    *out = (RR_V3){ point.x + vx * seconds, point.y, point.z + vz * seconds };
    return 1;
}

/* One axis of bounded steering. Hysteresis: enter the deadzone below 0.35
 * reference-scaled units, leave it only above 0.50, so sensor noise at the
 * boundary cannot chatter the output between 0 and 1 count. */
static int aim_axis(float error, float gain, float limit, float flick_cap,
                    float* carry, unsigned char* inside) {
    const float dz = *inside ? 0.50f : 0.35f;
    if (fabsf(error) < dz) {
        *carry = 0.0f;
        *inside = 1;
        return 0;
    }
    *inside = 0;
    if (error * *carry < 0.0f) *carry = 0.0f;
    /* A large correction must not be rate-limited by the smoothing limit, or
     * the first frames after a flick stay behind a fast target. The flick cap
     * is scaled by calibration so it bounds the same angular jump at any
     * sensitivity. */
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

    /* Project to the equivalent error at 1080p / 60 degrees, then apply the
     * sensitivity calibration. Both factors are required: dropping either
     * makes the correction depend on resolution or zoom. Verified: identical
     * count sequences for equal angular errors across resolutions/FOVs. */
    const float projection = (1080.0f / height) *
                             (tanf(fov * RR_AIM_PI / 360.0f) / RR_TAN30);
    const float calib = (cal > 0 ? (float)cal : 100.0f) / 100.0f;
    const float scale = projection * calib;

    /* Per-step limits represent similar angular corrections at any sens. */
    const float limit = fmaxf(8.0f, 32.0f * calib);
    const float flick_cap = 160.0f * calib;

    const float response = aim_response_ms(smooth);
    const float gain = aim_gain(response, elapsed_ms);
    const float vertical_gain = (firing && response > 0.0f)
        ? aim_gain(response * 0.8f, elapsed_ms) : gain;

    *mx = aim_axis(dx * scale, gain, limit, flick_cap,
                   &state->carry_x, &state->inside_x);
    *my = aim_axis(dy * scale, vertical_gain, limit, flick_cap,
                   &state->carry_y, &state->inside_y);
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

/* Shared sanity gate for a candidate world point: finite and within 6 m of
 * the character origin (squared bound 36). */
static int point_near_origin(RR_V3 point, RR_V3 origin) {
    const float dx = point.x - origin.x, dy = point.y - origin.y, dz = point.z - origin.z;
    return isfinite(dx) && isfinite(dy) && isfinite(dz) &&
           dx * dx + dy * dy + dz * dz <= 36.0f;
}

/* Cheap behind-camera rejection before the full projection. Sign is valid
 * for an unnormalized forward vector. */
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
        const float score = sx * sx + sy * sy;
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
    if (p->nbones > 0 && p->nbones <= RR_MAX_BONES && idx >= 0 && idx < p->nbones) {
        return (RR_V3){p->bone[idx][0], p->bone[idx][1], p->bone[idx][2]};
    }
    /* When the game has not created the skeleton, estimate from the character
     * origin. These are position estimates, not measured bone transforms. */
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
        if (!mesh_anchor(p, start + side * 2, &a) ||
            !mesh_anchor(p, start + side * 2 + 1, &b)) continue;
        const RR_V3 mid = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};
        if (!point_in_front(cam, mid)) continue;
        float x, y;
        if (!game_w2s(cam, &mid, width, height, &x, &y)) continue;
        const float dx = x - cx, dy = y - cy;
        const float dist = dx * dx + dy * dy;
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
        RR_V3 point;
        float x, y;
        if (i < 4) point = group_point(p, i);
        else if (!limb_point(p, i + 1, cam, width, height, &point, clear)) continue;
        if (!game_w2s(cam, &point, width, height, &x, &y)) continue;
        const float dx = x - width * 0.5f, dy = y - height * 0.5f;
        const float d = dx * dx + dy * dy;
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

/* Input and trigger state belongs to the single input worker. */
static volatile LONG g_aim_active;
static RR_AIM_STEER g_input_steer;
static RR_TRIGGER_STATE g_trigger_state;
static RR_RECOIL_STATE g_recoil_state;
static RR_ANTIAIM_STATE g_antiaim_state;
static ULONGLONG g_input_at;
/* Sticky aim lock: the player the steering is currently committed to, with the
 * tick it was last confirmed on. Re-picking every frame makes the crosshair
 * jump between two bodies that straddle the same screen distance. */
static uint64_t g_aim_lock_addr;
static int32_t g_aim_lock_id;
static ULONGLONG g_aim_lock_at;

static int aim_clear_path(RR_V3 start, RR_V3 end) {
    return rr_los_check(start, end) == RR_LOS_CLEAR;
}

RR_AIM_PICK rr_trigger_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                            int width, int height, const BR_Config* config,
                            int (*clear)(RR_V3, RR_V3)) {
    RR_AIM_PICK best = {0};
    if (!players || count <= 0 || !camera || !camera->valid || !config ||
        width < 16 || height < 16)
        return best;
    if (count > RR_MAX_PLAYERS) count = RR_MAX_PLAYERS;

    /* The crosshair is a pixel, not a ray. Comparing projections keeps the test
     * the same at every range: a joint centre twelve pixels off the crosshair is
     * a hit whether the target stands at 5 m or 90 m, which is what the player
     * sees and what the weapon actually converges on. The old metre-radius test
     * on bare joint centres needed a near-perfect pixel hit and went dead as
     * soon as the crosshair sat on an arm or a shoulder. */
    const float cx = width * 0.5f, cy = height * 0.5f;
    float tolerance = (float)(config->trigger_fov > 0 ? config->trigger_fov : 10);
    if (tolerance > 80.0f) tolerance = 80.0f;
    float best_d2 = tolerance * tolerance;
    float best_dist = INFINITY;

    const float max_dist = (float)config->aa_dist;
    const int want_clan   = config->trigger_targets & RR_REL_CLAN;
    const int want_police = config->trigger_targets & RR_REL_POLICE;
    const int want_party  = config->trigger_targets & RR_REL_PARTY;
    const int last_group  = config->trigger_head_only ? RR_AIM_HEAD : RR_AIM_PELVIS;

    for (int i = 0; i < count; ++i) {
        const RR_PLAYER* p = &players[i];
        if (!p->addr || !p->valid || p->local || !aim_alive(p) || p->hidden_in_object ||
            p->is_vehicle || p->is_loot ||
            !isfinite(p->dist) || (max_dist > 0.0f && p->dist > max_dist) ||
            !rr_relationship_allowed(p->relationships, want_clan, want_police, want_party))
            continue;
        for (int group = RR_AIM_HEAD; group <= last_group; ++group) {
            RR_V3 point;
            float sx, sy;
            if (!rr_aim_point(p, group, camera, width, height, &point) ||
                !game_w2s(camera, &point, width, height, &sx, &sy)) continue;
            const float dx = sx - cx, dy = sy - cy;
            const float d2 = dx * dx + dy * dy;
            if (!isfinite(d2) || d2 > best_d2) continue;
            /* Only the point that could actually be fired at pays for a
             * geometry trace. */
            if (clear && !clear(camera->pos, point)) continue;
            /* Closest to the crosshair wins; a draw goes to the nearer body so
             * two overlapping players resolve to the one in front. */
            if (d2 < best_d2 - 0.25f || (d2 <= best_d2 + 0.25f && p->dist < best_dist)) {
                best = (RR_AIM_PICK){p, point, dx, dy};
                best_d2 = d2;
                best_dist = p->dist;
            }
        }
    }
    return best;
}

int rr_trigger_ready(RR_TRIGGER_STATE* state, uint64_t addr, int32_t id,
                     unsigned now, int delay_ms) {
    if (!state) return 0;
    if (!addr) { memset(state, 0, sizeof(*state)); return 0; }
    const unsigned delay = delay_ms < 0 ? 0u : delay_ms > 300 ? 300u : (unsigned)delay_ms;
    if (!state->tracked || state->addr != addr || state->id != id) {
        memset(state, 0, sizeof(*state));
        state->tracked = 1; state->addr = addr; state->id = id; state->acquired_at = now;
    }
    /* Unsigned subtraction stays correct across the 32-bit tick wrap. */
    if (now - state->acquired_at < delay ||
        (state->fired && now - state->last_shot < (delay > 16 ? delay : 16))) return 0;
    state->last_shot = now;
    state->fired = 1;
    return 1;
}

static void aim_reset_worker_state(void) {
    memset(&g_input_steer, 0, sizeof(g_input_steer));
    memset(&g_trigger_state, 0, sizeof(g_trigger_state));
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
    move.mi.dx = dx;
    move.mi.dy = dy;
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

    /* 64-bit tick: no 49.7-day wrap in the worker clock. Trigger timestamps
     * stay 32-bit; their wrap-safe subtraction is verified. */
    const ULONGLONG now64 = RR_AIM_NOW();
    const unsigned now = (unsigned)now64;
    /* Two updates inside the same millisecond must not zero the follow gain:
     * a zero-length step would stall steering until the next tick. */
    unsigned elapsed = g_input_at ? (unsigned)(now64 - g_input_at) : 16u;
    if (!elapsed) elapsed = 1u;
    g_input_at = now64;

    const int held = (GetAsyncKeyState(config->aim_key) & 0x8000) != 0;
    const int firing = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

    /* A dead local player gets the death camera and has no weapon: steering and
     * clicking there only fights the spectate view. */
    int local_dead = 0;
    for (int i = 0; i < count; ++i)
        if (players[i].local) { local_dead = players[i].dead; break; }

    if (config->aim_enabled && held && !local_dead &&
        (!config->aim_require_fire || firing)) {
        const int lock_fresh = g_aim_lock_id && (now64 - g_aim_lock_at) < 500;
        RR_AIM_PICK pick = rr_aim_pick(players, count, camera, width, height, config,
                                       lock_fresh ? g_aim_lock_addr : 0,
                                       lock_fresh ? g_aim_lock_id : 0,
                                       config->aa_los_on ? aim_clear_path : NULL);
        /* The held target died, left the widened ring or lost its sight line:
         * acquire again in the same frame instead of idling. */
        if (!pick.player && lock_fresh)
            pick = rr_aim_pick(players, count, camera, width, height, config,
                               0, 0, config->aa_los_on ? aim_clear_path : NULL);
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
            g_aim_lock_addr = 0;
            g_aim_lock_id = 0;
            memset(&g_input_steer, 0, sizeof(g_input_steer));
        }
    } else {
        g_aim_lock_addr = 0;
        g_aim_lock_id = 0;
        memset(&g_input_steer, 0, sizeof(g_input_steer));
    }

    /* Key zero follows the aim hold key, even when aim steering is disabled. */
    const int trigger_held = config->trigger_key
        ? (GetAsyncKeyState(config->trigger_key) & 0x8000) != 0 : held;
    RR_AIM_PICK trigger = {0};
    if (config->trigger_enabled && trigger_held && !firing && !local_dead)
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
        /* A partial insertion must not leave the synthetic left button held. */
        if (SendInput(2, click, sizeof(INPUT)) == 1)
            SendInput(1, &click[1], sizeof(INPUT));
    }

    /* Recoil control and view perturbation share the worker but not the aim
     * hold key. Recoil follows the fire button; anti-aim stays out of the way
     * while the aimbot is steering or a shot is being fired. */
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