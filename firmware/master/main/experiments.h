/*
 * experiments.h — the experiment runner (tests T1–T5 of the proposal).
 *
 * A task on the master that sets a configuration, waits until the cluster is
 * idle, submits a fixed workload, waits until every job is committed, and
 * stores one summary row per run (/api/runs.csv). Every committed job is also
 * tagged with its run id in /api/csv (the last 4096 jobs are kept), so the
 * report's graphs come straight from the boards.
 *
 *   T1 scheduling   3 orderings x 2 placements, fractal frame + 60 mixed-priority jobs, 3 repetitions
 *   T2 correctness  single-node reference frame, then 2 workers + 4 concurrent producers: same image CRC?
 *   T3 recovery     reference frame, then 2 workers with Worker A crashed at 40%: same image CRC?
 *   T4 slots        K = 1..4 permits on A/B/C, 120 matrix jobs, 3 repetitions
 *   T5 scalability  0 / 1 / 2 / 3 workers x small / large matrix jobs, 3 repetitions
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "job_queue.h"
#include "placement.h"

#define NX_EXP_MAX_RUNS 64

typedef struct {
    uint16_t id;                 /* run id: the "run" column of /api/csv     */
    uint8_t  suite;              /* 1..5 = T1..T5                            */
    char     label[32];
    uint32_t jobs, committed, failed, retries, dups;
    uint32_t makespan_ms;
    float    throughput;
    uint32_t mean_ms, p95_ms, max_ms;
    uint32_t mean_wait_ms[NX_PRIO_MAX + 1], max_wait_ms[NX_PRIO_MAX + 1];
    uint32_t by_node[NX_NODES];   /* commits by master and each worker       */
    uint32_t crc;                /* fractal image CRC32, 0 if no frame       */
    uint32_t detect_ms, requeued, recovered, recovery_ms;
    uint16_t fault_tiles;        /* T3: tiles completed at reboot request */
    uint8_t producers;           /* T2: producers that submitted all jobs */
    uint8_t slots;               /* requested and confirmed before start */
    int8_t   pass;               /* T2/T3: 1 pass, 0 fail; -1 not applicable */
} nx_run_t;

typedef struct {
    bool     busy;
    uint8_t  suite;
    uint16_t run, of;
    char     label[32];
    uint16_t n_runs;
} nx_exp_status_t;

void exp_start(nx_job_queue_t *gq);
bool exp_request(uint8_t suite);       /* 1..5; false if a suite is already running */
void exp_stop(void);
void exp_status(nx_exp_status_t *st);
int  exp_recent(nx_run_t *out, int max);   /* newest first */
int  exp_runs_csv(char *buf, int cap);
