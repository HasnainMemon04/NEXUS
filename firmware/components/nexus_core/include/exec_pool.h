/*
 * exec_pool.h — fixed-size worker thread pool with execution slots (M2, M3).
 *
 * OS CONCEPT: thread pool (OSC §4.5.1). N executor tasks are created once at
 * boot and reused for every job; no task is ever created per job.
 * OS CONCEPT: counting semaphore (OSC §6.6). sem_slots holds K <= N permits;
 * an executor must take a permit before it takes a job, so at most K jobs run
 * at the same time. K can be changed at run time (dynamic scaling, A3).
 */
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "job_queue.h"

typedef void (*nx_result_cb_t)(const nx_job_t *job, const nx_result_t *res, const uint8_t *data, void *arg);

typedef struct {
    nx_job_queue_t   *q;            /* local ready queue the executors consume from  */
    SemaphoreHandle_t sem_slots;    /* the K execution permits                       */
    portMUX_TYPE      lock;         /* spinlock for the small counters below         */
    uint8_t  n;                     /* pool size N                                   */
    uint8_t  target_k;              /* permits wanted                                */
    uint8_t  permits;               /* permits currently in circulation              */
    uint8_t  running, peak_running; /* jobs executing now / highest ever (proof of M3) */
    uint32_t completed, timeouts;
    uint32_t running_id[NX_POOL_SIZE];
    nx_result_cb_t on_result;
    void    *cb_arg;
    uint8_t *scratch[NX_POOL_SIZE]; /* per-executor work buffers, allocated once      */
} nx_pool_t;

/* Create the N executor tasks (named exec_0 .. exec_3) with K permits. */
void    pool_start(nx_pool_t *p, nx_job_queue_t *q, uint8_t k, nx_result_cb_t cb, void *arg);
/* Change K live: extra permits are given at once; surplus permits are retired
 * as soon as they come back (never by interrupting a running job). */
void    pool_set_slots(nx_pool_t *p, uint8_t k);
uint8_t pool_running_ids(nx_pool_t *p, uint32_t *ids, uint8_t max);
