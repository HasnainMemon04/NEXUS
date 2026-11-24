/* Deterministic semaphore accounting against the production queue.
 * ESP-IDF/QEMU exercises actual task timing. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "job_queue.h"
#include "../../master/main/placement.h"

struct host_sem { unsigned max, count; };
static int64_t now_us;

SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t initial)
{
    SemaphoreHandle_t sem = malloc(sizeof(*sem));
    if (sem) *sem = (struct host_sem){ max, initial };
    return sem;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return xSemaphoreCreateCounting(1, 1); }

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t wait)
{
    (void)wait;
    if (sem->count == 0) return pdFALSE;
    sem->count--;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    assert(sem->count < sem->max);
    sem->count++;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t sem) { free(sem); }

void *heap_caps_calloc(size_t count, size_t size, unsigned capabilities)
{
    (void)capabilities;
    return calloc(count, size);
}

int64_t esp_timer_get_time(void) { return now_us; }
uint32_t esp_random(void) { return 0x12345678; }

static void check_counts(nx_job_queue_t *q)
{
    assert(q->sem_ready->count == q->ready_count);
    assert(q->sem_free->count + q->ready_count == q->ready_cap);
    assert(q->inflight_count <= q->inflight_cap);
    assert(q->mtx->count == 1);
}

static void cleanup(nx_job_queue_t *q)
{
    check_counts(q);
    vSemaphoreDelete(q->sem_free);
    vSemaphoreDelete(q->sem_ready);
    vSemaphoreDelete(q->mtx);
    free(q->ready);
    free(q->inflight);
}

static nx_job_spec_t spin(uint32_t id, uint8_t priority)
{
    return (nx_job_spec_t){ .id = id, .type = NX_JOB_SPIN, .prio = priority, .budget_ms = 25 };
}

static nx_job_t take(nx_job_queue_t *q, uint8_t node)
{
    nx_job_t result;
    assert(jq_wait_ready(q, 0));
    assert(jq_pop_best(q, &result, node, 1));
    check_counts(q);
    return result;
}

static void test_order(nx_ordering_t ordering, uint32_t expected)
{
    nx_job_queue_t q;
    assert(jq_init(&q, 2, 0));
    assert(jq_get_ordering(&q) == NX_ORDER_AGING);
    jq_set_ordering(&q, ordering);
    assert(jq_get_ordering(&q) == ordering);
    now_us = 1000;
    nx_job_spec_t first = spin(1, 0);
    nx_job_spec_t second = spin(2, NX_PRIO_MAX);
    assert(jq_submit(&q, &first, 0));
    now_us += 3 * NX_AGING_STEP_MS * 1000;
    assert(jq_submit(&q, &second, 0));
    assert(!jq_submit(&q, &second, 0));                 /* bounded */
    assert(take(&q, 0).spec.id == expected);
    assert(take(&q, 0).spec.id == 3 - expected);
    assert(!jq_wait_ready(&q, 0));                      /* empty */
    cleanup(&q);
}

static void test_completion(void)
{
    nx_job_queue_t q;
    assert(jq_init(&q, 3, 2));
    now_us = 1000;
    nx_job_spec_t spec = { .id = 7, .type = NX_JOB_FRACTAL, .budget_ms = 25 };
    assert(jq_submit(&q, &spec, 0));
    nx_job_t dispatched = take(&q, 1);
    jq_mark_running(&q, dispatched.spec.id, 1, 2);
    assert(q.inflight[0].state == NX_JS_DISPATCHED);
    jq_mark_running(&q, dispatched.spec.id, 1, 1);
    assert(q.inflight[0].state == NX_JS_RUNNING);
    nx_result_t result = { .id = 7, .attempt = 1, .status = NX_RES_OK };
    nx_job_t done;
    assert(jq_commit(&q, &result, 1, &done) == NX_COMMIT_INVALID); /* short tile */
    result.data_len = NX_TILE_BYTES;
    assert(jq_commit(&q, &result, 2, &done) == NX_COMMIT_INVALID); /* wrong worker */
    assert(jq_requeue_node(&q, 1, 9) == 1);
    dispatched = take(&q, 2);
    assert(dispatched.spec.attempt == 2);
    assert(dispatched.recovery_from == 1 && dispatched.recovery_serial == 9);
    assert(dispatched.recovery_min_attempt == 2);
    assert(jq_commit(&q, &result, 2, &done) == NX_COMMIT_INVALID); /* wrong attempt owner */
    result.attempt = 2;
    assert(jq_commit(&q, &result, 2, &done) == NX_COMMIT_OK);
    assert(done.node == 2 && done.recovery_from == 1 && done.recovery_serial == 9);
    result.attempt = 1;
    assert(jq_commit(&q, &result, 1, &done) == NX_COMMIT_DUPLICATE);
    cleanup(&q);
}

static void test_late_result_cancels_retry(void)
{
    nx_job_queue_t q;
    assert(jq_init(&q, 1, 1));
    now_us = 1000;
    nx_job_spec_t first = spin(11, 0);
    assert(jq_submit(&q, &first, 0));
    nx_job_t dispatched = take(&q, 1);
    assert(jq_requeue_node(&q, 1, 5) == 1);
    nx_job_t done;
    nx_result_t first_result = { .id = first.id, .attempt = 1 };
    assert(jq_commit(&q, &first_result, 1, &done) == NX_COMMIT_OK);
    assert(done.spec.attempt == 1 && done.t_dispatch == dispatched.t_dispatch);
    assert(jq_commit(&q, &first_result, 1, &done) == NX_COMMIT_DUPLICATE);
    assert(jq_has_pending(&q));                         /* cancelled record still occupies space */
    assert(jq_wait_ready(&q, 0));
    assert(!jq_pop_best(&q, &done, 2, 1));               /* remove cancelled retry */
    assert(!jq_has_pending(&q));
    cleanup(&q);
}

static void test_saturated_requeue_and_leases(void)
{
    nx_job_queue_t q;
    assert(jq_init(&q, 1, 1));
    now_us = 1000;
    nx_job_spec_t first = spin(21, 0);
    nx_job_spec_t second = spin(22, 0);
    assert(jq_submit(&q, &first, 0));
    (void)take(&q, 1);
    assert(jq_submit(&q, &second, 0));
    nx_job_t result;
    assert(jq_wait_ready(&q, 0));
    assert(!jq_pop_best(&q, &result, 2, 1));              /* restore ready token */
    check_counts(&q);
    assert(jq_requeue_node(&q, 1, 5) == 1);
    assert(q.inflight[0].requeue_pending);
    assert(jq_retry_pending(&q) == 0);                  /* ready still full */
    assert(take(&q, 2).spec.id == second.id);             /* exchange pending and ready jobs */
    nx_result_t second_result = { .id = second.id, .attempt = 1 };
    assert(jq_commit(&q, &second_result, 2, &result) == NX_COMMIT_OK);
    result = take(&q, 2);
    assert(result.spec.id == first.id && result.spec.attempt == 2);
    now_us = result.lease_until + 1;
    assert(jq_requeue_expired(&q, now_us) == 1);
    result = take(&q, 1);
    assert(result.spec.attempt == 3);
    assert(jq_requeue_one(&q, first.id, 3, 1));
    assert(q.failed == 1 && q.inflight_count == 0 && q.ready_count == 0);
    cleanup(&q);
}

static void test_placement(void)
{
    nx_placement_node_t nodes[NX_NODES] = {
        { .eligible = true, .k = 2, .requested_k = 2, .cores = 2, .cpu_mhz = 240 },
        { .eligible = true, .k = 4, .requested_k = 4, .cores = 2, .cpu_mhz = 160 },
        { .eligible = true, .k = 2, .requested_k = 2, .cores = 2, .cpu_mhz = 240 },
    };
    assert(nx_select_node(nodes, true, NX_PLACE_LEAST_LOADED, 0) == 0);
    nodes[0].inflight = 3;                              /* no remote fallback */
    assert(nx_select_node(nodes, true, NX_PLACE_ROUND_ROBIN, 0) == -1);
    assert(nx_select_node(nodes, false, NX_PLACE_ROUND_ROBIN, 0) == 1);
    assert(nx_select_node(nodes, false, NX_PLACE_ROUND_ROBIN, 1) == 2);
    assert(nx_select_node(nodes, false, NX_PLACE_LEAST_LOADED, 0) == 2);
    nodes[2].inflight = 1;
    assert(nx_select_node(nodes, false, NX_PLACE_LEAST_LOADED, 0) == 1);
    nodes[1].requested_k = 1;
    nodes[1].inflight = 2;                              /* requested downscale cap */
    assert(nx_select_node(nodes, false, NX_PLACE_ROUND_ROBIN, 0) == 2);
    nodes[2].eligible = false;                           /* offline or draining */
    assert(nx_select_node(nodes, false, NX_PLACE_LEAST_LOADED, 0) == -1);
    nodes[3] = (nx_placement_node_t){
        .eligible = true, .k = 1, .requested_k = 1, .cores = 1, .cpu_mhz = 160,
    };
    assert(nx_select_node(nodes, false, NX_PLACE_ROUND_ROBIN, 2) == 3);
    assert(nx_select_node(nodes, false, NX_PLACE_LEAST_LOADED, 0) == 3);
    nodes[3].inflight = 2;
    assert(nx_select_node(nodes, false, NX_PLACE_ROUND_ROBIN, 0) == -1);
}

int main(void)
{
    test_order(NX_ORDER_FCFS, 1);
    test_order(NX_ORDER_PRIORITY, 2);
    test_order(NX_ORDER_AGING, 1);
    test_completion();
    test_late_result_cancels_retry();
    test_saturated_requeue_and_leases();
    test_placement();
    puts("PASS queue policies, bounded tokens, duplicate results, lease/retry and hardware-aware placement");
    return 0;
}
