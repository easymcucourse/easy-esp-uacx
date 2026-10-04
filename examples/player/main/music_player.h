#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "music_library.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t track_index;
    size_t track_count;
    unsigned volume_permille;
    bool paused;
    bool running;
    char format_text[32];
} music_player_status_t;

esp_err_t music_player_start(const music_library_t *library);
void music_player_set_track_index(size_t track_index);
void music_player_set_paused(bool paused);
void music_player_play_index(size_t track_index);
void music_player_toggle_pause(void);
void music_player_next(void);
void music_player_previous(void);
void music_player_restart(void);
void music_player_volume_up(void);
void music_player_volume_down(void);
void music_player_set_volume_permille(unsigned volume_permille);
music_player_status_t music_player_get_status(void);

#ifdef __cplusplus
}
#endif
