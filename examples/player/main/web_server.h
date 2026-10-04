#pragma once

#include "esp_err.h"
#include "music_library.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_server_start(const music_library_t *library);

#ifdef __cplusplus
}
#endif
