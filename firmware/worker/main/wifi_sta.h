/* wifi_sta.h — the worker joins the master's Wi-Fi network as a station. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"

void     wifi_sta_start(void);
bool     wifi_sta_wait_connected(TickType_t wait);
bool     wifi_sta_link(uint32_t *gateway, uint32_t *generation);
