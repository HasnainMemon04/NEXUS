/*
 * experiments.c — scripted, repeatable experiments (A5 and tests T1–T5).
 *
 * The runner is one more producer of the global job queue: it submits a fixed
 * workload with jq_submit() and blocks when the queue is full, like any client.
 * Workloads are deterministic (fixed fractal view, seeded matrices), so runs
 * can be repeated and compared. While a suite runs, the client tasks and
 * auto-zoom are switched off so that every commit belongs to the run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "logger.h"
#include "cluster.h"
#include "producers.h"
#include "stats.h"
#include "experiments.h"

/* fixed fractal view with uneven tile costs (part of the "seahorse valley") */
#define VIEW_X   (-0.745f)
#define VIEW_Y   (0.11f)
#define VIEW_W   (0.35f)
#define REPS     3
#define T2_PRODUCERS 4
#define T2_JOBS_EACH 40
#define RUN_TIMEOUT_US 180000000LL

typedef struct { uint8_t ord, plc, k; bool use[NX_NODES]; } cfg_t;
typedef enum { W_FRAME, W_FRAME_SPIN, W_FRAME_PRODUCERS, W_FRAME_CRASH, W_MATMUL } work_t;

static nx_job_queue_t   *s_gq;
static QueueHandle_t     s_req;
static SemaphoreHandle_t s_mtx;            /* protects s_runs and s_st        */
static SemaphoreHandle_t s_burst_ready, s_burst_go, s_burst_done;
static atomic_bool       s_stop;
static nx_node_view_t    s_nv[NX_NODES];    /* accessed only by the run task */
static nx_run_t          s_runs[NX_EXP_MAX_RUNS];
static uint16_t          s_nruns, s_next_id = 1;
static nx_exp_status_t   s_st;
static uint32_t          s_ref_crc;        /* single-node reference image     */

static const char *ORD[] = { "FCFS", "PRIO", "AGING" };
static const char *PLC[] = { "RR", "LEAST" };

/* ------------------------------ helpers ------------------------------ */

static int64_t now_us(void) { return esp_timer_get_time(); }

static void apply(const cfg_t *c)
{
    jq_set_ordering(s_gq, c->ord);
    cluster_set_placement(c->plc);
    if (cluster_local() != c->use[NX_NODE_LOCAL]) cluster_set_local(c->use[NX_NODE_LOCAL]);
    cluster_set_worker_drain(!c->use[1], !c->use[2], !c->use[3]);
}

static bool wait_ready(const cfg_t *c, bool confirm_slots, uint32_t timeout_ms)
{
    int64_t end = now_us() + (int64_t)timeout_ms * 1000;
    while (!s_stop && now_us() < end) {
        cluster_view(s_nv);
        bool ok = true;
        for (uint8_t i = 0; i < NX_NODES; i++) {
            if (!c->use[i]) continue;
            if (s_nv[i].state != NODE_ONLINE) {
                ok = false;
            } else if (confirm_slots && i != 0 && !s_nv[i].slot_ack_supported) {
                ok = false;
            } else if (confirm_slots && (!s_nv[i].slots_ready || s_nv[i].k != c->k)) {
                if (s_nv[i].requested_k != c->k || s_nv[i].slots_ready)
                    cluster_set_slots(i, c->k);
                ok = false;
            }
        }
        if (ok) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

/* Idle = no queued job and nothing in flight, three checks in a row. */
static bool wait_idle(uint32_t timeout_ms)
{
    int64_t end = now_us() + (int64_t)timeout_ms * 1000;
    int quiet = 0;
    while (!s_stop && now_us() < end) {
        quiet = jq_has_pending(s_gq) ? 0 : quiet + 1;
        if (quiet >= 3) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

/* jq_submit that gives up when Stop is pressed (the queue may stay full if no board takes work) */
static bool submit_stoppable(const nx_job_spec_t *j, int64_t end)
{
    while (!s_stop && now_us() < end)
        if (jq_submit(s_gq, j, pdMS_TO_TICKS(200))) return true;
    return false;
}

static int submit_spin_mix(int n, int salt, int64_t end)
{
    for (int i = 0; i < n; i++) {
        nx_job_spec_t j = { .type = NX_JOB_SPIN, .prio = (uint8_t)((i + salt) % 4) };
        j.p.spin.duration_ms = 60 + ((i + salt) * 37) % 90;
        j.budget_ms = j.p.spin.duration_ms * 3 + 1000;
        if (!submit_stoppable(&j, end)) return i;
    }
    return n;
}

static int submit_matmul(int n, uint16_t size, int64_t end)
{
    for (int i = 0; i < n; i++) {
        nx_job_spec_t j = { .type = NX_JOB_MATMUL, .prio = 1, .budget_ms = 3000 };
        j.p.matmul.n = size;
        j.p.matmul.seed = 1 + (uint32_t)i;
        if (!submit_stoppable(&j, end)) return i;
    }
    return n;
}

typedef struct { int64_t end; int salt, submitted; bool start; } burst_t;

static void burst_task(void *arg)
{
    burst_t *p = arg;
    xSemaphoreGive(s_burst_ready);
    xSemaphoreTake(s_burst_go, portMAX_DELAY);
    if (p->start) p->submitted = submit_spin_mix(T2_JOBS_EACH, p->salt, p->end);
    xSemaphoreGive(s_burst_done);
    vTaskDelete(NULL);
}

static uint16_t submit_t2(int64_t end, nx_run_t *run)
{
    burst_t args[T2_PRODUCERS] = {0};
    int made = 0;
    for (int i = 0; i < T2_PRODUCERS; i++) {
        args[i].end = end;
        args[i].salt = i * 11;
        if (xTaskCreate(burst_task, "burst", 3072, &args[i], NX_PRIO_CLIENT, NULL) != pdPASS) break;
        made++;
    }
    for (int i = 0; i < made; i++) xSemaphoreTake(s_burst_ready, portMAX_DELAY);
    for (int i = 0; i < made; i++) args[i].start = made == T2_PRODUCERS;
    for (int i = 0; i < made; i++) xSemaphoreGive(s_burst_go);
    uint16_t frame = made == T2_PRODUCERS ? producers_frame_now(VIEW_X, VIEW_Y, VIEW_W, &s_stop, end) : 0;
    for (int i = 0; i < made; i++) xSemaphoreTake(s_burst_done, portMAX_DELAY);
    for (int i = 0; i < made; i++)
        if (args[i].submitted == T2_JOBS_EACH) run->producers++;
    return frame;
}

static void set_status(uint8_t suite, uint16_t run, uint16_t of, const char *label)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_st.busy = true;
    s_st.suite = suite;
    s_st.run = run;
    s_st.of = of;
    strlcpy(s_st.label, label, sizeof(s_st.label));
    xSemaphoreGive(s_mtx);
}

/* ------------------------------ one run ------------------------------ */

static void do_run(uint8_t suite, const char *label, const cfg_t *c, work_t work, int param, uint16_t size)
{
    nx_run_t r = { .suite = suite, .pass = -1 };
    strlcpy(r.label, label, sizeof(r.label));

    apply(c);
    if (!wait_ready(c, false, 30000)) {
        snprintf(r.label, sizeof(r.label), "%.20s (%s)", label, s_stop ? "stopped" : "skipped");
        nx_log("EXP", NX_LOG_WARN, "%s %s", label, s_stop ? "stopped" : "skipped: a required worker is not online");
        goto store;
    }
    if (s_stop || !wait_idle(30000)) {
        snprintf(r.label, sizeof(r.label), "%.20s (%s)", label, s_stop ? "stopped" : "not idle");
        nx_log("EXP", NX_LOG_WARN, "%s %s", label, s_stop ? "stopped" : "skipped: cluster not idle");
        goto store;
    }

    if (!wait_ready(c, true, 12000)) {
        snprintf(r.label, sizeof(r.label), "%.16s (slot timeout)", label);
        nx_log("EXP", NX_LOG_ERR, "%s: required slot count was not acknowledged", label);
        goto store;
    }
    r.slots = c->k;
    if (work == W_FRAME_SPIN) {
        /* Seed a full, identical backlog before any scheduler can consume it. */
        cluster_set_worker_drain(true, true, true);
    }

    cluster_view(s_nv);
    uint32_t failures[NX_NODES];
    for (uint8_t i = 1; i < NX_NODES; i++) failures[i] = s_nv[i].failures;
    r.id = s_next_id++;
    if (s_next_id == 0) s_next_id = 1;
    nx_stats_set_run(r.id);
    uint32_t c0 = nx_stats_count(), d0 = nx_stats_dups();
    uint16_t ready, inflight;
    uint32_t f0, rt0, f1, rt1;
    jq_counts(s_gq, &ready, &inflight, &f0, &rt0);
    int64_t t0 = now_us();
    const int64_t t_end = t0 + RUN_TIMEOUT_US;
    uint16_t frame_no = 0;
    bool submitted = false;
    uint32_t fault_ticket = 0, old_inc = 0;
    uint8_t fault_mac[6] = {0};

    switch (work) {
    case W_FRAME:
    case W_FRAME_CRASH:
        r.jobs = NX_TILES;
        frame_no = producers_frame_now(VIEW_X, VIEW_Y, VIEW_W, &s_stop, t_end);
        submitted = frame_no != 0;
        break;
    case W_FRAME_SPIN:
        r.jobs = NX_TILES + param;
        frame_no = producers_frame_now(VIEW_X, VIEW_Y, VIEW_W, &s_stop, t_end);
        submitted = frame_no && submit_spin_mix(param, 0, t_end) == param;
        apply(c);
        break;
    case W_FRAME_PRODUCERS:
        r.jobs = NX_TILES + T2_PRODUCERS * T2_JOBS_EACH;
        frame_no = submit_t2(t_end, &r);
        submitted = frame_no && r.producers == T2_PRODUCERS;
        break;
    case W_MATMUL:
        r.jobs = (uint32_t)param;
        submitted = submit_matmul(param, size, t_end) == param;
        break;
    }

    while (submitted && !s_stop && now_us() < t_end) {
        jq_counts(s_gq, &ready, &inflight, &f1, &rt1);
        if ((nx_stats_count() - c0) + (f1 - f0) >= r.jobs) break;
        if (work == W_FRAME_CRASH && !fault_ticket) {
            nx_frame_view_t fv;
            frame_view(&fv);
            cluster_view(s_nv);
            if (fv.frame == frame_no && fv.done >= NX_TILES * 2 / 5 && fv.done < NX_TILES &&
                s_nv[1].state == NODE_ONLINE && jq_count_on_node(s_gq, 1) > 0) {
                old_inc = s_nv[1].inc;
                memcpy(fault_mac, s_nv[1].mac, sizeof(fault_mac));
                fault_ticket = cluster_reboot(1);
                if (fault_ticket) {
                    r.fault_tiles = fv.done;
                    cluster_set_drain(1, true); /* recovered work must land on B */
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(work == W_FRAME_CRASH ? 10 : 40));
    }

    bool fault_confirmed = false;
    if (fault_ticket) {
        int64_t rejoin_end = now_us() + 15000000;
        while (!s_stop && now_us() < rejoin_end) {
            cluster_view(s_nv);
            nx_node_view_t *a = &s_nv[1];
            if (a->failure_ticket == fault_ticket && a->failure_at_us >= t0) {
                r.detect_ms = a->failure_detect_ms;
                r.requeued = a->failure_requeued;
                r.recovered = a->failure_recovered;
                if (a->failure_recovery_at_us >= a->failure_at_us)
                    r.recovery_ms = (uint32_t)((a->failure_recovery_at_us - a->failure_at_us) / 1000);
                fault_confirmed = a->state == NODE_ONLINE && a->inc != old_inc &&
                                  memcmp(a->mac, fault_mac, sizeof(fault_mac)) == 0 &&
                                  r.requeued > 0 && r.recovered > 0;
                if (fault_confirmed) break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    /* ---- summary ---- */
    nx_window_t w;
    nx_stats_window(c0, nx_stats_count(), t0, r.id, &w);
    jq_counts(s_gq, &ready, &inflight, &f1, &rt1);
    r.committed = w.jobs;
    r.failed = f1 - f0;
    r.retries = rt1 - rt0;
    r.dups = nx_stats_dups() - d0;
    r.makespan_ms = w.makespan_ms;
    r.throughput = w.throughput;
    r.mean_ms = w.mean_ms;
    r.p95_ms = w.p95_ms;
    r.max_ms = w.max_ms;
    memcpy(r.mean_wait_ms, w.mean_wait_ms, sizeof(r.mean_wait_ms));
    memcpy(r.max_wait_ms, w.max_wait_ms, sizeof(r.max_wait_ms));
    memcpy(r.by_node, w.by_node, sizeof(r.by_node));
    if (work != W_MATMUL) {
        nx_frame_view_t fv;
        frame_view(&fv);
        r.crc = (fv.frame == frame_no && fv.done == fv.total) ? fv.crc : 0;   /* someone else's frame never counts */
    }
    cluster_view(s_nv);
    bool interrupted = false;
    bool all_participated = true;
    for (uint8_t i = 0; i < NX_NODES; i++) {
        if (!c->use[i]) continue;
        if (r.by_node[i] == 0) all_participated = false;
        if (i != NX_NODE_LOCAL && !(work == W_FRAME_CRASH && i == 1) && s_nv[i].failures != failures[i])
            interrupted = true;
    }
    bool complete = submitted && !s_stop && !interrupted && r.committed == r.jobs &&
                    r.failed == 0 && w.repeated_ids == 0;
    bool crc_match = s_ref_crc != 0 && r.crc == s_ref_crc;
    if (work == W_FRAME_PRODUCERS)
        r.pass = complete && crc_match && all_participated;
    else if (work == W_FRAME_CRASH)
        r.pass = complete && crc_match && fault_confirmed;
    else if (!complete || !all_participated)
        r.pass = 0;
    nx_log("EXP", r.pass == 0 ? NX_LOG_ERR : NX_LOG_OK, "%s: %lu/%lu jobs in %lu ms, %.1f jobs/s, p95 %lu ms%s",
           r.label, (unsigned long)r.committed, (unsigned long)r.jobs, (unsigned long)r.makespan_ms,
           (double)r.throughput, (unsigned long)r.p95_ms, r.pass == 1 ? ", PASS" : r.pass == 0 ? ", FAIL" : "");

store:
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_runs[s_nruns % NX_EXP_MAX_RUNS] = r;
    s_nruns++;
    s_st.n_runs = s_nruns < NX_EXP_MAX_RUNS ? s_nruns : NX_EXP_MAX_RUNS;
    xSemaphoreGive(s_mtx);
}

/* ------------------------------ suites ------------------------------ */

static const cfg_t TWO_WORKERS = { NX_ORDER_AGING, NX_PLACE_LEAST_LOADED, NX_DEFAULT_SLOTS, { false, true, true } };
static const cfg_t ALL_WORKERS = { NX_ORDER_AGING, NX_PLACE_LEAST_LOADED, NX_DEFAULT_SLOTS, { false, true, true, true } };
static const cfg_t SINGLE_NODE = { NX_ORDER_AGING, NX_PLACE_LEAST_LOADED, NX_DEFAULT_SLOTS, { true } };

/* Render the fixed view on the master alone; its image CRC is the reference for T2/T3. */
static bool reference_run(uint8_t suite, uint16_t run, uint16_t of)
{
    set_status(suite, run, of, "single-node reference");
    do_run(suite, "reference (single node)", &SINGLE_NODE, W_FRAME, 0, 0);
    const nx_run_t *r = s_nruns ? &s_runs[(s_nruns - 1) % NX_EXP_MAX_RUNS] : NULL;
    s_ref_crc = r && r->pass != 0 ? r->crc : 0;
    if (s_stop || s_ref_crc == 0) {
        nx_log("EXP", NX_LOG_ERR, "reference frame incomplete: T%u aborted", suite);
        return false;
    }
    return true;
}

static void run_suite(uint8_t suite)
{
    char label[32];
    int n = 0;
    switch (suite) {
    case 1: {                                                    /* T1 scheduling */
        const int of = REPS * 3 * 2;
        for (int rep = 1; rep <= REPS && !s_stop; rep++)
            for (uint8_t o = 0; o < 3 && !s_stop; o++)
                for (uint8_t p = 0; p < 2 && !s_stop; p++) {
                    cfg_t c = TWO_WORKERS;
                    c.ord = o;
                    c.plc = p;
                    snprintf(label, sizeof(label), "T1 %s+%s #%d", ORD[o], PLC[p], rep);
                    set_status(suite, (uint16_t)++n, of, label);
                    do_run(suite, label, &c, W_FRAME_SPIN, 60, 0);
                }
        break;
    }
    case 2:                                                      /* T2 correctness */
        if (!reference_run(suite, 1, 2)) break;
        set_status(suite, 2, 2, "T2 2 workers + 4 producers");
        do_run(suite, "T2 2 workers + 4 producers", &TWO_WORKERS, W_FRAME_PRODUCERS, 0, 0);
        break;
    case 3:                                                      /* T3 recovery */
        if (!reference_run(suite, 1, 2)) break;
        set_status(suite, 2, 2, "T3 crash Worker A at 40%");
        do_run(suite, "T3 crash Worker A at 40%", &TWO_WORKERS, W_FRAME_CRASH, 0, 0);
        cluster_set_drain(1, false);
        break;
    case 4: {                                                    /* T4 slots */
        const int of = REPS * 4;
        for (int rep = 1; rep <= REPS && !s_stop; rep++)
            for (uint8_t k = 1; k <= 4 && !s_stop; k++) {
                cfg_t c = ALL_WORKERS;
                c.k = k;
                snprintf(label, sizeof(label), "T4 K=%u #%d", k, rep);
                set_status(suite, (uint16_t)++n, of, label);
                do_run(suite, label, &c, W_MATMUL, 120, 32);
            }
        break;
    }
    case 5: {                                                    /* T5 scalability */
        static const uint16_t sizes[] = { 12, 32 };
        const int of = REPS * 2 * NX_NODES;
        for (int rep = 1; rep <= REPS && !s_stop; rep++)
            for (int s = 0; s < 2 && !s_stop; s++)
                for (int nodes = 0; nodes <= NX_MAX_WORKERS && !s_stop; nodes++) {
                    cfg_t c = nodes == 0 ? SINGLE_NODE : ALL_WORKERS;
                    for (int i = 1; i < NX_NODES; i++) c.use[i] = i <= nodes;
                    snprintf(label, sizeof(label), "T5 n=%u %d worker%s #%d", sizes[s], nodes, nodes == 1 ? "" : "s", rep);
                    set_status(suite, (uint16_t)++n, of, label);
                    do_run(suite, label, &c, W_MATMUL, 100, sizes[s]);
                }
        break;
    }
    }
}

static void runner_task(void *arg)
{
    uint8_t suite;
    for (;;) {
        xQueueReceive(s_req, &suite, portMAX_DELAY);
        cfg_t previous = { .ord = jq_get_ordering(s_gq), .plc = cluster_placement() };
        cluster_view(s_nv);
        uint8_t old_slots[NX_NODES];
        for (int i = 0; i < NX_NODES; i++) {
            old_slots[i] = s_nv[i].requested_k ? s_nv[i].requested_k : s_nv[i].k;
            previous.use[i] = i == NX_NODE_LOCAL ? cluster_local() : !s_nv[i].drain;
        }
        uint8_t old_clients = producers_clients();
        bool old_auto = producers_auto();
        producers_set_clients(0);              /* every commit must belong to the run */
        producers_set_auto(false);
        producers_pause_frames(true);
        nx_log("EXP", NX_LOG_INFO, "suite T%u started (clients and auto-zoom paused)", suite);
        if (wait_idle(60000)) run_suite(suite);
        else nx_log("EXP", NX_LOG_WARN, "cluster did not become idle");

        nx_stats_set_run(0);
        apply(&previous);
        for (uint8_t i = 0; i < NX_NODES; i++)
            if (old_slots[i]) cluster_set_slots(i, old_slots[i]);
        producers_set_clients(old_clients);
        producers_set_auto(old_auto);
        producers_pause_frames(false);
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_st.busy = false;
        xSemaphoreGive(s_mtx);
        nx_log("EXP", NX_LOG_OK, "suite T%u %s; results at /api/runs.csv and /api/csv", suite, s_stop ? "stopped" : "finished");
    }
}

/* ------------------------------ public API ------------------------------ */

void exp_start(nx_job_queue_t *gq)
{
    s_gq = gq;
    s_req = xQueueCreate(1, sizeof(uint8_t));
    s_mtx = xSemaphoreCreateMutex();
    s_burst_ready = xSemaphoreCreateCounting(T2_PRODUCERS, 0);
    s_burst_go = xSemaphoreCreateCounting(T2_PRODUCERS, 0);
    s_burst_done = xSemaphoreCreateCounting(T2_PRODUCERS, 0);
    if (!s_req || !s_mtx || !s_burst_ready || !s_burst_go || !s_burst_done) abort();
    if (xTaskCreate(runner_task, "experiments", 4096, NULL, NX_PRIO_CLIENT, NULL) != pdPASS) abort();
}

bool exp_request(uint8_t suite)
{
    if (suite < 1 || suite > 5) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool busy = s_st.busy;
    if (!busy) {
        s_stop = false;
        s_st.busy = true;
        s_st.suite = suite;
        s_st.run = 0;
        s_st.of = 0;
        s_st.label[0] = 0;
        if (xQueueSend(s_req, &suite, 0) != pdTRUE) {
            s_st.busy = false;
            busy = true;
        }
    }
    xSemaphoreGive(s_mtx);
    return !busy;
}

void exp_stop(void) { s_stop = true; }

void exp_status(nx_exp_status_t *st)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *st = s_st;
    xSemaphoreGive(s_mtx);
}

int exp_recent(nx_run_t *out, int max)
{
    int n = 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (int k = 0; k < max && k < (int)s_nruns && k < NX_EXP_MAX_RUNS; k++)
        out[n++] = s_runs[(s_nruns - 1 - k) % NX_EXP_MAX_RUNS];
    xSemaphoreGive(s_mtx);
    return n;
}

int exp_runs_csv(char *buf, int cap)
{
    int len = snprintf(buf, cap, "run,suite,label,jobs,committed,failed,retries,dups,makespan_ms,throughput,"
                       "mean_ms,p95_ms,max_ms,wait_p0,wait_p1,wait_p2,wait_p3,maxwait_p0,maxwait_p1,maxwait_p2,maxwait_p3,"
                       "by_master,by_a,by_b,crc,detect_ms,pass,requeued,recovered,recovery_ms,fault_tiles,producers,confirmed_k,by_c\n");
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int total = s_nruns < NX_EXP_MAX_RUNS ? s_nruns : NX_EXP_MAX_RUNS;
    for (int k = total - 1; k >= 0 && len < cap - 300; k--) {
        const nx_run_t *r = &s_runs[(s_nruns - 1 - k) % NX_EXP_MAX_RUNS];
        len += snprintf(buf + len, cap - len,
                        "%u,T%u,%s,%lu,%lu,%lu,%lu,%lu,%lu,%.2f,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%08lx,%lu,%d,%lu,%lu,%lu,%u,%u,%u,%lu\n",
                        r->id, r->suite, r->label, (unsigned long)r->jobs, (unsigned long)r->committed,
                        (unsigned long)r->failed, (unsigned long)r->retries, (unsigned long)r->dups,
                        (unsigned long)r->makespan_ms, (double)r->throughput, (unsigned long)r->mean_ms,
                        (unsigned long)r->p95_ms, (unsigned long)r->max_ms,
                        (unsigned long)r->mean_wait_ms[0], (unsigned long)r->mean_wait_ms[1],
                        (unsigned long)r->mean_wait_ms[2], (unsigned long)r->mean_wait_ms[3],
                        (unsigned long)r->max_wait_ms[0], (unsigned long)r->max_wait_ms[1],
                        (unsigned long)r->max_wait_ms[2], (unsigned long)r->max_wait_ms[3],
                        (unsigned long)r->by_node[0], (unsigned long)r->by_node[1], (unsigned long)r->by_node[2],
                        (unsigned long)r->crc, (unsigned long)r->detect_ms, r->pass,
                        (unsigned long)r->requeued, (unsigned long)r->recovered,
                        (unsigned long)r->recovery_ms, r->fault_tiles, r->producers, r->slots, (unsigned long)r->by_node[3]);
    }
    xSemaphoreGive(s_mtx);
    return len;
}
