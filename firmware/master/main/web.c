/*
 * web.c — HTTP + WebSocket server for the dashboard.
 *
 * ESP-IDF runs every handler in one "httpd" task, so handlers only copy state
 * or post a command and return. A separate telemetry task builds the JSON
 * state up to four times a second and asks the httpd task to push it to every
 * WebSocket client (httpd_queue_work), because only that task may send.
 */
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "proto.h"
#include "logger.h"
#include "cluster.h"
#include "producers.h"
#include "stats.h"
#include "experiments.h"
#include "web.h"

#define JSON_CAP   16384
#define TILES_CAP  (8 + 8 * (4 + NX_TILE_BYTES))

extern const uint8_t index_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_gz_end[]   asm("_binary_index_html_gz_end");

static httpd_handle_t    s_server;
static nx_job_queue_t   *s_gq;
static SemaphoreHandle_t s_build_mtx;    /* build_state() uses static buffers  */
static char             *s_json;         /* scratch for /api/state             */
static char             *s_push;         /* latest state for WebSocket clients */
static int               s_push_len;
static atomic_bool       s_ws_active;
static atomic_bool       s_ws_pending;
static uint8_t          *s_tiles;
#define RUN_CSV_CAP (NX_EXP_MAX_RUNS * 300 + 450)
static char             *s_run_csv;

/* ------------------------------ JSON writer ------------------------------ */

typedef struct { char *b; int cap, len; } jw_t;

static void jw(jw_t *w, const char *fmt, ...)
{
    if (w->len >= w->cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->b + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n > 0) w->len = (w->len + n < w->cap - 1) ? w->len + n : w->cap - 1;
}

static void jw_str(jw_t *w, const char *s)
{
    jw(w, "\"");
    for (; *s && w->len < w->cap - 8; s++) {
        if (*s == '"' || *s == '\\') jw(w, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) jw(w, " ");
        else w->b[w->len++] = *s;
    }
    w->b[w->len] = 0;
    jw(w, "\"");
}

/* ------------------------------ state ------------------------------ */

static int build_state(char *buf, int cap, uint32_t *log_cursor, int max_logs)
{
    static nx_queue_view_t qv;
    static nx_node_view_t nv[NX_NODES];
    static nx_stats_view_t sv;
    static nx_frame_view_t fv;
    static nx_log_rec_t logs[24];
    int64_t now = esp_timer_get_time();

    xSemaphoreTake(s_build_mtx, portMAX_DELAY);
    jq_view(s_gq, &qv, now);                 /* each view takes its own lock, one at a time */
    cluster_view(nv);
    nx_stats_view(&sv);
    frame_view(&fv);
    uint16_t inflight[NX_NODES];
    for (int i = 0; i < NX_NODES; i++) inflight[i] = jq_count_on_node(s_gq, (uint8_t)i);
    if (max_logs > 24) max_logs = 24;
    int nlogs = logger_recent(logs, max_logs, log_cursor);

    jw_t w = { buf, cap, 0 };
    jw(&w, "{\"t\":%lld,\"ord\":%u,\"plc\":%u,\"cli\":%u,\"local\":%d,\"auto\":%d,\"blocked\":%lu,",
       now / 1000, qv.ordering, cluster_placement(), producers_clients(), cluster_local(), producers_auto(),
       (unsigned long)producers_blocked());

    jw(&w, "\"q\":{\"n\":%u,\"cap\":%u,\"inf\":%u,\"lock\":%lu,\"cont\":%lu,\"ret\":%lu,\"fail\":%lu,\"canc\":%lu,\"byp\":[%u,%u,%u,%u],\"old\":[%lu,%lu,%lu,%lu],\"top\":[",
       qv.ready_count, qv.ready_cap, qv.inflight_count, (unsigned long)qv.lock_ops, (unsigned long)qv.lock_contended,
       (unsigned long)qv.retries, (unsigned long)qv.failed, (unsigned long)qv.cancelled,
       qv.ready_by_prio[0], qv.ready_by_prio[1], qv.ready_by_prio[2], qv.ready_by_prio[3],
       (unsigned long)qv.oldest_wait_ms[0], (unsigned long)qv.oldest_wait_ms[1], (unsigned long)qv.oldest_wait_ms[2], (unsigned long)qv.oldest_wait_ms[3]);
    for (int i = 0; i < qv.n_top; i++) {
        const nx_job_t *j = &qv.top[i];
        jw(&w, "%s[%lu,%u,%u,%d,%u,%u,%u,%u]", i ? "," : "", (unsigned long)j->spec.id, j->spec.type, j->spec.prio,
           jq_effective_priority(qv.ordering, j, now), j->spec.attempt,
           j->spec.type == NX_JOB_FRACTAL ? j->spec.p.fractal.tx : 0, j->spec.type == NX_JOB_FRACTAL ? j->spec.p.fractal.ty : 0,
           j->spec.type == NX_JOB_FRACTAL ? j->spec.p.fractal.frame : 0);
    }
    jw(&w, "],\"run\":[");
    for (int i = 0; i < qv.n_inflight; i++) {
        const nx_job_t *j = &qv.inflight[i];
        jw(&w, "%s[%lu,%u,%u,%u,%u,%u,%lld,%u,%u,%u]", i ? "," : "", (unsigned long)j->spec.id, j->spec.type, j->spec.prio,
           j->node, j->spec.attempt, j->state, (now - j->t_dispatch) / 1000,
           j->spec.type == NX_JOB_FRACTAL ? j->spec.p.fractal.tx : 0, j->spec.type == NX_JOB_FRACTAL ? j->spec.p.fractal.ty : 0,
           j->spec.type == NX_JOB_FRACTAL ? j->spec.p.fractal.frame : 0);
    }
    jw(&w, "],\"failed\":[");
    for (int i = 0; i < qv.n_failed; i++)
        jw(&w, "%s[%lu,%u,%u,%u]", i ? "," : "", (unsigned long)qv.failed_jobs[i].spec.id, qv.failed_jobs[i].spec.type,
           qv.failed_jobs[i].spec.prio, qv.failed_jobs[i].spec.attempt);
    jw(&w, "]},\"nodes\":[");

    for (int i = 0; i < NX_NODES; i++) {
        const nx_node_view_t *n = &nv[i];
        const nx_heartbeat_t *hb = &n->hb;
        esp_ip4_addr_t ip = { .addr = n->ip };
        jw(&w, "%s{\"name\":", i ? "," : "");
        jw_str(&w, n->name);
        jw(&w, ",\"st\":%u,\"ip\":\"" IPSTR "\",\"mac\":\"" MACSTR "\",\"inc\":\"%08lx\",\"k\":%u,\"n\":%u,\"drain\":%d,\"cred\":%d,"
               "\"hb\":%lld,\"disp\":%lu,\"res\":%lu,\"dup\":%lu,",
           n->state, IP2STR(&ip), MAC2STR(n->mac), (unsigned long)n->inc, n->k, n->n, n->drain,
           (int)n->k + 1 - (int)inflight[i], n->last_heard ? (now - n->last_heard) / 1000 : -1LL,
           (unsigned long)n->dispatched, (unsigned long)n->results, (unsigned long)n->duplicates);
        const char *chip = n->cores ? (n->chip == NX_CHIP_ESP32S3 ? "ESP32-S3" : n->chip == NX_CHIP_ESP32 ? "ESP32" : "ESP") : "";
        jw(&w, "\"chip\":\"%s\",\"cores\":%u,\"mhz\":%u,\"target_k\":%u,\"slots_ready\":%d,",
           chip, n->cores, n->cpu_mhz, n->requested_k, n->slots_ready);
        if (n->hb_valid) {
            jw(&w, "\"run\":%u,\"peak\":%u,\"done\":%lu,\"tout\":%lu,\"heap\":%lu,\"up\":%lu,\"cpu\":[%u,%u],\"ids\":[",
               hb->running, hb->peak_running, (unsigned long)hb->completed, (unsigned long)hb->timeouts,
               (unsigned long)hb->heap_free, (unsigned long)hb->uptime_ms, hb->core_load[0], hb->core_load[1]);
            for (int k = 0; k < hb->n_running_ids && k < NX_POOL_SIZE; k++) jw(&w, "%s%lu", k ? "," : "", (unsigned long)hb->running_ids[k]);
            const nx_lab_results_t *L = &hb->lab;
            jw(&w, "],\"lab\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%u],\"tasks\":[",
               (unsigned long)L->a_counter, (unsigned long)L->a_us, (unsigned long)L->b_counter, (unsigned long)L->b_us,
               (unsigned long)L->c_peak, (unsigned long)L->c_jobs, (unsigned long)L->d_bin_us, (unsigned long)L->d_mutex_us, L->done_mask);
            for (int k = 0; k < hb->ntasks && k < NX_MAX_TASKS; k++) {
                const nx_task_info_t *t = &hb->tasks[k];
                char name[13];
                memcpy(name, t->name, 12);
                name[12] = 0;
                jw(&w, "%s[", k ? "," : "");
                jw_str(&w, name);
                jw(&w, ",%u,%u,%d,%u,%u]", t->state, t->prio, t->core, t->cpu_pct, t->stack_free);
            }
            jw(&w, "]");
        } else {
            jw(&w, "\"run\":0,\"tasks\":[]");
        }
        jw(&w, "}");
    }

    jw(&w, "],\"st\":{\"thr\":%.2f,\"p95\":%lu,\"mean\":%lu,\"done\":%lu,\"dup\":%lu,\"wait\":[%lu,%lu,%lu,%lu],\"recent\":[",
       (double)sv.throughput, (unsigned long)sv.p95_ms, (unsigned long)sv.mean_ms, (unsigned long)sv.committed,
       (unsigned long)sv.duplicates, (unsigned long)sv.mean_wait_ms[0], (unsigned long)sv.mean_wait_ms[1],
       (unsigned long)sv.mean_wait_ms[2], (unsigned long)sv.mean_wait_ms[3]);
    for (int i = 0; i < sv.n_recent; i++) {
        const nx_done_rec_t *r = &sv.recent[i];
        jw(&w, "%s[%lu,%u,%u,%u,%lu,%u,%u,%u]", i ? "," : "", (unsigned long)r->id, r->type, r->prio, r->node,
           (unsigned long)r->latency_ms, r->attempt, r->tx, r->ty);
    }
    jw(&w, "],\"fail\":");
    if (sv.has_failure)
        jw(&w, "{\"node\":%u,\"det\":%lu,\"req\":%d,\"inj\":%d,\"age\":%lu}", sv.fail_node, (unsigned long)sv.fail_detect_ms,
           sv.fail_requeued, sv.fail_injected, (unsigned long)sv.fail_age_ms);
    else
        jw(&w, "null");

    jw(&w, "},\"fr\":{\"no\":%u,\"done\":%u,\"tot\":%u,\"ver\":%lu,\"ms\":%lu,\"zoom\":%.1f,\"it\":%u,\"own\":\"",
       fv.frame, fv.done, fv.total, (unsigned long)fv.version, (unsigned long)fv.elapsed_ms, (double)fv.zoom, fv.max_iter);
    for (int t = 0; t < NX_TILES && w.len < w.cap - 2; t++) w.b[w.len++] = (char)('0' + fv.owner[t]);
    w.b[w.len] = 0;
    jw(&w, "\"},");

    nx_exp_status_t es;
    static nx_run_t runs[6];
    exp_status(&es);
    int nr = exp_recent(runs, 6);
    jw(&w, "\"exp\":{\"busy\":%d,\"suite\":%u,\"run\":%u,\"of\":%u,\"label\":", es.busy, es.suite, es.run, es.of);
    jw_str(&w, es.label);
    jw(&w, "},\"runs\":[");
    for (int i = 0; i < nr; i++) {
        const nx_run_t *r = &runs[i];
        jw(&w, "%s[%u,%u,", i ? "," : "", r->id, r->suite);
        jw_str(&w, r->label);
        jw(&w, ",%.2f,%lu,%lu,%lu,%lu,%lu,\"%08lx\",%d,%lu,%lu,%lu,%lu,[", (double)r->throughput, (unsigned long)r->mean_ms,
           (unsigned long)r->p95_ms, (unsigned long)r->makespan_ms, (unsigned long)r->committed, (unsigned long)r->jobs,
           (unsigned long)r->crc, r->pass, (unsigned long)r->detect_ms,
           (unsigned long)r->requeued, (unsigned long)r->recovered, (unsigned long)r->recovery_ms);
        for (int node = 0; node < NX_NODES; node++)
            jw(&w, "%s%lu", node ? "," : "", (unsigned long)r->by_node[node]);
        jw(&w, "]]");
    }
    jw(&w, "],\"log\":[");
    for (int i = 0; i < nlogs; i++) {
        jw(&w, "%s[%lu,%lld,", i ? "," : "", (unsigned long)logs[i].seq, logs[i].t_us / 1000);
        jw_str(&w, logs[i].tag);
        jw(&w, ",%u,", logs[i].level);
        jw_str(&w, logs[i].msg);
        jw(&w, "]");
    }
    jw(&w, "],\"logq\":%lu,\"drop\":%lu}", (unsigned long)logger_queue_depth(), (unsigned long)logger_dropped());
    xSemaphoreGive(s_build_mtx);
    return w.len < cap - 1 ? w.len : 0;
}

/* ------------------------------ handlers ------------------------------ */

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)index_gz_start, index_gz_end - index_gz_start);
}

static bool query(httpd_req_t *req, const char *key, char *val, size_t len)
{
    char q[160];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    return httpd_query_key_value(q, key, val, len) == ESP_OK;
}

static esp_err_t state_get(httpd_req_t *req)
{
    uint32_t cursor = 0;
    char v[16];
    if (query(req, "log", v, sizeof(v))) cursor = (uint32_t)strtoul(v, NULL, 10);
    int len = build_state(s_json, JSON_CAP, &cursor, 20);
    if (!len) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Telemetry exceeded buffer");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s_json, len);
}

static esp_err_t tiles_get(httpd_req_t *req)
{
    char v[16];
    uint32_t since = query(req, "since", v, sizeof(v)) ? (uint32_t)strtoul(v, NULL, 10) : 0;
    int len = frame_tiles_since(since, s_tiles, TILES_CAP);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)s_tiles, len);
}

static esp_err_t csv_get(httpd_req_t *req)
{
    static char chunk[4096];
    uint32_t cursor = 0, end = nx_stats_count();
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"nexus_jobs.csv\"");
    for (;;) {
        int n = nx_stats_csv(chunk, sizeof(chunk), &cursor, end);
        if (n <= 0) break;
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) return ESP_FAIL;
        if (cursor >= end) break;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t runs_csv_get(httpd_req_t *req)
{
    int len = exp_runs_csv(s_run_csv, RUN_CSV_CAP);
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"nexus_runs.csv\"");
    return httpd_resp_send(req, s_run_csv, len);
}

static esp_err_t control_conflict(httpd_req_t *req, const char *message)
{
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, message);
}

static esp_err_t cmd_get(httpd_req_t *req)
{
    char op[16] = "", val[16] = "0", nd[8] = "0", x[16] = "0.5", y[16] = "0.5";
    query(req, "op", op, sizeof(op));
    query(req, "v", val, sizeof(val));
    query(req, "node", nd, sizeof(nd));
    query(req, "x", x, sizeof(x));
    query(req, "y", y, sizeof(y));
    /* an experiment owns the cluster: other controls would mix extra jobs into its runs */
    nx_exp_status_t es;
    exp_status(&es);
    if (es.busy && strcmp(op, "expstop"))
        return control_conflict(req, "Experiment running");
    char *end_v, *end_node;
    long v = strtol(val, &end_v, 10);
    long node_i = strtol(nd, &end_node, 10);
    if (end_v == val || *end_v || end_node == nd || *end_node || node_i < 0 || node_i >= NX_NODES)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid control arguments");
    uint8_t node = (uint8_t)node_i;
    bool ok = true;
    static const char *ord_names[] = { "FCFS", "strict priority", "priority + aging" };

    if (!strcmp(op, "ord") && v >= 0 && v <= 2) {
        jq_set_ordering(s_gq, (uint8_t)v);
        nx_log("MST", NX_LOG_INFO, "ordering policy -> %s", ord_names[v]);
    } else if (!strcmp(op, "plc") && v >= 0 && v <= 1) {
        cluster_set_placement((uint8_t)v);
        nx_log("MST", NX_LOG_INFO, "placement policy -> %s", v ? "least-loaded" : "round-robin");
    } else if (!strcmp(op, "k") && v >= 1 && v <= NX_POOL_SIZE) ok = cluster_set_slots(node, (uint8_t)v);
    else if (!strcmp(op, "drain"))     cluster_set_drain(node, v != 0);
    else if (!strcmp(op, "local"))     cluster_set_local(v != 0);
    else if (!strcmp(op, "cli") && v >= 0 && v <= 4) producers_set_clients((uint8_t)v);
    else if (!strcmp(op, "storm"))     producers_storm(8000);
    else if (!strcmp(op, "runaway"))   ok = producers_runaway();
    else if (!strcmp(op, "zoom")) {
        char *ex, *ey;
        float fx = strtof(x, &ex), fy = strtof(y, &ey);
        ok = ex != x && !*ex && ey != y && !*ey && producers_zoom_at(fx, fy);
    }
    else if (!strcmp(op, "auto"))      producers_set_auto(v != 0);
    else if (!strcmp(op, "mute") && v >= 0 && v <= 10000) ok = cluster_mute(node, v > 0 ? (uint32_t)v : 4000);
    else if (!strcmp(op, "reboot"))    ok = cluster_reboot(node) != 0;
    else if (!strcmp(op, "lab") && v >= 0 && v <= 3) ok = cluster_run_lab(node, (uint8_t)v);
    else if (!strcmp(op, "exp") && v >= 1 && v <= 5) ok = exp_request((uint8_t)v);
    else if (!strcmp(op, "expstop"))   exp_stop();
    else return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown op");
    if (!ok) return control_conflict(req, "Control unavailable");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (httpd_ws_get_fd_info(req->handle, httpd_req_to_sockfd(req)) != HTTPD_WS_CLIENT_WEBSOCKET)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "WebSocket upgrade required");
    /* the dashboard sends nothing important; read and drop any frame */
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT };
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) return ESP_FAIL;
    if (f.len > 128) return ESP_FAIL;
    if (f.len) {
        uint8_t buf[128];
        f.payload = buf;
        return httpd_ws_recv_frame(req, &f, f.len);
    }
    return ESP_OK;
}

/* Runs inside the httpd task (queued by the telemetry task). */
static void ws_broadcast(void *arg)
{
    static bool reported_stream;
    int fds[8];
    size_t n = sizeof(fds) / sizeof(fds[0]);
    bool active = false;
    if (httpd_get_client_list(s_server, &n, fds) == ESP_OK) {
        httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .final = true,
                               .payload = (uint8_t *)s_push, .len = (size_t)s_push_len };
        for (size_t i = 0; i < n; i++) {
            if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
            /* A zero-length snapshot is only a client discovery pass. */
            if (!f.len || httpd_ws_send_frame_async(s_server, fds[i], &f) == ESP_OK) active = true;
            else httpd_sess_trigger_close(s_server, fds[i]);
        }
    }
    if (!active) reported_stream = false;
    else if (s_push_len && !reported_stream) {
        reported_stream = true;
        nx_log("WEB", NX_LOG_OK, "live dashboard telemetry sent (%d bytes)", s_push_len);
    }
    atomic_store(&s_ws_active, active);
    atomic_store(&s_ws_pending, false);
}

static void telemetry_task(void *arg)
{
    uint32_t cursor = 0;
    for (;;) {
        /* Match idle telemetry to the worker heartbeat rate. Use the faster
         * cadence while jobs move through the queue and paint the canvas. */
        bool busy = atomic_load(&s_ws_active) && jq_has_pending(s_gq);
        vTaskDelay(pdMS_TO_TICKS(busy ? 250 : NX_HEARTBEAT_MS));
        if (atomic_load(&s_ws_pending)) continue;
        /* ESP-IDF 6.1 handles the upgrade itself. Discover new sessions in the
         * HTTP task even when there is no established telemetry audience yet. */
        s_push_len = atomic_load(&s_ws_active) ? build_state(s_push, JSON_CAP, &cursor, 12) : 0;
        atomic_store(&s_ws_pending, true);
        if (httpd_queue_work(s_server, ws_broadcast, NULL) != ESP_OK)
            atomic_store(&s_ws_pending, false);
    }
}

void web_start(nx_job_queue_t *gq)
{
    s_gq = gq;
    s_build_mtx = xSemaphoreCreateMutex();
    s_json = heap_caps_malloc(JSON_CAP, MALLOC_CAP_SPIRAM);
    s_push = heap_caps_malloc(JSON_CAP, MALLOC_CAP_SPIRAM);
    s_tiles = heap_caps_malloc(TILES_CAP, MALLOC_CAP_SPIRAM);
    s_run_csv = heap_caps_malloc(RUN_CSV_CAP, MALLOC_CAP_SPIRAM);
    if (!s_build_mtx || !s_json || !s_push || !s_tiles || !s_run_csv) abort();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    /* Seven clients plus HTTP internals, the worker listener, three links,
     * and room for a worker reconnect stay within the 16 lwIP sockets. */
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;      /* idle browser connections are recycled */
    cfg.stack_size = 8192;
    cfg.send_wait_timeout = 2;
    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));

    const httpd_uri_t uris[] = {
        { .uri = "/",           .method = HTTP_GET, .handler = page_get },
        { .uri = "/ws",         .method = HTTP_GET, .handler = ws_handler, .is_websocket = true },
        { .uri = "/api/state",  .method = HTTP_GET, .handler = state_get },
        { .uri = "/api/tiles",  .method = HTTP_GET, .handler = tiles_get },
        { .uri = "/api/cmd",    .method = HTTP_GET, .handler = cmd_get },
        { .uri = "/api/csv",    .method = HTTP_GET, .handler = csv_get },
        { .uri = "/api/runs.csv", .method = HTTP_GET, .handler = runs_csv_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uris[i]));

    if (xTaskCreate(telemetry_task, "telemetry", NX_STACK_LINK, NULL, NX_PRIO_TELEMETRY, NULL) != pdPASS) abort();
}
