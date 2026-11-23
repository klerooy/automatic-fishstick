/* rr_antiaim.c - Bounded view perturbation modes.
 *
 * spin   : constant horizontal drift, direction fixed until reset
 * jitter : alternates between -range and +range
 * static : applies +range once, then holds
 * random : random walk between targets inside the range
 */
#include "rr_antiaim.h"
#include <string.h>

void rr_antiaim_reset(RR_ANTIAIM_STATE* state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

static int antiaim_clamp(int v, int limit) {
    if (v > limit) return limit;
    if (v < -limit) return -limit;
    return v;
}

void rr_antiaim_tick(RR_ANTIAIM_STATE* state, int mode, int speed, int range,
                     unsigned now, int* mx, int* my) {
    if (!mx || !my) return;
    *mx = *my = 0;
    if (!state) return;

    if (range < 1) range = 1;
    if (range > 60) range = 60;
    if (speed < 1) speed = 1;
    if (speed > 100) speed = 100;

    unsigned dt = state->last_at ? now - state->last_at : 16;
    if (dt > 100) dt = 100;
    if (!dt) dt = 1;
    state->last_at = now;
    state->seed = state->seed * 1664525u + 1013904223u;

    if (mode == RR_ANTIAIM_SPIN) {
        float rate = (float)range * (0.2f + 0.8f * (float)speed / 100.0f) * (float)dt / 16.0f;
        if (state->dir < 0) rate = -rate;
        state->carry_x += rate;
        int step = (int)state->carry_x;
        if (step > range) step = range;
        if (step < -range) step = -range;
        state->carry_x -= (float)step;
        *mx = step;
        state->current_x = 0;
        state->primed = 0;
        return;
    }

    if (mode == RR_ANTIAIM_STATIC) {
        if (!state->primed) {
            state->primed = 1;
            *mx = range;
            state->current_x = range;
        }
        return;
    }

    if (mode == RR_ANTIAIM_JITTER) {
        unsigned interval = 520u - (unsigned)(speed * 4u);
        if (!state->next_at || now >= state->next_at) {
            state->next_at = now + interval;
            state->dir = state->dir >= 0 ? -1 : 1;
            int target = state->dir * range;
            *mx = antiaim_clamp(target - state->current_x, 2 * range);
            state->current_x = target;
        }
        return;
    }

    /* Random walk: pick a target inside the range and move to it. */
    unsigned interval = 420u - (unsigned)(speed * 3u);
    if (!state->next_at || now >= state->next_at) {
        state->next_at = now + interval;
        unsigned r = state->seed;
        int tx = (int)(r % (unsigned)(2 * range + 1)) - range;
        r = r * 1103515245u + 12345u;
        int ty = (int)(r % (unsigned)(2 * range + 1)) - range;
        ty /= 2;
        *mx = antiaim_clamp(tx - state->current_x, 2 * range);
        *my = antiaim_clamp(ty - state->current_y, 2 * range);
        state->current_x = tx;
        state->current_y = ty;
    }
}
