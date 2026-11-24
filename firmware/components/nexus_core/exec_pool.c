/*
 * exec_pool.c — fixed-size worker thread pool with execution slots (M2, M3).
 *
 * Executor loop (one FreeRTOS task per executor):
 *   take(sem_ready)   wait until a job exists        (consumer side of the bounded buffer)
 *   take(sem_slots)   wait for an execution permit   (at most K jobs run at once)
 *   pop best job      lock q_mtx, remove, unlock, give(sem_free)
 *   run it            cooperative cancel when the execution budget runs out (A2)
 *   give(sem_slots)   return the permit
 *   report result     callback: send RESULT to the master, or commit locally
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "exec_pool.h"
#include "kernels.h"

typedef struct { nx_pool_t *pool; uint8_t index; } exec_arg_t;
static exec_arg_t s_args[NX_POOL_SIZE * 2];   /* master local pool + one worker pool at most */
static uint8_t s_arg_used;

static void return_permit(nx_pool_t *p)
{
    bool retire;
    taskENTER_CRITICAL(&p->lock);
    retire = p->permits > p->target_k;      /* K was lowered: keep this permit out of circulation */
    if (retire) p->permits--;
    taskEXIT_CRITICAL(&p->lock);
    if (!retire) xSemaphoreGive(p->sem_slots);
}

static void take_permit(nx_pool_t *p)
{
    for (;;) {
        xSemaphoreTake(p->sem_slots, portMAX_DELAY);
        taskENTER_CRITICAL(&p->lock);
        bool keep = p->permits <= p->target_k;
        if (!keep) p->permits--;
        taskEXIT_CRITICAL(&p->lock);
        if (keep) return;
    }
}

static void executor_task(void *arg)
{
    nx_pool_t *p = ((exec_arg_t *)arg)->pool;
    const uint8_t me = ((exec_arg_t *)arg)->index;
    int64_t last_block = esp_timer_get_time();

    for (;;) {
        int64_t w0 = esp_timer_get_time();
        jq_wait_ready(p->q, portMAX_DELAY);                 /* OS CONCEPT: sleep while no job   */
        take_permit(p);                                   /* OS CONCEPT: counting semaphore   */
        if (esp_timer_get_time() - w0 > 1000) last_block = esp_timer_get_time();

        nx_job_t job;
        if (!jq_pop_best(p->q, &job, 0, 0)) {              /* entry was cancelled while queued */
            return_permit(p);
            continue;
        }

        taskENTER_CRITICAL(&p->lock);
        p->running++;
        if (p->running > p->peak_running) p->peak_running = p->running;
        p->running_id[me] = job.spec.id;
        taskEXIT_CRITICAL(&p->lock);

        nx_result_t res = { .id = job.spec.id, .attempt = job.spec.attempt, .core = (uint8_t)esp_cpu_get_core_id() };
        int64_t t0 = esp_timer_get_time();
        uint16_t len = 0;
        uint32_t crc = 0;      /* nx_result_t is packed: compute into locals, then copy */
        res.status = nx_run_job(&job.spec, p->scratch[me], &len, &crc,
                                t0 + (int64_t)job.spec.budget_ms * 1000);
        res.checksum = crc;
        res.exec_us = (uint32_t)(esp_timer_get_time() - t0);
        res.data_len = len;

        taskENTER_CRITICAL(&p->lock);
        p->running--;
        p->running_id[me] = 0;
        p->completed++;
        if (res.status == NX_RES_TIMEOUT) p->timeouts++;
        taskEXIT_CRITICAL(&p->lock);
        return_permit(p);

        p->on_result(&job, &res, p->scratch[me], p->cb_arg);

        /* OS CONCEPT: a CPU-bound task that never blocks starves the idle task and the
         * task watchdog complains after 5 s. Sleep one tick at least once per second. */
        if (esp_timer_get_time() - last_block > 1000000) {
            vTaskDelay(1);
            last_block = esp_timer_get_time();
        }
    }
}

void pool_start(nx_pool_t *p, nx_job_queue_t *q, uint8_t k, nx_result_cb_t cb, void *arg)
{
    memset(p, 0, sizeof(*p));
    p->q = q;
    p->n = NX_POOL_SIZE;
    p->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    p->on_result = cb;
    p->cb_arg = arg;
    p->sem_slots = xSemaphoreCreateCounting(NX_POOL_SIZE, 0);
    if (!p->sem_slots) abort();
    p->target_k = 0;
    p->permits = 0;
    pool_set_slots(p, k);

    for (int i = 0; i < NX_POOL_SIZE; i++) {
        p->scratch[i] = heap_caps_malloc(NX_SCRATCH_BYTES, MALLOC_CAP_SPIRAM);
        if (!p->scratch[i]) p->scratch[i] = heap_caps_malloc(NX_SCRATCH_BYTES, MALLOC_CAP_8BIT);
        if (!p->scratch[i] || s_arg_used >= sizeof(s_args) / sizeof(s_args[0])) abort();
        exec_arg_t *a = &s_args[s_arg_used++];
        a->pool = p;
        a->index = (uint8_t)i;
        char name[12];
        snprintf(name, sizeof(name), "exec_%d", i);
        /* FreeRTOS pins tasks on first float use. Spread them explicitly at boot. */
        if (xTaskCreatePinnedToCore(executor_task, name, NX_STACK_EXEC, a, NX_PRIO_EXEC,
                                    NULL, i % configNUMBER_OF_CORES) != pdPASS) abort();
    }
}

void pool_set_slots(nx_pool_t *p, uint8_t k)
{
    if (k < 1) k = 1;
    if (k > p->n) k = p->n;

    int add = 0;
    taskENTER_CRITICAL(&p->lock);
    p->target_k = k;
    if (p->permits < k) { add = k - p->permits; p->permits = k; }
    taskEXIT_CRITICAL(&p->lock);
    for (int i = 0; i < add; i++) xSemaphoreGive(p->sem_slots);

    /* Retire idle permits now; busy ones are retired by return_permit(). */
    while (xSemaphoreTake(p->sem_slots, 0) == pdTRUE) {
        bool keep;
        taskENTER_CRITICAL(&p->lock);
        keep = p->permits <= p->target_k;
        if (!keep) p->permits--;
        taskEXIT_CRITICAL(&p->lock);
        if (keep) { xSemaphoreGive(p->sem_slots); break; }
    }
}

uint8_t pool_running_ids(nx_pool_t *p, uint32_t *ids, uint8_t max)
{
    uint8_t n = 0;
    taskENTER_CRITICAL(&p->lock);
    for (int i = 0; i < NX_POOL_SIZE && n < max; i++)
        if (p->running_id[i]) ids[n++] = p->running_id[i];
    taskEXIT_CRITICAL(&p->lock);
    return n;
}
