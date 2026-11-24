/*
 * producers.h — the producer side on the master (M1: "multiple client threads
 * submit compute tasks to a shared task pool") and the fractal frame store.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "job_queue.h"

void    producers_start(nx_job_queue_t *gq);
void    producers_set_clients(uint8_t n);     /* 0..4 client tasks active        */
uint8_t producers_clients(void);
uint32_t producers_blocked(void);             /* times a producer waited for space */
void    producers_storm(uint32_t ms);         /* burst of P3 jobs (starvation test) */
bool    producers_runaway(void);              /* a job that never finishes (A2)    */
bool    producers_zoom_at(float fx, float fy);/* new frame centred at a click (0..1) */
void    producers_set_auto(bool on);
void    producers_pause_frames(bool paused);
/* Submit one whole frame from the calling task (it blocks like any producer).
 * Gives up at the deadline or when stopped. Returns 0 if incomplete. */
uint16_t producers_frame_now(float cx, float cy, float w, const atomic_bool *stop, int64_t deadline_us);
bool    producers_auto(void);

/* ---- fractal frame store: committed tiles are copied into a 600 x 400 image ---- */
typedef struct {
    uint16_t frame, done, total;
    uint16_t max_iter;
    float    zoom;
    uint32_t version;                 /* increases with every committed tile */
    uint32_t elapsed_ms;              /* so far, or total when done          */
    uint32_t crc;                     /* CRC32 of the finished image, 0 before */
    uint8_t  owner[NX_TILES];         /* 0 = not yet, else node + 1          */
} nx_frame_view_t;

void frame_on_tile(const nx_job_spec_t *spec, const uint8_t *pixels, uint8_t node);
void frame_view(nx_frame_view_t *v);
/* Binary list of tiles committed after version `since`: header {u16 frame,
 * u32 page watermark, u16 count} then count x {u16 tile, u8 owner, u8 pad, 1600 px}. */
int  frame_tiles_since(uint32_t since, uint8_t *buf, int cap);
