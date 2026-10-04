#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t st7789_display_init(void);
esp_err_t st7789_display_show(const char *status, const char *detail);
esp_err_t st7789_display_show_player(const char *hardware_info,
                                     const char *ip_address,
                                     const char *sound_card,
                                     const char *filename,
                                     const char *media_info);
esp_err_t st7789_display_update_track_info(const char *filename,
                                           const char *media_info);
esp_err_t st7789_display_update_status_bar(const char *hardware_info,
                                           const char *ip_address,
                                           const char *sound_card);
esp_err_t st7789_display_update_file_list(const char *const *filenames,
                                          size_t count,
                                          size_t selected_row);
esp_err_t st7789_display_update_cd(bool playing);

#ifdef __cplusplus
}
#endif
