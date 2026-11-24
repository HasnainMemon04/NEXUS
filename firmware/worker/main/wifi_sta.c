/*
 * wifi_sta.c — connect to the master's access point and stay connected.
 * Power saving is switched off: the default modem-sleep mode adds up to one
 * beacon interval (~100 ms) of delay to every message the worker receives.
 */
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "logger.h"
#include "wifi_sta.h"

#define BIT_CONNECTED BIT0
#define RETRY_START_MS 250u
#define RETRY_MAX_MS 4000u

static EventGroupHandle_t s_events;
static esp_timer_handle_t s_retry;
static uint32_t s_retry_ms = RETRY_START_MS;
static _Atomic uint32_t s_gateway;
static _Atomic uint32_t s_generation;

static bool clear_connection(void)
{
    EventBits_t previous = xEventGroupClearBits(s_events, BIT_CONNECTED);
    if ((previous & BIT_CONNECTED) == 0) return false;
    atomic_fetch_add(&s_generation, 1);
    return true;
}

static void retry_connect(void *arg)
{
    esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK && !(xEventGroupGetBits(s_events) & BIT_CONNECTED)) {
        nx_log("NET", NX_LOG_WARN, "Wi-Fi connection request failed: %s", esp_err_to_name(e));
        (void)esp_timer_start_once(s_retry, (uint64_t)RETRY_MAX_MS * 1000);
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        retry_connect(NULL);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        bool had_ip = clear_connection();
        const wifi_event_sta_disconnected_t *event = data;
        if (had_ip) s_retry_ms = RETRY_START_MS;
        if (had_ip || s_retry_ms == RETRY_START_MS || s_retry_ms == RETRY_MAX_MS) {
            nx_log("NET", NX_LOG_WARN, "Wi-Fi disconnected: reason %u, RSSI %d dBm, retry %lu ms",
                   event->reason, event->rssi, (unsigned long)s_retry_ms);
        }
        (void)esp_timer_stop(s_retry);
        uint32_t delay_ms = s_retry_ms + esp_random() % 125;
        (void)esp_timer_start_once(s_retry, (uint64_t)delay_ms * 1000);
        if (s_retry_ms < RETRY_MAX_MS) s_retry_ms *= 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        if (clear_connection()) {
            nx_log("NET", NX_LOG_WARN, "Wi-Fi station lost its address, re-associating");
            if (esp_wifi_disconnect() != ESP_OK) retry_connect(NULL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        if (e->ip_changed || atomic_load(&s_gateway) != e->ip_info.gw.addr) clear_connection();
        (void)esp_timer_stop(s_retry);
        s_retry_ms = RETRY_START_MS;
        atomic_store(&s_gateway, e->ip_info.gw.addr);
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            nx_log("NET", NX_LOG_INFO, "AP channel %u, RSSI %d dBm", ap.primary, ap.rssi);
        }
        nx_log("NET", NX_LOG_OK, "joined %s, my IP " IPSTR ", master " IPSTR,
               CONFIG_NX_WIFI_SSID, IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

void wifi_sta_start(void)
{
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(s_events ? ESP_OK : ESP_ERR_NO_MEM);
    const esp_timer_create_args_t retry_args = { .callback = retry_connect, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_event, NULL, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, CONFIG_NX_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_NX_WIFI_PASSWORD, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.channel = CONFIG_NX_WIFI_CHANNEL;

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

bool wifi_sta_wait_connected(TickType_t wait)
{
    return xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE, wait) & BIT_CONNECTED;
}

bool wifi_sta_link(uint32_t *gateway, uint32_t *generation)
{
    uint32_t before = atomic_load(&s_generation);
    if ((xEventGroupGetBits(s_events) & BIT_CONNECTED) == 0) return false;
    uint32_t ip = atomic_load(&s_gateway);
    if (ip == 0 || before != atomic_load(&s_generation)) return false;
    *gateway = ip;
    *generation = before;
    return true;
}
