/*
 * nexus_config.h — every tunable of NEXUS in one place.
 *
 * Values are starting points; the experiments in the report measure their effect.
 * FreeRTOS task priorities: a larger number is more urgent. ESP-IDF's own tasks
 * use Wi-Fi = 23 (core 0) and lwIP TCP/IP = 18, so all NEXUS tasks stay below 18.
 */
#pragma once

/* ---- job queues (bounded buffers, OSC §7.1.1) ---- */
#define NX_QUEUE_CAPACITY      256   /* master: QUEUED jobs waiting to be dispatched        */
#define NX_INFLIGHT_CAPACITY   32    /* master: jobs dispatched but not yet committed        */
#define NX_LOCAL_QUEUE_CAP     8     /* worker / local engine: holds at most K + 1 jobs      */

/* ---- thread pool and execution slots (OSC §4.5.1, §6.6) ---- */
#define NX_POOL_SIZE           4     /* N: executor tasks per board, created once at boot    */
#define NX_DEFAULT_SLOTS       2     /* K: execution permits at boot (1..N, changeable live) */

/* ---- scheduling (OSC §5.3.4) ---- */
#define NX_PRIO_MAX            3     /* priorities P0 (low) .. P3 (high)                     */
#define NX_AGING_STEP_MS       1500  /* +1 effective priority per 1.5 s of waiting           */

/* ---- timeouts and failure detection (OSC §19.5.1) ---- */
#define NX_MAX_ATTEMPTS        3     /* after the 3rd failed attempt a job is FAILED         */
#define NX_HEARTBEAT_MS        500   /* worker -> master progress message period             */
#define NX_SUSPECT_MS          1500  /* silence before a worker is SUSPECT                   */
#define NX_OFFLINE_MS          3000  /* silence before a worker is OFFLINE (jobs requeued)  */
#define NX_LEASE_MARGIN_MS     2000  /* lease = execution budget + this margin               */
#define NX_SUPERVISOR_TICK_MS  100

/* ---- cluster and message passing ---- */
#define NX_MAX_WORKERS         3
#define NX_NODE_LOCAL          0     /* node index 0 = master's own engine (single-node mode) */
#define NX_NODE_NONE           0xFF
#define NX_LINK_PORT           5005
#define NX_MAX_FRAME           2048  /* largest message payload (a RESULT with one tile)    */

/* ---- FreeRTOS task priorities ---- */
#define NX_PRIO_SUPERVISOR     11
#define NX_PRIO_LINK           10
#define NX_PRIO_DISPATCH       8
#define NX_PRIO_TELEMETRY      6
#define NX_PRIO_EXEC           5
#define NX_PRIO_CLIENT         4
#define NX_PRIO_MONITOR        4
#define NX_PRIO_LOGGER         3

/* ---- task stack sizes (bytes) ---- */
#define NX_STACK_EXEC          3072
#define NX_STACK_LINK          4096
#define NX_STACK_SMALL         3072

/* ---- fractal workload: 600 x 400 image, 40 x 40 tiles -> 150 jobs per frame ---- */
#define NX_IMG_W               600
#define NX_IMG_H               400
#define NX_TILE                40
#define NX_TILES_X             (NX_IMG_W / NX_TILE)
#define NX_TILES_Y             (NX_IMG_H / NX_TILE)
#define NX_TILES               (NX_TILES_X * NX_TILES_Y)
#define NX_TILE_BYTES          (NX_TILE * NX_TILE)

/* ---- other workloads ---- */
#define NX_MATMUL_MAX_N        32    /* n x n matrices, inputs are 0..99                     */
#define NX_MATMUL_SCRATCH     (2 * NX_MATMUL_MAX_N * NX_MATMUL_MAX_N + NX_MATMUL_MAX_N * 4)
#define NX_SCRATCH_BYTES      ((NX_MATMUL_SCRATCH > NX_TILE_BYTES) ? NX_MATMUL_SCRATCH : NX_TILE_BYTES)

/* ---- monitor ---- */
#define NX_MAX_TASKS           24    /* tasks reported per board                             */
#define NX_MONITOR_PERIOD_MS   500   /* sampled twice a second                               */
