idf_component_register(
    SRCS "main.c" "wifi_sta.c" "worker_link.c"
    INCLUDE_DIRS "."
    REQUIRES nexus_core esp_wifi esp_netif esp_event nvs_flash esp_timer
)
