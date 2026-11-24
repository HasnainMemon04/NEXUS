/*
 * logger.c — logging thread (A4): producers never block, one consumer prints.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nexus_config.h"
#include "logger.h"

#define LOG_QUEUE_LEN  64
#define LOG_RING       64

static QueueHandle_t s_q;               /* log_queue: tasks -> logger task        */
static SemaphoreHandle_t s_ring_mtx;    /* protects the ring read by the dashboard */
static nx_log_rec_t s_ring[LOG_RING];
static uint32_t s_seq;
static atomic_uint s_dropped;

void nx_log(const char *tag, uint8_t level, const char *fmt, ...)
{
    if (!s_q) return;
    nx_log_rec_t r = { .t_us = esp_timer_get_time(), .level = level };
    strncpy(r.tag, tag, sizeof(r.tag) - 1);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.msg, sizeof(r.msg), fmt, ap);
    va_end(ap);
    /* OS CONCEPT: non-blocking send — a full queue drops the record instead of
     * blocking the caller (which may be the dispatcher or an executor). */
    if (xQueueSend(s_q, &r, 0) != pdTRUE) atomic_fetch_add(&s_dropped, 1);
}

static void logger_task(void *arg)
{
    nx_log_rec_t r;
    for (;;) {
        xQueueReceive(s_q, &r, portMAX_DELAY);
        xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
        r.seq = ++s_seq;
        s_ring[r.seq % LOG_RING] = r;
        xSemaphoreGive(s_ring_mtx);

        if (r.level == NX_LOG_ERR)       ESP_LOGE(r.tag, "%s", r.msg);
        else if (r.level == NX_LOG_WARN) ESP_LOGW(r.tag, "%s", r.msg);
        else                             ESP_LOGI(r.tag, "%s", r.msg);
    }
}

void logger_start(void)
{
    s_ring_mtx = xSemaphoreCreateMutex();
    s_q = xQueueCreate(LOG_QUEUE_LEN, sizeof(nx_log_rec_t));
    if (!s_ring_mtx || !s_q || xTaskCreate(logger_task, "logger", NX_STACK_SMALL, NULL, NX_PRIO_LOGGER, NULL) != pdPASS) abort();
}

uint32_t logger_dropped(void) { return atomic_load(&s_dropped); }
uint32_t logger_queue_depth(void) { return s_q ? uxQueueMessagesWaiting(s_q) : 0; }

int logger_recent(nx_log_rec_t *out, int max, uint32_t *after_seq)
{
    int n = 0;
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    uint32_t first = *after_seq + 1;
    if (s_seq >= LOG_RING && first <= s_seq - LOG_RING) first = s_seq - LOG_RING + 1;
    if (s_seq >= (uint32_t)max && first + max <= s_seq) first = s_seq - max + 1;
    for (uint32_t s = first; s <= s_seq && n < max; s++) out[n++] = s_ring[s % LOG_RING];
    *after_seq = s_seq;
    xSemaphoreGive(s_ring_mtx);
    return n;
}
