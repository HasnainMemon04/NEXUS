/*
 * stats.c — performance statistics (A5).
 * A ring of the last 4096 committed jobs lives in PSRAM; the dashboard and the
 * CSV export read it. One mutex protects the ring and the counters.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "stats.h"

#define RING 4096          /* enough for a whole T1 suite (18 x 210 jobs) */

static nx_done_rec_t *s_ring;
static uint32_t s_count;            /* total committed (also the next ring index) */
static uint32_t s_dups;
static uint16_t s_run;   /* current experiment run id */
static SemaphoreHandle_t s_mtx;
static uint32_t *s_ids;          /* duplicate-ID check for experiment windows */
static uint32_t *s_lat;          /* sort buffer for nx_stats_window (PSRAM) */
static struct { bool valid; uint8_t node; uint32_t detect_ms; int requeued; bool injected; int64_t at; } s_fail;

void nx_stats_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_ring = heap_caps_calloc(RING, sizeof(nx_done_rec_t), MALLOC_CAP_SPIRAM);
    if (!s_ring) s_ring = heap_caps_calloc(RING, sizeof(nx_done_rec_t), MALLOC_CAP_8BIT);
    s_lat = heap_caps_calloc(RING, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_ids = heap_caps_calloc(RING, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!s_mtx || !s_ring || !s_lat || !s_ids) abort();
}

void nx_stats_on_commit(const nx_job_t *job, const nx_result_t *res)
{
    nx_done_rec_t r = {
        .id = job->spec.id, .type = job->spec.type, .prio = job->spec.prio, .node = job->node,
        .attempt = (uint8_t)job->spec.attempt,
        .latency_ms = (uint32_t)((job->t_done - job->t_submit) / 1000),
        .wait_ms = job->t_dispatch >= job->t_submit ? (uint32_t)((job->t_dispatch - job->t_submit) / 1000) : 0,
        .checksum = res->checksum, .exec_us = res->exec_us, .t_done = job->t_done,
    };
    if (job->spec.type == NX_JOB_FRACTAL) { r.tx = job->spec.p.fractal.tx; r.ty = job->spec.p.fractal.ty; }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    r.run = s_run;
    s_ring[s_count % RING] = r;
    s_count++;
    xSemaphoreGive(s_mtx);
}

void nx_stats_on_duplicate(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_dups++;
    xSemaphoreGive(s_mtx);
}

void nx_stats_on_failure(uint8_t node, uint32_t detect_ms, int requeued, bool injected)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_fail.valid = true;
    s_fail.node = node;
    s_fail.detect_ms = detect_ms;
    s_fail.requeued = requeued;
    s_fail.injected = injected;
    s_fail.at = esp_timer_get_time();
    xSemaphoreGive(s_mtx);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

void nx_stats_view(nx_stats_view_t *v)
{
    static uint32_t lat[200];
    int64_t now = esp_timer_get_time();
    uint32_t wait_sum[NX_PRIO_MAX + 1] = { 0 }, wait_n[NX_PRIO_MAX + 1] = { 0 };
    memset(v, 0, sizeof(*v));

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    v->committed = s_count;
    v->duplicates = s_dups;
    int avail = s_count < RING ? (int)s_count : RING;
    int nl = 0, recent3s = 0;
    uint64_t lat_sum = 0;
    for (int k = 0; k < avail; k++) {
        const nx_done_rec_t *r = &s_ring[(s_count - 1 - k) % RING];
        bool in_window = now - r->t_done <= 3000000;
        if (in_window) {
            recent3s++;
            uint8_t p = r->prio > NX_PRIO_MAX ? NX_PRIO_MAX : r->prio;
            wait_sum[p] += r->wait_ms;
            wait_n[p]++;
        }
        if (nl < 200) { lat[nl++] = r->latency_ms; lat_sum += r->latency_ms; }
        if (k < 8) v->recent[v->n_recent++] = *r;
        if (!in_window && nl >= 200 && k >= 8) break;
    }
    if (s_fail.valid) {
        v->has_failure = true;
        v->fail_node = s_fail.node;
        v->fail_detect_ms = s_fail.detect_ms;
        v->fail_requeued = s_fail.requeued;
        v->fail_injected = s_fail.injected;
        v->fail_age_ms = (uint32_t)((now - s_fail.at) / 1000);
    }
    xSemaphoreGive(s_mtx);

    v->throughput = recent3s / 3.0f;
    if (nl) {
        qsort(lat, (size_t)nl, sizeof(*lat), cmp_u32);
        v->p95_ms = lat[(nl * 95 + 99) / 100 - 1];
        v->mean_ms = (uint32_t)(lat_sum / nl);
    }
    for (int p = 0; p <= NX_PRIO_MAX; p++) v->mean_wait_ms[p] = wait_n[p] ? wait_sum[p] / wait_n[p] : 0;
}

int nx_stats_csv(char *buf, int cap, uint32_t *cursor, uint32_t end)
{
    int len = 0;
    if (*cursor == 0) len += snprintf(buf, cap, "run,id,type,prio,node,attempt,tile_x,tile_y,latency_ms,wait_ms,exec_us,checksum\n");
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint32_t first = *cursor;
    if (s_count > RING && first < s_count - RING) first = s_count - RING;
    uint32_t i = first;
    for (; i < end && i < s_count && len < cap - 112; i++) {
        const nx_done_rec_t *r = &s_ring[i % RING];
        len += snprintf(buf + len, cap - len, "%u,%lu,%u,%u,%u,%u,%u,%u,%lu,%lu,%lu,%08lx\n",
                        r->run, (unsigned long)r->id, r->type, r->prio, r->node, r->attempt, r->tx, r->ty,
                        (unsigned long)r->latency_ms, (unsigned long)r->wait_ms, (unsigned long)r->exec_us, (unsigned long)r->checksum);
    }
    *cursor = i;
    xSemaphoreGive(s_mtx);
    return len;
}

/* ------------------------------ experiments ------------------------------ */

void nx_stats_set_run(uint16_t run)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_run = run;
    xSemaphoreGive(s_mtx);
}

uint32_t nx_stats_count(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint32_t n = s_count;
    xSemaphoreGive(s_mtx);
    return n;
}

uint32_t nx_stats_dups(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint32_t n = s_dups;
    xSemaphoreGive(s_mtx);
    return n;
}

void nx_stats_window(uint32_t from, uint32_t to, int64_t t_start, uint16_t run, nx_window_t *w)
{
    uint32_t *lat = s_lat;
    uint64_t lat_sum = 0, wait_sum[NX_PRIO_MAX + 1] = { 0 };
    uint32_t wait_n[NX_PRIO_MAX + 1] = { 0 };
    int64_t last_done = t_start;
    memset(w, 0, sizeof(*w));

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (to > s_count) to = s_count;
    if (s_count > RING && from < s_count - RING) from = s_count - RING;   /* older entries were overwritten */
    for (uint32_t i = from; i < to; i++) {
        const nx_done_rec_t *r = &s_ring[i % RING];
        uint8_t p = r->prio > NX_PRIO_MAX ? NX_PRIO_MAX : r->prio;
        if (r->run != run) continue;
        s_ids[w->jobs] = r->id;
        lat[w->jobs++] = r->latency_ms;
        lat_sum += r->latency_ms;
        if (r->latency_ms > w->max_ms) w->max_ms = r->latency_ms;
        wait_sum[p] += r->wait_ms;
        wait_n[p]++;
        if (r->wait_ms > w->max_wait_ms[p]) w->max_wait_ms[p] = r->wait_ms;
        if (r->node < NX_NODES) w->by_node[r->node]++;
        if (r->t_done > last_done) last_done = r->t_done;
    }
    xSemaphoreGive(s_mtx);

    w->makespan_ms = (uint32_t)((last_done - t_start) / 1000);
    w->throughput = w->makespan_ms ? w->jobs * 1000.0f / w->makespan_ms : 0;
    if (w->jobs) {
        qsort(lat, w->jobs, sizeof(*lat), cmp_u32);
        qsort(s_ids, w->jobs, sizeof(*s_ids), cmp_u32);
        for (uint32_t i = 1; i < w->jobs; i++)
            if (s_ids[i] == s_ids[i - 1]) w->repeated_ids++;
        w->mean_ms = (uint32_t)(lat_sum / w->jobs);
        w->p95_ms = lat[(w->jobs * 95 + 99) / 100 - 1];
    }
    for (int p = 0; p <= NX_PRIO_MAX; p++) w->mean_wait_ms[p] = wait_n[p] ? (uint32_t)(wait_sum[p] / wait_n[p]) : 0;
}

bool nx_stats_last_failure(uint32_t *detect_ms, uint32_t *age_ms)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool valid = s_fail.valid;
    if (valid) {
        *detect_ms = s_fail.detect_ms;
        *age_ms = (uint32_t)((esp_timer_get_time() - s_fail.at) / 1000);
    }
    xSemaphoreGive(s_mtx);
    return valid;
}
