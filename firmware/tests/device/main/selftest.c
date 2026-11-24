/* Runs the production queue, executor pool, kernels and labs under FreeRTOS. */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "exec_pool.h"
#include "monitor.h"
#include "oslab.h"

static nx_job_queue_t s_queue;
static nx_pool_t s_pool;
static SemaphoreHandle_t s_done;
static uint8_t s_strip[NX_IMG_W * NX_TILE];
static atomic_uint s_tile_mask, s_errors, s_mm_crc;

static void require(bool condition, const char *description)
{
    if (condition) return;
    printf("NEXUS_SELFTEST_FAIL %s\n", description);
    fflush(stdout);
    esp_restart();
}

static void result(const nx_job_t *job, const nx_result_t *res, const uint8_t *data, void *arg)
{
    const nx_job_spec_t *spec = &job->spec;
    nx_result_status_t expected = NX_RES_OK;
    if (spec->type == NX_JOB_SPIN && spec->p.spin.duration_ms > spec->budget_ms) expected = NX_RES_TIMEOUT;
    if (res->status != expected) {
        atomic_fetch_add(&s_errors, 1);
    } else if (spec->type == NX_JOB_FRACTAL) {
        uint16_t tx = spec->p.fractal.tx;
        if (tx >= NX_TILES_X || res->data_len != NX_TILE_BYTES ||
            res->checksum != esp_rom_crc32_le(0, data, NX_TILE_BYTES)) {
            atomic_fetch_add(&s_errors, 1);
        } else if (atomic_fetch_or(&s_tile_mask, (1u << tx)) & (1u << tx)) {
            atomic_fetch_add(&s_errors, 1);
        } else {
            for (unsigned row = 0; row < NX_TILE; row++)
                memcpy(s_strip + row * NX_IMG_W + tx * NX_TILE, data + row * NX_TILE, NX_TILE);
        }
    } else if (spec->type == NX_JOB_MATMUL) {
        atomic_store(&s_mm_crc, res->checksum);
    }
    if (xSemaphoreGive(s_done) != pdTRUE) atomic_fetch_add(&s_errors, 1);
}

static void submit(const nx_job_spec_t *spec)
{
    require(jq_submit(&s_queue, spec, pdMS_TO_TICKS(10000)), "producer could not submit");
}

static void wait_results(unsigned count)
{
    for (unsigned i = 0; i < count; i++)
        require(xSemaphoreTake(s_done, pdMS_TO_TICKS(15000)) == pdTRUE, "missing result");
    require(atomic_load(&s_errors) == 0, "invalid result, status, or duplicate tile");
}

static uint32_t fractal(float cx, float cy, float width, uint16_t iterations)
{
    uint32_t crc = 0;
    const float step = width / NX_IMG_W;
    const float left = cx - width / 2.0f;
    const float top = cy - step * NX_IMG_H / 2.0f;
    for (uint16_t ty = 0; ty < NX_TILES_Y; ty++) {
        atomic_store(&s_tile_mask, 0);
        for (uint16_t tx = 0; tx < NX_TILES_X; tx++) {
            nx_job_spec_t spec = { .type = NX_JOB_FRACTAL, .prio = tx % 4, .budget_ms = 5000 };
            spec.p.fractal = (nx_fractal_params_t){
                .x0 = left + tx * NX_TILE * step, .y0 = top + ty * NX_TILE * step,
                .step = step, .max_iter = iterations, .tx = tx, .ty = ty, .frame = 1,
            };
            submit(&spec);
        }
        wait_results(NX_TILES_X);
        require(atomic_load(&s_tile_mask) == ((1u << NX_TILES_X) - 1), "incomplete image strip");
        crc = esp_rom_crc32_le(crc, s_strip, sizeof(s_strip));
    }
    return crc;
}

static void verify_slots(void)
{
    for (uint8_t k = 1; k <= NX_POOL_SIZE; k++) {
        pool_set_slots(&s_pool, k);
        taskENTER_CRITICAL(&s_pool.lock);
        s_pool.peak_running = 0;
        taskEXIT_CRITICAL(&s_pool.lock);
        nx_job_spec_t spec = { .type = NX_JOB_SPIN, .budget_ms = 1000 };
        spec.p.spin.duration_ms = 40;
        for (unsigned i = 0; i < 8; i++) submit(&spec);
        wait_results(8);
        taskENTER_CRITICAL(&s_pool.lock);
        uint8_t peak = s_pool.peak_running;
        uint8_t permits = s_pool.permits;
        taskEXIT_CRITICAL(&s_pool.lock);
        require(peak > 0 && peak <= k && permits == k, "execution-slot cap");
        printf("NEXUS_SLOTS K=%u peak=%u permits=%u\n", k, peak, permits);
    }
    pool_set_slots(&s_pool, 2);
    nx_job_spec_t timeout = { .type = NX_JOB_SPIN, .budget_ms = 20 };
    timeout.p.spin.duration_ms = 100;
    submit(&timeout);
    wait_results(1);
    require(s_pool.timeouts == 1, "execution-budget timeout");
}

static void verify_labs(void)
{
    nx_lab_results_t lab = { 0 };
    for (uint8_t index = 0; index < 4; index++) {
        printf("NEXUS_LAB_START lab=%c\n", 'A' + index);
        require(oslab_run(index), "lab did not start");
        int64_t deadline = esp_timer_get_time() + 60000000LL;
        while (oslab_busy() && esp_timer_get_time() < deadline) vTaskDelay(1);
        oslab_get(&lab);
        printf("NEXUS_LAB_DONE lab=%c mask=%u busy=%d\n", 'A' + index, lab.done_mask, oslab_busy());
        require(!oslab_busy() && (lab.done_mask & (1u << index)), "lab did not finish");
    }
    printf("NEXUS_LABS unlocked=%" PRIu32 " locked=%" PRIu32 " permits=%" PRIu32
           " jobs=%" PRIu32 " priority_binary_us=%" PRIu32 " priority_mutex_us=%" PRIu32 "\n",
           lab.a_counter, lab.b_counter, lab.c_peak, lab.c_jobs, lab.d_bin_us, lab.d_mutex_us);
    require(lab.a_counter > 0 && lab.a_counter < 200000, "lab A lost updates");
    require(lab.b_counter == 200000, "lab B mutex protection");
    require(lab.c_peak == 2 && lab.c_jobs == 60, "lab C semaphore capacity");
    require(lab.d_mutex_us > 0 && lab.d_bin_us > lab.d_mutex_us, "lab D priority inheritance");
}

void app_main(void)
{
    printf("NEXUS_SELFTEST_START target=%s cores=%d\n", CONFIG_IDF_TARGET, configNUMBER_OF_CORES);
    require(configNUMBER_OF_CORES == 2, "dual-core test configuration");
    require(jq_init(&s_queue, NX_LOCAL_QUEUE_CAP, 0), "queue allocation");
    s_done = xSemaphoreCreateCounting(NX_TILES_X, 0);
    require(s_done != NULL, "completion semaphore allocation");
    pool_start(&s_pool, &s_queue, configNUMBER_OF_CORES, result, NULL);
    oslab_init();

    int64_t t0 = esp_timer_get_time();
    uint32_t overview = fractal(-0.5f, 0.0f, 3.1f, 120);
    int64_t overview_us = esp_timer_get_time() - t0;
    t0 = esp_timer_get_time();
    uint32_t detail = fractal(-0.7436439f, 0.1318259f, 0.07f, 256);
    int64_t detail_us = esp_timer_get_time() - t0;
    uint32_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    t0 = esp_timer_get_time();
    require(fractal(-0.5f, 0.0f, 3.1f, 120) == overview, "repeat image mismatch");
    int64_t repeat_us = esp_timer_get_time() - t0;
    uint32_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    require(after == before, "internal heap changed across identical workload");

    nx_job_spec_t matrix = { .type = NX_JOB_MATMUL, .budget_ms = 1000 };
    matrix.p.matmul.n = 32;
    matrix.p.matmul.seed = 0x4baf1234;
    submit(&matrix);
    wait_results(1);
    uint32_t matmul = atomic_load(&s_mm_crc);
    printf("NEXUS_CHECKSUMS overview=%08" PRIx32 " detail=%08" PRIx32 " matmul=%08" PRIx32 "\n",
           overview, detail, matmul);
    printf("NEXUS_KERNEL_TIME tiles=%u overview_us=%" PRId64 " detail_us=%" PRId64 " repeat_us=%" PRId64 "\n",
           NX_TILES, overview_us, detail_us, repeat_us);
    verify_slots();
    verify_labs();

    static nx_monitor_t monitor;
    monitor_sample(&monitor);
    unsigned executors = 0;
    for (uint8_t i = 0; i < monitor.count; i++) {
        if (strncmp(monitor.task[i].name, "exec_", 5)) continue;
        executors++;
        printf("NEXUS_STACK task=%s minimum_free=%u\n", monitor.task[i].name, monitor.task[i].stack_free);
        require(monitor.task[i].stack_free > 512, "executor stack margin");
    }
    require(executors == NX_POOL_SIZE, "executor task monitor count");
    printf("NEXUS_SELFTEST_PASS target=%s heap_before=%" PRIu32 " heap_after=%" PRIu32 " heap_min=%zu\n",
           CONFIG_IDF_TARGET, before, after, heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    fflush(stdout);
    esp_restart();
}
