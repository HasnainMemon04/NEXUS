/* worker_link.h — the worker's side of the message link to the master. */
#pragma once
#include "job_queue.h"
#include "exec_pool.h"

void worker_link_init(nx_job_queue_t *q, nx_pool_t *pool);
void worker_link_start(void);
/* exec_pool result callback: queue a RESULT message for the master. */
void worker_link_on_result(const nx_job_t *job, const nx_result_t *res, const uint8_t *data, void *arg);
