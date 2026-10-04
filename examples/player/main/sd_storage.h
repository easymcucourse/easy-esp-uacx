#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"

uint32_t sd_storage_run_speed_sweep(void);
esp_err_t sd_storage_mount(const char *mount_point, uint32_t speed_khz,
                           sdmmc_card_t **out_card);
