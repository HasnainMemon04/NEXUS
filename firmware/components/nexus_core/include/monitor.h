/*
 * monitor.h — FreeRTOS task monitor (A4).
 *
 * Takes a SAMPLE of every task (state, priority, core affinity, free stack)
 * with uxTaskGetSystemState(), and computes each task's CPU share from the
 * difference between two samples of its run-time counter. A sample is not a
 * trace: a task can change state many times between two samples.
 */
#pragma once
#include <stdint.h>
#include "nexus_config.h"

typedef struct __attribute__((packed)) {
    char     name[12];
    uint8_t  state;       /* 0 running, 1 ready, 2 blocked, 3 suspended (eTaskState) */
    uint8_t  prio;
    int8_t   core;        /* pinned core 0/1, or -1 = may run on either core          */
    uint8_t  cpu_pct;     /* share of total CPU capacity of both cores, 0..100        */
    uint16_t stack_free;  /* minimum free stack ever seen, bytes                      */
} nx_task_info_t;

typedef struct {
    uint8_t        count;
    uint8_t        core_load[2];   /* % busy per core = 100 - idle task share */
    nx_task_info_t task[NX_MAX_TASKS];
} nx_monitor_t;

/* Call from one task only, about twice a second. */
void monitor_sample(nx_monitor_t *out);
