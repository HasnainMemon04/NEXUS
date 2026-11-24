# The dashboard page is gzip-compressed at configure time and embedded in flash.
set(WWW_SRC ${CMAKE_CURRENT_LIST_DIR}/www/index.html)
set(WWW_GZ  ${CMAKE_CURRENT_BINARY_DIR}/index.html.gz)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    file(ARCHIVE_CREATE OUTPUT ${WWW_GZ} PATHS ${WWW_SRC} FORMAT raw COMPRESSION GZip)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${WWW_SRC})
endif()

idf_component_register(
    SRCS "main.c" "wifi_ap.c" "cluster.c" "producers.c" "stats.c" "web.c" "experiments.c"
    INCLUDE_DIRS "."
    REQUIRES nexus_core esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_timer
    EMBED_FILES ${WWW_GZ}
)
