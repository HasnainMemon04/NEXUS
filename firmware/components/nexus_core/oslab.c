/*
 * oslab.c — synchronization laboratory (OSC §6.1–6.8).
 *
 * The unsafe code in lab A is deliberate and touches only s_counter.
 * Lab tasks are created for the experiment and delete themselves afterwards.
 */
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "logger.h"
#include "oslab.h"

#define LAB_LOOPS        100000
#define LAB_C_TASKS      6
#define LAB_C_JOBS_EACH  10
#define LAB_C_PERMITS    2

static atomic_uint      s_counter;          /* the shared variable of labs A and B */
static SemaphoreHandle_t s_ready, s_start, s_race_write;   /* barriers and phase signaling */
static SemaphoreHandle_t s_done;             /* lab tasks report completion here    */
static SemaphoreHandle_t s_lock;             /* lab B mutex, lab C/D lock           */
static nx_lab_results_t  s_res;
static bool              s_busy;
static bool              s_pair_ready;
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_c_running, s_c_peak, s_c_jobs;
static uint32_t          s_d_h_wait;

/* ---------- A: race condition on a read-modify-write ---------- */
static bool await_pair(void)
{
    xSemaphoreGive(s_ready);
    xSemaphoreTake(s_start, portMAX_DELAY);
    return s_pair_ready;
}

static void lab_a_task(void *arg)
{
    if (await_pair()) {
        /* Separate atomic read and write avoid a C data race. They still lose an
         * update when both tasks read before either writes, forced once here. */
        uint32_t first = atomic_load_explicit(&s_counter, memory_order_relaxed);
        xSemaphoreGive(s_ready);
        xSemaphoreTake(s_race_write, portMAX_DELAY);
        atomic_store_explicit(&s_counter, first + 1, memory_order_relaxed);
        for (int i = 1; i < LAB_LOOPS; i++) {
            uint32_t v = atomic_load_explicit(&s_counter, memory_order_relaxed);
            atomic_store_explicit(&s_counter, v + 1, memory_order_relaxed);
        }
    }
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

static void lab_b_task(void *arg)
{
    if (await_pair()) {
        for (int i = 0; i < LAB_LOOPS; i++) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            uint32_t v = atomic_load_explicit(&s_counter, memory_order_relaxed);
            atomic_store_explicit(&s_counter, v + 1, memory_order_relaxed);
            xSemaphoreGive(s_lock);
        }
    }
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

static bool run_counter_lab(TaskFunction_t fn, uint32_t *counter, uint32_t *elapsed_us)
{
    atomic_store(&s_counter, 0);
    uint8_t made = 0;
    for (; made < 2; made++) {
        if (xTaskCreatePinnedToCore(fn, made ? "lab_t1" : "lab_t0", 2048, NULL, 4, NULL,
                                    made % configNUMBER_OF_CORES) != pdPASS) break;
    }
    for (int i = 0; i < made; i++) xSemaphoreTake(s_ready, portMAX_DELAY);
    s_pair_ready = made == 2;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < made; i++) xSemaphoreGive(s_start);
    if (s_pair_ready && fn == lab_a_task) {
        for (int i = 0; i < 2; i++) xSemaphoreTake(s_ready, portMAX_DELAY);
        for (int i = 0; i < 2; i++) xSemaphoreGive(s_race_write);
    }
    for (int i = 0; i < made; i++) xSemaphoreTake(s_done, portMAX_DELAY);
    *elapsed_us = (uint32_t)(esp_timer_get_time() - t0);
    *counter = atomic_load(&s_counter);
    return s_pair_ready;
}

/* ---------- C: counting semaphore caps concurrency ---------- */
static void lab_c_task(void *arg)
{
    SemaphoreHandle_t slots = (SemaphoreHandle_t)arg;
    for (int i = 0; i < LAB_C_JOBS_EACH; i++) {
        xSemaphoreTake(slots, portMAX_DELAY);
        taskENTER_CRITICAL(&s_mux);
        s_c_running++;
        if (s_c_running > s_c_peak) s_c_peak = s_c_running;
        taskEXIT_CRITICAL(&s_mux);

        /* Model a job that waits for I/O while retaining its permit so two
         * jobs can overlap even when both tasks share one CPU. */
        vTaskDelay(pdMS_TO_TICKS(20));

        taskENTER_CRITICAL(&s_mux);
        s_c_running--;
        s_c_jobs++;
        taskEXIT_CRITICAL(&s_mux);
        xSemaphoreGive(slots);
        vTaskDelay(1);
    }
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

/* ---------- D: priority inversion ---------- */
#define PRIO_L 6
#define PRIO_M 7
#define PRIO_H 8

static void busy_ms(int ms)
{
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < end) { }
}

static void lab_d_low(void *arg)      /* L: takes the lock and works 20 ms while holding it */
{
    SemaphoreHandle_t lock = (SemaphoreHandle_t)arg;
    xSemaphoreTake(lock, portMAX_DELAY);
    xSemaphoreGive(s_ready);          /* L owns the lock */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    busy_ms(20);
    xSemaphoreGive(lock);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

static void lab_d_high(void *arg)     /* H: needs the same lock; we measure how long it waits */
{
    SemaphoreHandle_t lock = (SemaphoreHandle_t)arg;
    int64_t t0 = esp_timer_get_time();
    xSemaphoreGive(s_ready);
    xSemaphoreTake(lock, portMAX_DELAY);
    s_d_h_wait = (uint32_t)(esp_timer_get_time() - t0);
    xSemaphoreGive(lock);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

static void lab_d_medium(void *arg)   /* M: CPU-bound, does not need the lock */
{
    busy_ms(300);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

static bool run_inversion(SemaphoreHandle_t lock, uint32_t *wait_us)
{
    const BaseType_t core = configNUMBER_OF_CORES - 1;
    TaskHandle_t low;
    if (xTaskCreatePinnedToCore(lab_d_low, "lab_L", 2048, lock, PRIO_L, &low, core) != pdPASS)
        return false;
    xSemaphoreTake(s_ready, portMAX_DELAY);
    int made = 1;
    if (xTaskCreatePinnedToCore(lab_d_high, "lab_H", 2048, lock, PRIO_H, NULL, core) == pdPASS) {
        made++;
        xSemaphoreTake(s_ready, portMAX_DELAY);
        if (xTaskCreatePinnedToCore(lab_d_medium, "lab_M", 2048, NULL, PRIO_M, NULL, core) == pdPASS)
            made++;
    }
    xTaskNotifyGive(low);
    for (int i = 0; i < made; i++) xSemaphoreTake(s_done, portMAX_DELAY);
    *wait_us = s_d_h_wait;
    return made == 3;
}

static void lab_runner(void *arg)
{
    uint8_t lab = (uint8_t)(uintptr_t)arg;
    nx_lab_results_t res;
    oslab_get(&res);
    uint32_t counter, us, binary_us, mutex_us;
    bool ok = false;
    switch (lab) {
    case 0:
        ok = run_counter_lab(lab_a_task, &counter, &us);
        res.a_counter = counter;
        res.a_us = us;
        if (ok) nx_log("LAB", NX_LOG_INFO, "A: %lu of %d increments recorded", (unsigned long)counter, 2 * LAB_LOOPS);
        break;
    case 1:
        s_lock = xSemaphoreCreateMutex();
        if (s_lock) {
            ok = run_counter_lab(lab_b_task, &counter, &us);
            res.b_counter = counter;
            res.b_us = us;
            vSemaphoreDelete(s_lock);
            if (ok) nx_log("LAB", NX_LOG_INFO, "B: %lu of %d increments recorded", (unsigned long)counter, 2 * LAB_LOOPS);
        }
        break;
    case 2: {
        SemaphoreHandle_t slots = xSemaphoreCreateCounting(LAB_C_PERMITS, LAB_C_PERMITS);
        if (!slots) break;
        s_c_running = s_c_peak = s_c_jobs = 0;
        int made = 0;
        for (; made < LAB_C_TASKS; made++)
            if (xTaskCreate(lab_c_task, "lab_c", 2048, slots, 4, NULL) != pdPASS) break;
        for (int i = 0; i < made; i++) xSemaphoreTake(s_done, portMAX_DELAY);
        vSemaphoreDelete(slots);
        res.c_peak = s_c_peak;
        res.c_jobs = s_c_jobs;
        ok = made == LAB_C_TASKS;
        if (ok) nx_log("LAB", NX_LOG_INFO, "C: %lu jobs, peak %lu with %d permits",
                       (unsigned long)res.c_jobs, (unsigned long)res.c_peak, LAB_C_PERMITS);
        break;
    }
    case 3: {
        SemaphoreHandle_t bin = xSemaphoreCreateBinary();
        SemaphoreHandle_t mtx = xSemaphoreCreateMutex();
        if (bin && mtx) {
            xSemaphoreGive(bin);
            ok = run_inversion(bin, &binary_us) && run_inversion(mtx, &mutex_us);
            if (ok) {
                res.d_bin_us = binary_us;
                res.d_mutex_us = mutex_us;
                nx_log("LAB", NX_LOG_INFO, "D: H waited %lu us with binary semaphore, %lu us with mutex",
                       (unsigned long)binary_us, (unsigned long)mutex_us);
            }
        }
        if (bin) vSemaphoreDelete(bin);
        if (mtx) vSemaphoreDelete(mtx);
        break;
    }
    }
    if (!ok) nx_log("LAB", NX_LOG_ERR, "Experiment %c could not allocate its tasks and semaphores", 'A' + lab);
    taskENTER_CRITICAL(&s_mux);
    if (ok) {
        res.done_mask |= (uint8_t)(1u << lab);
        s_res = res;
    }
    s_busy = false;
    taskEXIT_CRITICAL(&s_mux);
    vTaskDelete(NULL);
}

void oslab_init(void)
{
    s_ready = xSemaphoreCreateCounting(2, 0);
    s_start = xSemaphoreCreateCounting(2, 0);
    s_race_write = xSemaphoreCreateCounting(2, 0);
    s_done = xSemaphoreCreateCounting(16, 0);
    if (!s_done || !s_ready || !s_start || !s_race_write) abort();
}

bool oslab_run(uint8_t lab)
{
    if (lab > 3) return false;
    taskENTER_CRITICAL(&s_mux);
    bool accepted = !s_busy;
    if (accepted) s_busy = true;
    taskEXIT_CRITICAL(&s_mux);
    if (!accepted) return false;
    if (xTaskCreatePinnedToCore(lab_runner, "lab_run", 3072, (void *)(uintptr_t)lab, 9, NULL, 0) != pdPASS) {
        taskENTER_CRITICAL(&s_mux);
        s_busy = false;
        taskEXIT_CRITICAL(&s_mux);
        return false;
    }
    return true;
}

void oslab_get(nx_lab_results_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_res;
    taskEXIT_CRITICAL(&s_mux);
}

bool oslab_busy(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool busy = s_busy;
    taskEXIT_CRITICAL(&s_mux);
    return busy;
}
