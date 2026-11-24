/*
 * nexus_types.h — the job contract shared by master and workers.
 *
 * A job carries parameters and a seed, never bulk input data: the worker
 * generates its input itself, so DISPATCH messages stay a few dozen bytes.
 * Structures that travel between boards are packed so both sides agree on
 * the byte layout.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    NX_JOB_FRACTAL = 1,   /* one 40x40 Mandelbrot tile                 */
    NX_JOB_MATMUL  = 2,   /* n x n integer matrix product              */
    NX_JOB_SPIN    = 3,   /* busy work for an exact time (synthetic)   */
} nx_job_type_t;

typedef enum {
    NX_JS_QUEUED = 1,     /* waiting in a ready queue                                  */
    NX_JS_DISPATCHED,     /* sent to a node, no ACK yet                                */
    NX_JS_RUNNING,        /* the node accepted it                                      */
    NX_JS_DONE,           /* result committed (exactly once)                           */
    NX_JS_FAILED,         /* gave up after NX_MAX_ATTEMPTS                             */
    NX_JS_CANCELLED,      /* still in the buffer but no longer wanted (lazy removal)   */
} nx_job_state_t;

typedef enum { NX_RES_OK = 0, NX_RES_TIMEOUT = 1, NX_RES_ERROR = 2 } nx_result_status_t;

typedef enum { NX_ORDER_FCFS = 0, NX_ORDER_PRIORITY = 1, NX_ORDER_AGING = 2 } nx_ordering_t;
typedef enum { NX_PLACE_ROUND_ROBIN = 0, NX_PLACE_LEAST_LOADED = 1 } nx_placement_t;

typedef struct __attribute__((packed)) {
    float    x0, y0;      /* complex coordinate of the tile's top-left pixel */
    float    step;        /* complex units per pixel                          */
    uint16_t max_iter;
    uint16_t tx, ty;      /* tile position in the image                       */
    uint16_t frame;       /* frame number (old frames are ignored)            */
} nx_fractal_params_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint16_t attempt;     /* 1, 2, 3 ...                                      */
    uint8_t  type;        /* nx_job_type_t                                    */
    uint8_t  prio;        /* base priority 0 (low) .. 3 (high)                */
    uint32_t budget_ms;   /* execution budget enforced on the worker (A2)     */
    union {
        nx_fractal_params_t fractal;
        struct __attribute__((packed)) { uint16_t n; uint32_t seed; } matmul;
        struct __attribute__((packed)) { uint32_t duration_ms; } spin;
    } p;
} nx_job_spec_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint16_t attempt;
    uint8_t  status;      /* nx_result_status_t                               */
    uint8_t  core;        /* core the executor started on                     */
    uint32_t exec_us;     /* execution time measured on the worker            */
    uint32_t checksum;    /* CRC32 of the output                              */
    uint16_t data_len;    /* tile bytes that follow (0 for other job types)   */
} nx_result_t;
