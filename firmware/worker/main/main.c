/*
 * NEXUS worker for classic ESP32 and ESP32-S3.
 *
 * The worker runs the same node engine as the master's single-node mode:
 * a bounded local job queue, a fixed pool of N = 4 executor tasks and K
 * execution permits. Jobs arrive from the master as DISPATCH messages and
 * results go back as RESULT messages.
 */
#include "nvs_flash.h"
#include "nexus_config.h"
#include "job_queue.h"
#include "exec_pool.h"
#include "logger.h"
#include "oslab.h"
#include "wifi_sta.h"
#include "worker_link.h"

static nx_job_queue_t s_local_q;
static nx_pool_t s_pool;

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    logger_start();
    oslab_init();

    ESP_ERROR_CHECK(jq_init(&s_local_q, NX_LOCAL_QUEUE_CAP, 0) ? ESP_OK : ESP_ERR_NO_MEM);
    worker_link_init(&s_local_q, &s_pool);
    const uint8_t slots = configNUMBER_OF_CORES < NX_DEFAULT_SLOTS ? configNUMBER_OF_CORES : NX_DEFAULT_SLOTS;
    pool_start(&s_pool, &s_local_q, slots, worker_link_on_result, NULL);

    wifi_sta_start();
    worker_link_start();
    nx_log("SYS", NX_LOG_OK, "worker up: %s, %u active FreeRTOS core(s), N = %d, K = %u",
           CONFIG_IDF_TARGET, (unsigned)configNUMBER_OF_CORES, NX_POOL_SIZE, slots);
}
