/*
 * NEXUS master (ESP32-S3 #1).
 *
 * Creates the cluster's Wi-Fi network, the global job queue, the scheduler
 * and supervisor (cluster.c), the producers (producers.c) and the dashboard
 * server (web.c). Workers connect by themselves; with "single-node mode" the
 * master also runs jobs on its own executor pool.
 */
#include "nvs_flash.h"
#include "nexus_config.h"
#include "job_queue.h"
#include "logger.h"
#include "oslab.h"
#include "stats.h"
#include "wifi_ap.h"
#include "cluster.h"
#include "producers.h"
#include "web.h"
#include "experiments.h"

static nx_job_queue_t s_global_q;

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    logger_start();
    oslab_init();
    nx_stats_init();

    if (!jq_init(&s_global_q, NX_QUEUE_CAPACITY, NX_INFLIGHT_CAPACITY)) {
        nx_log("SYS", NX_LOG_ERR, "out of memory for the job queue");
        return;
    }
    wifi_ap_start();
    cluster_start(&s_global_q);
    producers_start(&s_global_q);
    exp_start(&s_global_q);
    web_start(&s_global_q);
    nx_log("SYS", NX_LOG_OK, "master up: Wi-Fi \"%s\", dashboard at http://192.168.4.1/", CONFIG_NX_WIFI_SSID);
}
