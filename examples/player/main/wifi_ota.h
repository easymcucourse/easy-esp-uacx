#pragma once

#include <stddef.h>

#include "esp_err.h"

#define WIFI_OTA_TCP_PORT 3333

/** Start Wi-Fi station mode and the dedicated TCP OTA server task. */
esp_err_t wifi_ota_start(void);

/** Copy the last station IP address, or "IP WAIT" before DHCP completes. */
void wifi_ota_get_ip_string(char *buffer, size_t buffer_size);
