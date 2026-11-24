/*
 * job_queue.h — the shared job queue (Project #2 requirement M1).
 *
 * OS CONCEPT: bounded buffer / producer–consumer (OSC §7.1.1).
 *   ready[]     the buffer of QUEUED jobs, fixed capacity
 *   mtx         mutex q_mtx: protects every field of the queue
 *   sem_free    counting semaphore: free places in ready[]   (producers wait on it)
 *   sem_ready   counting semaphore: jobs waiting in ready[]  (consumers wait on it)
 *
 * The same module is used on every board. On the master it also keeps the
 * in-flight table (jobs dispatched to a node but not yet committed), under the
 * same mutex, so a job is always in exactly one place.
 */
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nexus_types.h"
#include "nexus_config.h"

typedef struct {
    nx_job_spec_t spec;
    uint8_t  state;          /* nx_job_state_t                                  */
    uint8_t  node;           /* node holding the job, NX_NODE_NONE if none      */
    bool     requeue_pending;/* master: waiting for a free place to be requeued */
    uint8_t  attempt_nodes[NX_MAX_ATTEMPTS]; /* original assignments, including late results */
    uint8_t  recovery_from;  /* node whose failure most recently caused a retry */
    uint16_t recovery_min_attempt;
    int64_t  attempt_dispatch_us[NX_MAX_ATTEMPTS];
    uint32_t recovery_serial;
    uint32_t seq;            /* FCFS order; kept when a job is retried          */
    uint32_t node_inc;       /* incarnation of that node at dispatch time       */
    int64_t  t_submit;       /* esp_timer_get_time() of the board that queued it*/
    int64_t  t_dispatch, t_ack, t_done;
    int64_t  lease_until;    /* master: requeue if no result by then            */
} nx_job_t;

typedef struct {
    nx_job_t *ready;    uint16_t ready_cap,    ready_count;
    nx_job_t *inflight; uint16_t inflight_cap, inflight_count;   /* cap 0 on workers */
    SemaphoreHandle_t mtx;        /* q_mtx     */
    SemaphoreHandle_t sem_free;   /* sem_free  */
    SemaphoreHandle_t sem_ready;  /* sem_ready */
    uint8_t  ordering;            /* nx_ordering_t, protected by mtx */
    uint32_t next_seq, next_id;
    /* statistics (updated while holding mtx) */
    uint32_t lock_ops, lock_contended, peak_ready;
    uint32_t retries, failed, cancelled;
    nx_job_t failed_recent[8];    /* ring of the last FAILED jobs, for the dashboard */
    uint8_t  failed_head;
} nx_job_queue_t;

typedef enum { NX_COMMIT_OK = 0, NX_COMMIT_DUPLICATE, NX_COMMIT_INVALID } nx_commit_t;

/* Allocate the buffers (in PSRAM when they are large). */
bool     jq_init(nx_job_queue_t *q, uint16_t ready_cap, uint16_t inflight_cap);
uint32_t jq_new_id(nx_job_queue_t *q);
void     jq_set_ordering(nx_job_queue_t *q, uint8_t ordering);
uint8_t  jq_get_ordering(nx_job_queue_t *q);

/* Producer: wait for a free place (up to `wait`), then insert. Gives the job an
 * id and sequence number if it has none. Returns false if the queue stayed full. */
bool jq_submit(nx_job_queue_t *q, const nx_job_spec_t *spec, TickType_t wait);

/* Consumer, step 1: wait until a job is available (takes one sem_ready token). */
bool jq_wait_ready(nx_job_queue_t *q, TickType_t wait);

/* Consumer, step 2 (only after jq_wait_ready succeeded): remove the best job for
 * the current ordering policy and free its place. On the master the job is also
 * recorded in-flight on `node`. Returns false after discarding a cancelled entry or
 * restoring the ready token when the in-flight table cannot admit it. */
bool jq_pop_best(nx_job_queue_t *q, nx_job_t *out, uint8_t node, uint32_t node_inc);

/* Effective priority used by the ordering policy (aging included). */
int jq_effective_priority(uint8_t ordering, const nx_job_t *j, int64_t now_us);

/* ---- master only: in-flight table ---- */
void        jq_mark_running(nx_job_queue_t *q, uint32_t id, uint16_t attempt, uint8_t node);
nx_commit_t jq_commit(nx_job_queue_t *q, const nx_result_t *result, uint8_t node, nx_job_t *done);
bool        jq_requeue_one(nx_job_queue_t *q, uint32_t id, uint16_t attempt, uint8_t node);
int         jq_requeue_node(nx_job_queue_t *q, uint8_t node, uint32_t failure_serial);
int         jq_requeue_expired(nx_job_queue_t *q, int64_t now_us);
int         jq_retry_pending(nx_job_queue_t *q);
uint16_t    jq_count_on_node(nx_job_queue_t *q, uint8_t node);
int         jq_cancel_old_frames(nx_job_queue_t *q, uint16_t keep_frame);
/* Physical queue occupancy, including cancelled records awaiting their tokens. */
bool        jq_has_pending(nx_job_queue_t *q);
/* Counters without copying the whole view (used by the experiment runner). */
void        jq_counts(nx_job_queue_t *q, uint16_t *ready, uint16_t *inflight, uint32_t *failed, uint32_t *retries);

/* Copy a consistent view for the dashboard (top `max_ready` jobs in dispatch order). */
typedef struct {
    uint16_t ready_count, inflight_count, ready_cap;
    uint32_t lock_ops, lock_contended, retries, failed, cancelled;
    uint8_t  ordering;
    uint8_t  n_top, n_inflight, n_failed;
    nx_job_t top[8];
    nx_job_t inflight[16];
    nx_job_t failed_jobs[4];
    uint16_t ready_by_prio[NX_PRIO_MAX + 1];
    uint32_t oldest_wait_ms[NX_PRIO_MAX + 1];
} nx_queue_view_t;
void jq_view(nx_job_queue_t *q, nx_queue_view_t *v, int64_t now_us);
