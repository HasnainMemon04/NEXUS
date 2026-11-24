/*
 * oslab.h — the synchronization laboratory (objective O8).
 *
 * Each lab is a small, isolated experiment that runs on its own variables and
 * never touches the job queue. Results are real measurements on this board.
 *   A  lost update: two tasks (one per core) increment a shared counter, no lock
 *   B  the same increment inside a mutex-protected critical section
 *   C  6 tasks share a counting semaphore with 2 permits: peak never above 2
 *   D  priority inversion: binary semaphore vs mutex (priority inheritance)
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct __attribute__((packed)) {
    uint32_t a_counter, a_us;       /* expected 200000; time taken        */
    uint32_t b_counter, b_us;       /* always 200000 with the mutex        */
    uint32_t c_peak, c_jobs;        /* peak concurrent holders; jobs done  */
    uint32_t d_bin_us, d_mutex_us;  /* H's blocking time in each variant   */
    uint8_t  done_mask;             /* bit per lab that has results        */
} nx_lab_results_t;

void oslab_init(void);
bool oslab_run(uint8_t lab);           /* 0 = A ... 3 = D; false if a lab is already running */
void oslab_get(nx_lab_results_t *out);
bool oslab_busy(void);
