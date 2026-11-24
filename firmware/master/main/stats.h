/* stats.h — performance statistics (A5), all times on the master's clock. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "job_queue.h"
#include "placement.h"

typedef struct {
    uint32_t id;
    uint8_t  type, prio, node, attempt;
    uint16_t tx, ty;
    uint32_t latency_ms;   /* done - submit   */
    uint32_t wait_ms;      /* dispatch - submit */
    uint32_t checksum;     /* checksum reported by the executor */
    uint32_t exec_us;      /* measured by the worker */
    uint16_t run;          /* experiment run id, 0 outside experiments */
    int64_t  t_done;
} nx_done_rec_t;

typedef struct {
    float    throughput;        /* jobs committed per second over the last 3 s */
    uint32_t p95_ms, mean_ms;   /* latency over the last 200 jobs              */
    uint32_t committed, duplicates;
    uint32_t mean_wait_ms[NX_PRIO_MAX + 1];
    uint8_t  n_recent;
    nx_done_rec_t recent[8];
    bool     has_failure;
    uint8_t  fail_node;
    uint32_t fail_detect_ms;
    int      fail_requeued;
    bool     fail_injected;
    uint32_t fail_age_ms;
} nx_stats_view_t;

void nx_stats_init(void);
void nx_stats_on_commit(const nx_job_t *job, const nx_result_t *res);
void nx_stats_on_duplicate(void);
void nx_stats_on_failure(uint8_t node, uint32_t detect_ms, int requeued, bool injected);
void nx_stats_view(nx_stats_view_t *v);
/* CSV of committed jobs with index > *cursor (one row per job); returns bytes written. */
int  nx_stats_csv(char *buf, int cap, uint32_t *cursor, uint32_t end);

/* ---- experiment support (experiments.c) ---- */
typedef struct {
    uint32_t jobs;                              /* commits in the window             */
    uint32_t repeated_ids;                      /* repeated committed IDs in the run */
    uint32_t makespan_ms;                       /* run start -> last commit           */
    float    throughput;                        /* jobs / s over the makespan         */
    uint32_t mean_ms, p95_ms, max_ms;           /* latency (submit -> done)           */
    uint32_t mean_wait_ms[NX_PRIO_MAX + 1];     /* queue wait (submit -> dispatch)    */
    uint32_t max_wait_ms[NX_PRIO_MAX + 1];
    uint32_t by_node[NX_NODES];                 /* commits by master and each worker */
} nx_window_t;

void     nx_stats_set_run(uint16_t run);        /* tag the following commits          */
uint32_t nx_stats_count(void);                  /* commits so far (ring position)     */
uint32_t nx_stats_dups(void);
/* Summarise commits [from, to) of the ring; t_start = run start (esp_timer us). */
void     nx_stats_window(uint32_t from, uint32_t to, int64_t t_start, uint16_t run, nx_window_t *w);
/* Last worker failure: detection delay and how long ago it happened. */
bool     nx_stats_last_failure(uint32_t *detect_ms, uint32_t *age_ms);
