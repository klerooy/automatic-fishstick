/* rr_antiaim.h - View perturbation for the input worker.
 *
 * This is a client-side view scrambler, not a server-side desync: it moves the
 * mouse so the local view sweeps, jitters or snaps. It cannot change what the
 * server believes the character is facing. Modes are bounded per frame so a
 * stalled worker cannot fling the view.
 */
#ifndef RR_ANTIAIM_H
#define RR_ANTIAIM_H

#ifdef __cplusplus
extern "C" {
#endif

enum { RR_ANTIAIM_SPIN = 0, RR_ANTIAIM_JITTER = 1, RR_ANTIAIM_STATIC = 2,
       RR_ANTIAIM_RANDOM = 3 };

typedef struct {
    unsigned last_at;      /* previous tick                              */
    unsigned next_at;      /* next scheduled change                      */
    unsigned seed;         /* random-mode generator state                */
    int      dir;          /* spin direction / jitter side               */
    int      primed;       /* static offset already applied              */
    int      current_x;    /* held horizontal offset                     */
    int      current_y;    /* held vertical offset                       */
    float    carry_x;      /* fractional spin counts between ticks       */
} RR_ANTIAIM_STATE;

void rr_antiaim_reset(RR_ANTIAIM_STATE* state);
void rr_antiaim_tick(RR_ANTIAIM_STATE* state, int mode, int speed, int range,
                     unsigned now, int* mx, int* my);

#ifdef __cplusplus
}
#endif
#endif
