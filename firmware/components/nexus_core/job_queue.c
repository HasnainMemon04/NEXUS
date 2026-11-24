/*
 * job_queue.c — the shared job queue (M1).
 *
 * OS CONCEPT: bounded buffer with one mutex and two counting semaphores
 * (OSC §7.1.1). Producers:  take(sem_free)  lock  insert  unlock  give(sem_ready)
 *               Consumers:  take(sem_ready) lock  remove  unlock  give(sem_free)
 *
 * Every function that touches the queue holds q_mtx for the whole update, so
 * the ready buffer, the in-flight table and the counters never disagree.
 */
#include <stdlib.h>
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "job_queue.h"

static void *alloc_table(size_t bytes)
{
    /* Large tables go to the 8 MB PSRAM; small ones stay in internal RAM. */
    void *p = NULL;
    if (bytes > 4096) p = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_calloc(1, bytes, MALLOC_CAP_8BIT);
    return p;
}

/* OS CONCEPT: mutual exclusion (OSC §6.5). We first try without waiting so we
 * can count how often another task already held the lock (contention). */
static void q_lock(nx_job_queue_t *q)
{
    bool waited = xSemaphoreTake(q->mtx, 0) != pdTRUE;
    if (waited) xSemaphoreTake(q->mtx, portMAX_DELAY);
    q->lock_ops++;
    if (waited) q->lock_contended++;
}

static void q_unlock(nx_job_queue_t *q) { xSemaphoreGive(q->mtx); }

bool jq_init(nx_job_queue_t *q, uint16_t ready_cap, uint16_t inflight_cap)
{
    memset(q, 0, sizeof(*q));
    if (ready_cap == 0) return false;
    q->ready = alloc_table(sizeof(nx_job_t) * ready_cap);
    q->inflight = inflight_cap ? alloc_table(sizeof(nx_job_t) * inflight_cap) : NULL;
    q->ready_cap = ready_cap;
    q->inflight_cap = inflight_cap;
    q->mtx = xSemaphoreCreateMutex();                            /* has priority inheritance */
    q->sem_free = xSemaphoreCreateCounting(ready_cap, ready_cap); /* all places free          */
    q->sem_ready = xSemaphoreCreateCounting(ready_cap, 0);        /* no job yet               */
    q->ordering = NX_ORDER_AGING;
    /* A surviving worker can deliver pre-reboot results after reconnecting. */
    q->next_id = inflight_cap ? (esp_random() & 0x3FFFFFFFu) + 1 : 1000;
    if (q->ready && (inflight_cap == 0 || q->inflight) && q->mtx && q->sem_free && q->sem_ready)
        return true;
    if (q->sem_ready) vSemaphoreDelete(q->sem_ready);
    if (q->sem_free) vSemaphoreDelete(q->sem_free);
    if (q->mtx) vSemaphoreDelete(q->mtx);
    free(q->ready);
    free(q->inflight);
    memset(q, 0, sizeof(*q));
    return false;
}

uint32_t jq_new_id(nx_job_queue_t *q)
{
    q_lock(q);
    uint32_t id = q->next_id++;
    q_unlock(q);
    return id;
}

void jq_set_ordering(nx_job_queue_t *q, uint8_t ordering)
{
    if (ordering > NX_ORDER_AGING) return;
    q_lock(q);
    q->ordering = ordering;
    q_unlock(q);
}

uint8_t jq_get_ordering(nx_job_queue_t *q)
{
    q_lock(q);
    uint8_t ordering = q->ordering;
    q_unlock(q);
    return ordering;
}

int jq_effective_priority(uint8_t ordering, const nx_job_t *j, int64_t now_us)
{
    if (ordering == NX_ORDER_FCFS) return 0;                  /* only arrival order counts */
    if (ordering == NX_ORDER_PRIORITY) return j->spec.prio;   /* strict priority           */
    /* OS CONCEPT: aging (OSC §5.3.4) — waiting raises the priority, so no job starves. */
    int64_t waited_ms = now_us > j->t_submit ? (now_us - j->t_submit) / 1000 : 0;
    if (waited_ms >= (int64_t)NX_PRIO_MAX * NX_AGING_STEP_MS) return NX_PRIO_MAX;
    int p = j->spec.prio + (int)(waited_ms / NX_AGING_STEP_MS);
    return p > NX_PRIO_MAX ? NX_PRIO_MAX : p;
}

/* Must hold q_mtx. Highest effective priority wins; ties go to the oldest (FCFS). */
static int pick_best_locked(nx_job_queue_t *q, int64_t now)
{
    int best = -1, best_p = -1;
    uint32_t best_seq = 0;
    for (int i = 0; i < q->ready_count; i++) {
        const nx_job_t *j = &q->ready[i];
        int p = jq_effective_priority(q->ordering, j, now);
        if (best < 0 || p > best_p || (p == best_p && j->seq < best_seq)) {
            best = i; best_p = p; best_seq = j->seq;
        }
    }
    return best;
}

/* Must hold q_mtx and own one sem_free token. */
static void insert_ready_locked(nx_job_queue_t *q, const nx_job_t *j)
{
    q->ready[q->ready_count++] = *j;
    if (q->ready_count > q->peak_ready) q->peak_ready = q->ready_count;
}

bool jq_submit(nx_job_queue_t *q, const nx_job_spec_t *spec, TickType_t wait)
{
    if (spec->attempt > NX_MAX_ATTEMPTS) return false;
    /* OS CONCEPT: a producer blocks here while the buffer is full. */
    if (xSemaphoreTake(q->sem_free, wait) != pdTRUE) return false;

    q_lock(q);
    nx_job_t j = {
        .spec = *spec, .state = NX_JS_QUEUED, .node = NX_NODE_NONE, .recovery_from = NX_NODE_NONE,
    };
    memset(j.attempt_nodes, NX_NODE_NONE, sizeof(j.attempt_nodes));
    if (j.spec.id == 0) j.spec.id = q->next_id++;
    if (j.spec.attempt == 0) j.spec.attempt = 1;
    j.seq = q->next_seq++;
    j.t_submit = esp_timer_get_time();
    insert_ready_locked(q, &j);
    q_unlock(q);

    xSemaphoreGive(q->sem_ready);   /* wake one consumer */
    return true;
}

bool jq_wait_ready(nx_job_queue_t *q, TickType_t wait)
{
    /* OS CONCEPT: a consumer sleeps here while the buffer is empty. */
    return xSemaphoreTake(q->sem_ready, wait) == pdTRUE;
}

static nx_job_t retry_job(const nx_job_t *job)
{
    nx_job_t again = *job;
    again.spec.attempt++;
    again.state = NX_JS_QUEUED;
    again.node = NX_NODE_NONE;
    again.requeue_pending = false;
    again.t_ack = 0;
    return again;
}

bool jq_pop_best(nx_job_queue_t *q, nx_job_t *out, uint8_t node, uint32_t node_inc)
{
    q_lock(q);
    int64_t now = esp_timer_get_time();
    int i = pick_best_locked(q, now);
    if (i < 0) {
        q_unlock(q);
        return false;
    }
    bool valid = q->ready[i].state != NX_JS_CANCELLED;
    int swap = -1;
    if (valid && q->inflight_cap > 0 && q->inflight_count == q->inflight_cap) {
        /* With both buffers full, exchange a pending retry for the ready job.
         * Refusing every dispatch here would prevent either queue freeing space. */
        for (int j = 0; j < q->inflight_count; j++) {
            if (q->inflight[j].requeue_pending) { swap = j; break; }
        }
        if (swap < 0) {
            q_unlock(q);
            xSemaphoreGive(q->sem_ready);
            return false;
        }
    }

    *out = q->ready[i];
    q->ready[i] = q->ready[--q->ready_count];   /* order is kept by seq, not by position */
    if (valid && q->inflight_cap > 0) {
        out->state = NX_JS_DISPATCHED;
        out->node = node;
        out->node_inc = node_inc;
        out->attempt_nodes[out->spec.attempt - 1] = node;
        out->attempt_dispatch_us[out->spec.attempt - 1] = now;
        out->t_dispatch = now;
        out->lease_until = now + ((int64_t)out->spec.budget_ms + NX_LEASE_MARGIN_MS) * 1000;
        if (swap >= 0) {
            nx_job_t again = retry_job(&q->inflight[swap]);
            insert_ready_locked(q, &again);
            q->inflight[swap] = *out;
        } else {
            q->inflight[q->inflight_count++] = *out;
        }
    } else if (valid) {
        out->state = NX_JS_RUNNING;
    }
    q_unlock(q);

    if (swap >= 0) xSemaphoreGive(q->sem_ready);
    else xSemaphoreGive(q->sem_free);
    return valid;
}

/* ------------------------- master: in-flight table ------------------------- */

static int find_inflight_locked(nx_job_queue_t *q, uint32_t id)
{
    for (int i = 0; i < q->inflight_count; i++)
        if (q->inflight[i].spec.id == id) return i;
    return -1;
}

static void remove_inflight_locked(nx_job_queue_t *q, int i)
{
    q->inflight[i] = q->inflight[--q->inflight_count];
}

void jq_mark_running(nx_job_queue_t *q, uint32_t id, uint16_t attempt, uint8_t node)
{
    q_lock(q);
    int i = find_inflight_locked(q, id);
    if (i >= 0 && q->inflight[i].spec.attempt == attempt && q->inflight[i].node == node &&
        q->inflight[i].state == NX_JS_DISPATCHED) {
        q->inflight[i].state = NX_JS_RUNNING;
        q->inflight[i].t_ack = esp_timer_get_time();
    }
    q_unlock(q);
}

/* The first valid result of a job commits it; any later result is a duplicate.
 * A result from an earlier attempt is still accepted if it matches a recorded
 * assignment and has the expected payload for that job. */
nx_commit_t jq_commit(nx_job_queue_t *q, const nx_result_t *result, uint8_t node, nx_job_t *done)
{
    nx_commit_t r = NX_COMMIT_DUPLICATE;
    q_lock(q);
    int i = find_inflight_locked(q, result->id);
    nx_job_t *job = i >= 0 ? &q->inflight[i] : NULL;
    if (!job) {
        for (int k = 0; k < q->ready_count; k++) {
            if (q->ready[k].spec.id == result->id && q->ready[k].state == NX_JS_QUEUED) {
                job = &q->ready[k];
                break;
            }
        }
    }
    if (job) {
        uint16_t expected = job->spec.type == NX_JOB_FRACTAL ? NX_TILE_BYTES : 0;
        uint16_t attempt = result->attempt;
        if (result->status != NX_RES_OK || attempt == 0 || attempt > NX_MAX_ATTEMPTS ||
            job->attempt_nodes[attempt - 1] != node || result->data_len != expected) {
            r = NX_COMMIT_INVALID;
        } else {
            *done = *job;
            done->state = NX_JS_DONE;
            done->node = node;
            if (attempt != done->spec.attempt) done->t_ack = 0;
            done->spec.attempt = attempt;
            done->t_dispatch = done->attempt_dispatch_us[attempt - 1];
            done->t_done = esp_timer_get_time();
            if (i >= 0) remove_inflight_locked(q, i);
            else job->state = NX_JS_CANCELLED;
            r = NX_COMMIT_OK;
        }
    }
    q_unlock(q);
    return r;
}

/* Must hold q_mtx. Moves in-flight entry i back to the ready buffer (attempt + 1),
 * or to FAILED after the last attempt. Never blocks: if the buffer is full right
 * now, the entry stays in flight with requeue_pending and the supervisor retries. */
static bool requeue_locked(nx_job_queue_t *q, int i)
{
    nx_job_t *j = &q->inflight[i];
    if (!j->requeue_pending) q->retries++;
    if (j->spec.attempt >= NX_MAX_ATTEMPTS) {
        j->state = NX_JS_FAILED;
        j->t_done = esp_timer_get_time();
        q->failed_recent[q->failed_head++ % 8] = *j;
        q->failed++;
        remove_inflight_locked(q, i);
        return true;
    }
    if (xSemaphoreTake(q->sem_free, 0) != pdTRUE) {
        j->requeue_pending = true;
        j->node = NX_NODE_NONE;
        return false;
    }
    nx_job_t again = retry_job(j);       /* keep FIFO age and earlier assignments */
    remove_inflight_locked(q, i);
    insert_ready_locked(q, &again);
    xSemaphoreGive(q->sem_ready);
    return true;
}

bool jq_requeue_one(nx_job_queue_t *q, uint32_t id, uint16_t attempt, uint8_t node)
{
    bool done = false;
    q_lock(q);
    int i = find_inflight_locked(q, id);
    if (i >= 0 && q->inflight[i].spec.attempt == attempt && q->inflight[i].node == node)
        done = requeue_locked(q, i);
    q_unlock(q);
    return done;
}

int jq_requeue_node(nx_job_queue_t *q, uint8_t node, uint32_t failure_serial)
{
    int n = 0;
    q_lock(q);
    for (int i = q->inflight_count - 1; i >= 0; i--) {
        nx_job_t *j = &q->inflight[i];
        if (j->node != node || j->requeue_pending) continue;
        if (j->spec.attempt < NX_MAX_ATTEMPTS) {
            j->recovery_from = node;
            j->recovery_min_attempt = j->spec.attempt + 1;
            j->recovery_serial = failure_serial;
            n++;
        }
        requeue_locked(q, i);
    }
    q_unlock(q);
    return n;
}

int jq_requeue_expired(nx_job_queue_t *q, int64_t now_us)
{
    int n = 0;
    q_lock(q);
    for (int i = q->inflight_count - 1; i >= 0; i--) {
        nx_job_t *j = &q->inflight[i];
        if (!j->requeue_pending && now_us > j->lease_until) { requeue_locked(q, i); n++; }
    }
    q_unlock(q);
    return n;
}

int jq_retry_pending(nx_job_queue_t *q)
{
    int n = 0;
    q_lock(q);
    for (int i = q->inflight_count - 1; i >= 0; i--)
        if (q->inflight[i].requeue_pending && requeue_locked(q, i)) n++;
    q_unlock(q);
    return n;
}

uint16_t jq_count_on_node(nx_job_queue_t *q, uint8_t node)
{
    uint16_t n = 0;
    q_lock(q);
    for (int i = 0; i < q->inflight_count; i++)
        if (q->inflight[i].node == node) n++;
    q_unlock(q);
    return n;
}

int jq_cancel_old_frames(nx_job_queue_t *q, uint16_t keep_frame)
{
    int n = 0;
    q_lock(q);
    for (int i = 0; i < q->ready_count; i++) {
        nx_job_t *j = &q->ready[i];
        if (j->state == NX_JS_QUEUED && j->spec.type == NX_JOB_FRACTAL && j->spec.p.fractal.frame != keep_frame) {
            j->state = NX_JS_CANCELLED;   /* removed when a consumer pops it */
            n++;
        }
    }
    q->cancelled += n;
    q_unlock(q);
    return n;
}

void jq_view(nx_job_queue_t *q, nx_queue_view_t *v, int64_t now_us)
{
    memset(v, 0, sizeof(*v));
    q_lock(q);
    v->ready_count = q->ready_count;
    v->inflight_count = q->inflight_count;
    v->ready_cap = q->ready_cap;
    v->lock_ops = q->lock_ops;
    v->lock_contended = q->lock_contended;
    v->retries = q->retries;
    v->failed = q->failed;
    v->cancelled = q->cancelled;
    v->ordering = q->ordering;

    /* top of the queue in the order the dispatcher would take it (selection, max 8) */
    bool taken[NX_QUEUE_CAPACITY] = { 0 };
    for (int k = 0; k < 8 && k < q->ready_count; k++) {
        int best = -1, best_p = -1;
        uint32_t best_seq = 0;
        for (int i = 0; i < q->ready_count && i < NX_QUEUE_CAPACITY; i++) {
            const nx_job_t *j = &q->ready[i];
            if (taken[i] || j->state != NX_JS_QUEUED) continue;
            int p = jq_effective_priority(q->ordering, j, now_us);
            if (best < 0 || p > best_p || (p == best_p && j->seq < best_seq)) { best = i; best_p = p; best_seq = j->seq; }
        }
        if (best < 0) break;
        taken[best] = true;
        v->top[v->n_top++] = q->ready[best];
    }
    for (int i = 0; i < q->ready_count; i++) {
        const nx_job_t *j = &q->ready[i];
        if (j->state != NX_JS_QUEUED) continue;
        uint8_t p = j->spec.prio > NX_PRIO_MAX ? NX_PRIO_MAX : j->spec.prio;
        v->ready_by_prio[p]++;
        uint32_t w = now_us > j->t_submit ? (uint32_t)((now_us - j->t_submit) / 1000) : 0;
        if (w > v->oldest_wait_ms[p]) v->oldest_wait_ms[p] = w;
    }
    for (int i = 0; i < q->inflight_count && v->n_inflight < 16; i++) v->inflight[v->n_inflight++] = q->inflight[i];
    for (int k = 0; k < 4 && k < (int)q->failed && k < 8; k++)
        v->failed_jobs[v->n_failed++] = q->failed_recent[(uint8_t)(q->failed_head - 1 - k) % 8];
    q_unlock(q);
}

bool jq_has_pending(nx_job_queue_t *q)
{
    q_lock(q);
    bool pending = q->ready_count != 0 || q->inflight_count != 0;
    q_unlock(q);
    return pending;
}

void jq_counts(nx_job_queue_t *q, uint16_t *ready, uint16_t *inflight, uint32_t *failed, uint32_t *retries)
{
    q_lock(q);
    uint16_t live = 0;
    for (int i = 0; i < q->ready_count; i++) if (q->ready[i].state == NX_JS_QUEUED) live++;   /* skip cancelled */
    *ready = live;
    *inflight = q->inflight_count;
    *failed = q->failed;
    *retries = q->retries;
    q_unlock(q);
}
