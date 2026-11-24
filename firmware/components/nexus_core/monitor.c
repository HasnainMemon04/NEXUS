/*
 * monitor.c — sampled FreeRTOS task table and CPU load.
 * Needs CONFIG_FREERTOS_USE_TRACE_FACILITY and CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
 * (set in sdkconfig.defaults). The run-time counter is the 1 MHz esp_timer.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "monitor.h"

#define MAX_SYS_TASKS 48

static TaskStatus_t s_now[MAX_SYS_TASKS];
static struct { UBaseType_t num; configRUN_TIME_COUNTER_TYPE rt; } s_prev[MAX_SYS_TASKS];
static int s_prev_n;
static configRUN_TIME_COUNTER_TYPE s_prev_total;

static configRUN_TIME_COUNTER_TYPE prev_runtime(UBaseType_t num)
{
    for (int i = 0; i < s_prev_n; i++) if (s_prev[i].num == num) return s_prev[i].rt;
    return 0;
}

void monitor_sample(nx_monitor_t *out)
{
    configRUN_TIME_COUNTER_TYPE total = 0;
    UBaseType_t n = uxTaskGetSystemState(s_now, MAX_SYS_TASKS, &total);
    configRUN_TIME_COUNTER_TYPE dt = total - s_prev_total;      /* wall time of the window */
    if (dt == 0) dt = 1;

    TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCore(0);
    TaskHandle_t idle1 = NULL;
#if configNUMBER_OF_CORES > 1
    idle1 = xTaskGetIdleTaskHandleForCore(1);
#endif

    memset(out, 0, sizeof(*out));
    if (!n) return;     /* snapshot did not fit; retain the previous baseline */
    for (UBaseType_t i = 0; i < n; i++) {
        const TaskStatus_t *t = &s_now[i];
        configRUN_TIME_COUNTER_TYPE d = t->ulRunTimeCounter - prev_runtime(t->xTaskNumber);
        /* share of both cores' capacity, like ESP-IDF's real_time_stats example */
        uint32_t pct = s_prev_n ? (uint32_t)((d * 100) / (dt * configNUMBER_OF_CORES)) : 0;
        if (pct > 100) pct = 100;
        if (t->xHandle == idle0) out->core_load[0] = (uint8_t)(100 - ((d * 100) / dt > 100 ? 100 : (d * 100) / dt));
        if (t->xHandle == idle1) out->core_load[1] = (uint8_t)(100 - ((d * 100) / dt > 100 ? 100 : (d * 100) / dt));

        if (out->count < NX_MAX_TASKS) {
            nx_task_info_t *o = &out->task[out->count++];
            strncpy(o->name, t->pcTaskName, sizeof(o->name) - 1);
            o->state = (uint8_t)t->eCurrentState;
            o->prio = (uint8_t)t->uxCurrentPriority;
    #ifdef CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
            BaseType_t core = t->xCoreID;
#else
            BaseType_t core = tskNO_AFFINITY;
#endif
            o->core = (core == 0 || core == 1) ? (int8_t)core : -1;
            o->cpu_pct = (uint8_t)pct;
            o->stack_free = (uint16_t)t->usStackHighWaterMark;
        }
    }
    for (UBaseType_t i = 0; i < n && i < MAX_SYS_TASKS; i++) {
        s_prev[i].num = s_now[i].xTaskNumber;
        s_prev[i].rt = s_now[i].ulRunTimeCounter;
    }
    s_prev_n = (int)n;
    s_prev_total = total;
}
