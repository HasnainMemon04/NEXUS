/*
 * wifi_ap.c — SoftAP "NEXUS-OS" at 192.168.4.1. Workers and browsers join it;
 * no router or internet is involved. Failure detection does NOT rely on the
 * access point (it reports a silent station only after 300 s by default):
 * the supervisor's heartbeat timeouts do that job.
 */
#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "logger.h"
#include "wifi_ap.h"

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *e = data;
        nx_log("NET", NX_LOG_INFO, "station " MACSTR " joined", MAC2STR(e->mac));
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *e = data;
        nx_log("NET", NX_LOG_WARN, "station " MACSTR " left", MAC2STR(e->mac));
    }
}

void wifi_ap_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.ap.ssid, CONFIG_NX_WIFI_SSID, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = strlen(CONFIG_NX_WIFI_SSID);
    strlcpy((char *)wc.ap.password, CONFIG_NX_WIFI_PASSWORD, sizeof(wc.ap.password));
    wc.ap.channel = CONFIG_NX_WIFI_CHANNEL;
    wc.ap.max_connection = 6;              /* three workers and three dashboard stations */
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20));
    ESP_ERROR_CHECK(esp_wifi_start());
}
