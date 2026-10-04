#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_LIBRARY_MAX_TRACKS 128
#define MUSIC_LIBRARY_PATH_MAX   320

typedef struct {
    char paths[MUSIC_LIBRARY_MAX_TRACKS][MUSIC_LIBRARY_PATH_MAX];
    size_t count;
} music_library_t;

esp_err_t music_library_scan(music_library_t *library,
                             const char *mount_point);
const char *music_library_track(const music_library_t *library, size_t index);

#ifdef __cplusplus
}
#endif
