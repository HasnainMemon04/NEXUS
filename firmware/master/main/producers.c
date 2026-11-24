/*
 * producers.c — client tasks and the fractal frame producer.
 *
 * OS CONCEPT: producers of the bounded buffer. jq_submit() blocks while the
 * queue is full, which is exactly the "producer waits on sem_free" case.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "logger.h"
#include "producers.h"

#define MAX_CLIENTS 4
#define HOME_X      (-0.6f)
#define HOME_Y      (0.0f)
#define HOME_W      3.3f        /* width of the full view in the complex plane */
#define ZOOM_STEP   0.42f
#define AUTO_DEPTH  6

typedef struct { float cx, cy, w; } frame_req_t;

static nx_job_queue_t   *s_gq;
static atomic_uchar      s_clients = 1;
static int64_t           s_storm_until;
static portMUX_TYPE      s_control_mux = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool       s_auto = true, s_frames_paused;
static atomic_uint       s_blocked;
static QueueHandle_t     s_frame_req;

/* frame store */
static SemaphoreHandle_t s_frame_submit;
static SemaphoreHandle_t s_fmtx;
static uint8_t          *s_pixels;                /* 600 x 400, one byte per pixel */
static uint32_t          s_tile_ver[NX_TILES];
static uint32_t          s_frame_base;
static nx_frame_view_t   s_fv;
static float             s_cx = HOME_X, s_cy = HOME_Y, s_w = HOME_W;
static int64_t           s_t_start, s_t_end;

static const float TARGETS[][2] = {
    { -0.743643887f, 0.131825904f }, { -0.10109636f, 0.95628651f },
    { 0.2549870375f, -0.000567979f }, { -1.25066f, 0.02012f },
};

/* ------------------------------ client tasks ------------------------------ */

static void client_task(void *arg)
{
    const uint8_t me = (uint8_t)(uintptr_t)arg;
    for (;;) {
        if (me >= atomic_load(&s_clients)) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        int64_t now = esp_timer_get_time();
        taskENTER_CRITICAL(&s_control_mux);
        bool storm = now < s_storm_until;
        taskEXIT_CRITICAL(&s_control_mux);
        uint32_t r = esp_random();
        nx_job_spec_t j = { 0 };
        if (storm) {
            j.type = NX_JOB_SPIN; j.prio = 3; j.p.spin.duration_ms = 150 + r % 100;
        } else if (r % 3 == 0) {
            j.type = NX_JOB_MATMUL; j.prio = (uint8_t)(r % 3); j.p.matmul.n = (uint16_t)(24 + r % 9); j.p.matmul.seed = r;
        } else {
            j.type = NX_JOB_SPIN; j.prio = (uint8_t)(r % 3); j.p.spin.duration_ms = 100 + r % 250;
        }
        /* budget leaves room for time-slicing when K > 2 jobs share two cores */
        j.budget_ms = j.type == NX_JOB_SPIN ? j.p.spin.duration_ms * 3 + 1000 : 2000;

        int64_t t0 = esp_timer_get_time();
        while (me < atomic_load(&s_clients)) {
            if (jq_submit(s_gq, &j, pdMS_TO_TICKS(100))) {
                if (esp_timer_get_time() - t0 > 2000) atomic_fetch_add(&s_blocked, 1);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(storm ? 90 + r % 70 : 700 + r % 700));
    }
}

/* ------------------------------ frames ------------------------------ */

static uint16_t max_iter_for(float w)
{
    float it = 120.0f + 34.0f * log2f(HOME_W / w);
    if (it < 120) it = 120;
    if (it > 420) it = 420;
    return (uint16_t)it;
}

static uint16_t submit_frame(const frame_req_t *r, const atomic_bool *stop, int64_t deadline_us)
{
    const float step = r->w / NX_IMG_W;
    const uint16_t max_iter = max_iter_for(r->w);

    while (xSemaphoreTake(s_frame_submit, pdMS_TO_TICKS(100)) != pdTRUE) {
        bool cancelled = stop ? atomic_load(stop) || esp_timer_get_time() >= deadline_us : atomic_load(&s_frames_paused);
        if (cancelled) return 0;
    }
    if (!stop && atomic_load(&s_frames_paused)) {
        xSemaphoreGive(s_frame_submit);
        return 0;
    }
    xSemaphoreTake(s_fmtx, portMAX_DELAY);
    if (++s_fv.frame == 0) s_fv.frame = 1;
    s_frame_base = s_fv.version;
    s_fv.done = 0;
    s_fv.total = NX_TILES;
    s_fv.max_iter = max_iter;
    s_fv.zoom = HOME_W / r->w;
    memset(s_fv.owner, 0, sizeof(s_fv.owner));
    s_fv.crc = 0;
    s_cx = r->cx; s_cy = r->cy; s_w = r->w;
    s_t_start = esp_timer_get_time();
    s_t_end = 0;
    uint16_t frame = s_fv.frame;
    xSemaphoreGive(s_fmtx);

    int cancelled = jq_cancel_old_frames(s_gq, frame);
    nx_log("CLI", NX_LOG_INFO, "frame %u: %d tile jobs (zoom %.1fx, max_iter %u)%s", frame, NX_TILES,
           (double)(HOME_W / r->w), max_iter, cancelled ? ", old tiles cancelled" : "");

    for (int ty = 0; ty < NX_TILES_Y; ty++) {
        for (int tx = 0; tx < NX_TILES_X; tx++) {
            /* priority by distance from the centre: the middle of the image first */
            float dx = (tx + 0.5f) / NX_TILES_X - 0.5f, dy = ((ty + 0.5f) / NX_TILES_Y - 0.5f) * NX_IMG_H / NX_IMG_W;
            float d = sqrtf(dx * dx + dy * dy) / 0.6f;
            nx_job_spec_t j = { .type = NX_JOB_FRACTAL, .budget_ms = 3000 };
            j.prio = d < 0.22f ? 3 : d < 0.45f ? 2 : d < 0.7f ? 1 : 0;
            j.p.fractal.x0 = r->cx + (tx * NX_TILE - NX_IMG_W / 2) * step;
            j.p.fractal.y0 = r->cy + (ty * NX_TILE - NX_IMG_H / 2) * step;
            j.p.fractal.step = step;
            j.p.fractal.max_iter = max_iter;
            j.p.fractal.tx = (uint16_t)tx;
            j.p.fractal.ty = (uint16_t)ty;
            j.p.fractal.frame = frame;
            for (;;) {
                bool cancelled = stop ? atomic_load(stop) || esp_timer_get_time() >= deadline_us
                                      : atomic_load(&s_frames_paused);
                if (cancelled) {
                    xSemaphoreGive(s_frame_submit);
                    return 0;
                }
                if (jq_submit(s_gq, &j, pdMS_TO_TICKS(100))) break;
            }
        }
    }
    xSemaphoreGive(s_frame_submit);
    return frame;
}

static void frame_task(void *arg)
{
    int target = 0, depth = 0;
    frame_req_t r = { HOME_X, HOME_Y, HOME_W };
    submit_frame(&r, NULL, 0);
    for (;;) {
        if (xQueueReceive(s_frame_req, &r, pdMS_TO_TICKS(250)) == pdTRUE) {
            depth = 0;
            submit_frame(&r, NULL, 0);
            continue;
        }
        /* auto-zoom: 2.5 s after a frame completes, move closer to a nice spot */
        xSemaphoreTake(s_fmtx, portMAX_DELAY);
        bool finished = s_t_end && esp_timer_get_time() - s_t_end > 2500000;
        frame_req_t cur = { s_cx, s_cy, s_w };
        xSemaphoreGive(s_fmtx);
        if (!atomic_load(&s_auto) || !finished) continue;
        if (depth >= AUTO_DEPTH) {
            depth = 0;
            target = (target + 1) % (int)(sizeof(TARGETS) / sizeof(TARGETS[0]));
            r = (frame_req_t){ HOME_X, HOME_Y, HOME_W };
        } else {
            depth++;
            r.cx = cur.cx + (TARGETS[target][0] - cur.cx) * 0.6f;
            r.cy = cur.cy + (TARGETS[target][1] - cur.cy) * 0.6f;
            r.w = cur.w * ZOOM_STEP;
        }
        submit_frame(&r, NULL, 0);
    }
}

void frame_on_tile(const nx_job_spec_t *spec, const uint8_t *pixels, uint8_t node)
{
    bool completed = false;
    uint16_t frame = 0;
    uint32_t ms = 0;
    if (spec->p.fractal.tx >= NX_TILES_X || spec->p.fractal.ty >= NX_TILES_Y) return;
    xSemaphoreTake(s_fmtx, portMAX_DELAY);
    int t = spec->p.fractal.ty * NX_TILES_X + spec->p.fractal.tx;
    if (spec->p.fractal.frame == s_fv.frame && t < NX_TILES && s_fv.owner[t] == 0) {
        for (int y = 0; y < NX_TILE; y++)
            memcpy(s_pixels + (spec->p.fractal.ty * NX_TILE + y) * NX_IMG_W + spec->p.fractal.tx * NX_TILE,
                   pixels + y * NX_TILE, NX_TILE);
        s_fv.owner[t] = node + 1;
        s_tile_ver[t] = ++s_fv.version;
        if (++s_fv.done == s_fv.total) {
            s_t_end = esp_timer_get_time();
            /* the same view must give the same image whoever computed the tiles (test T2/T3) */
            s_fv.crc = esp_rom_crc32_le(0, s_pixels, NX_IMG_W * NX_IMG_H);
            completed = true;
            frame = s_fv.frame;
            ms = (uint32_t)((s_t_end - s_t_start) / 1000);
        }
    }
    xSemaphoreGive(s_fmtx);
    if (completed) nx_log("MST", NX_LOG_OK, "frame %u complete in %lu ms", frame, (unsigned long)ms);
}

void frame_view(nx_frame_view_t *v)
{
    xSemaphoreTake(s_fmtx, portMAX_DELAY);
    *v = s_fv;
    v->elapsed_ms = (uint32_t)(((s_t_end ? s_t_end : esp_timer_get_time()) - s_t_start) / 1000);
    xSemaphoreGive(s_fmtx);
}

int frame_tiles_since(uint32_t since, uint8_t *buf, int cap)
{
    xSemaphoreTake(s_fmtx, portMAX_DELAY);
    int len = 8;
    uint16_t count = 0;
    uint32_t base = since > s_frame_base ? since : s_frame_base;
    if (base > s_fv.version) base = s_frame_base;
    uint32_t room = (uint32_t)((cap - 8) / (4 + NX_TILE_BYTES));
    uint32_t advance = s_fv.version - base;
    uint32_t watermark = base + (advance > room ? room : advance);
    for (int t = 0; t < NX_TILES; t++) {
        if (!s_fv.owner[t] || s_tile_ver[t] <= base || s_tile_ver[t] > watermark) continue;
        buf[len] = (uint8_t)(t & 0xFF);
        buf[len + 1] = (uint8_t)(t >> 8);
        buf[len + 2] = s_fv.owner[t];
        buf[len + 3] = 0;
        int tx = t % NX_TILES_X, ty = t / NX_TILES_X;
        for (int y = 0; y < NX_TILE; y++)
            memcpy(buf + len + 4 + y * NX_TILE, s_pixels + (ty * NX_TILE + y) * NX_IMG_W + tx * NX_TILE, NX_TILE);
        len += 4 + NX_TILE_BYTES;
        count++;
    }
    uint16_t frame = s_fv.frame;
    uint32_t ver = watermark;
    xSemaphoreGive(s_fmtx);
    memcpy(buf, &frame, 2);
    memcpy(buf + 2, &ver, 4);
    memcpy(buf + 6, &count, 2);
    return len;
}

/* ------------------------------ controls ------------------------------ */

void producers_set_clients(uint8_t n) { atomic_store(&s_clients, n > MAX_CLIENTS ? MAX_CLIENTS : n); }
uint8_t producers_clients(void) { return atomic_load(&s_clients); }
uint32_t producers_blocked(void) { return atomic_load(&s_blocked); }
void producers_set_auto(bool on) { atomic_store(&s_auto, on); }

uint16_t producers_frame_now(float cx, float cy, float w, const atomic_bool *stop, int64_t deadline_us)
{
    frame_req_t r = { cx, cy, w };
    return submit_frame(&r, stop, deadline_us);
}
bool producers_auto(void) { return atomic_load(&s_auto); }

void producers_pause_frames(bool paused)
{
    atomic_store(&s_frames_paused, paused);
    if (paused) xQueueReset(s_frame_req);
}

void producers_storm(uint32_t ms)
{
    if (producers_clients() == 0) producers_set_clients(1);
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    taskENTER_CRITICAL(&s_control_mux);
    s_storm_until = until;
    taskEXIT_CRITICAL(&s_control_mux);
    nx_log("CLI", NX_LOG_WARN, "high-priority storm: %u client task(s) submit P3 jobs for %lu ms", s_clients, (unsigned long)ms);
}

bool producers_runaway(void)
{
    nx_job_spec_t j = { .type = NX_JOB_SPIN, .prio = 2, .budget_ms = 1500 };
    j.p.spin.duration_ms = 60000;       /* would run for a minute; the budget stops it */
    if (!jq_submit(s_gq, &j, 0)) return false;
    nx_log("CLI", NX_LOG_WARN, "runaway job submitted (budget 1500 ms)");
    return true;
}

bool producers_zoom_at(float fx, float fy)
{
    if (!isfinite(fx) || !isfinite(fy) || fx < 0 || fx > 1 || fy < 0 || fy > 1) return false;
    xSemaphoreTake(s_fmtx, portMAX_DELAY);
    frame_req_t r = { s_cx + (fx - 0.5f) * s_w, s_cy + (fy - 0.5f) * s_w * NX_IMG_H / NX_IMG_W, fmaxf(s_w * 0.4f, 0.0001f) };
    xSemaphoreGive(s_fmtx);
    return xQueueSend(s_frame_req, &r, 0) == pdTRUE;
}

void producers_start(nx_job_queue_t *gq)
{
    s_gq = gq;
    s_frame_submit = xSemaphoreCreateMutex();
    s_fmtx = xSemaphoreCreateMutex();
    s_pixels = heap_caps_calloc(1, NX_IMG_W * NX_IMG_H, MALLOC_CAP_SPIRAM);
    s_frame_req = xQueueCreate(2, sizeof(frame_req_t));
    if (!s_frame_submit || !s_fmtx || !s_pixels || !s_frame_req) abort();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        char name[12] = "client_0";
        name[7] = (char)('0' + i);
        if (xTaskCreatePinnedToCore(client_task, name, NX_STACK_SMALL, (void *)(uintptr_t)i, NX_PRIO_CLIENT, NULL, tskNO_AFFINITY) != pdPASS) abort();
    }
    if (xTaskCreatePinnedToCore(frame_task, "frames", NX_STACK_SMALL, NULL, NX_PRIO_CLIENT, NULL, tskNO_AFFINITY) != pdPASS) abort();
}
