/* rr_aim.h - Target-region selection and bounded mouse-steering math. */
#ifndef RR_AIM_H
#define RR_AIM_H
#include "rr_game.h"

void rr_aim_update(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                   int width, int height);
int rr_aim_is_active(void);

enum { RR_AIM_HEAD, RR_AIM_NECK, RR_AIM_CHEST, RR_AIM_PELVIS,
       RR_AIM_CLOSEST, RR_AIM_ARMS, RR_AIM_LEGS, RR_AIM_FACE,
       RR_AIM_HANDS, RR_AIM_FEET, RR_AIM_GROUP_COUNT };

int rr_aim_bone_in_group(int group, int bone);
int rr_aim_point(const RR_PLAYER* player, int group, const RR_CAMERA* camera,
                 int width, int height, RR_V3* out);
int rr_aim_visible_point(const RR_PLAYER* player, int group, const RR_CAMERA* camera,
                         int width, int height, RR_V3* out,
                         int (*clear)(RR_V3, RR_V3));

struct BR_Config;
typedef struct {
    const RR_PLAYER* player;
    RR_V3 point;
    float dx, dy;
} RR_AIM_PICK;

RR_AIM_PICK rr_aim_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                        int width, int height, const struct BR_Config* config,
                        uint64_t locked_addr, int32_t locked_id,
                        int (*clear)(RR_V3, RR_V3), unsigned now64,
                        unsigned elapsed_ms, unsigned age_ms);

typedef struct {
    float carry_x, carry_y;
    unsigned char inside_x, inside_y;
} RR_AIM_STEER;

typedef struct {
    uint64_t addr;
    int32_t id;
    unsigned acquired_at, last_shot, lost_at;
    int tracked, fired;
} RR_TRIGGER_STATE;

RR_AIM_PICK rr_trigger_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                            int width, int height, const struct BR_Config* config,
                            int (*clear)(RR_V3, RR_V3));
int rr_trigger_ready(RR_TRIGGER_STATE* state, uint64_t addr, int32_t id,
                     unsigned now, int delay_ms);

void rr_aim_step(RR_AIM_STEER* state, float dx, float dy, float fov, int height,
                 int smooth, int cal, unsigned elapsed_ms, int firing,
                 int* mx, int* my);

int rr_aim_motion_point(RR_V3 point, RR_V3 position, RR_V3 previous,
                        unsigned sample_ms, unsigned age_ms, int smooth,
                        unsigned elapsed_ms, RR_V3* out);

#endif