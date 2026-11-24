/*
 * logger.h — the logging and monitoring thread (A4).
 *
 * OS CONCEPT: producer–consumer with a non-blocking producer. Any task calls
 * nx_log(); the record goes into a FreeRTOS queue and one logger task prints it
 * and keeps the recent lines for the dashboard. If the queue is full the record
 * is dropped and counted, so logging can never block the engine.
 */
#pragma once
#include <stdint.h>

enum { NX_LOG_INFO = 0, NX_LOG_OK = 1, NX_LOG_WARN = 2, NX_LOG_ERR = 3 };

typedef struct {
    uint32_t seq;
    int64_t  t_us;
    char     tag[5];
    uint8_t  level;
    char     msg[100];
} nx_log_rec_t;

void     logger_start(void);
void     nx_log(const char *tag, uint8_t level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
uint32_t logger_dropped(void);
uint32_t logger_queue_depth(void);
/* Copy records newer than *after_seq (oldest first); updates *after_seq. */
int      logger_recent(nx_log_rec_t *out, int max, uint32_t *after_seq);
