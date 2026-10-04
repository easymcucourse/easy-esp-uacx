#include "music_library.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"

#define MAX_SCAN_DEPTH 5

static const char *TAG = "music_library";

static bool has_supported_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot != NULL &&
           (strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".wav") == 0 ||
            strcasecmp(dot, ".flac") == 0);
}

static int compare_paths(const void *left, const void *right)
{
    return strcasecmp((const char *)left, (const char *)right);
}

static esp_err_t scan_directory(music_library_t *library, const char *path,
                                unsigned depth)
{
    DIR *directory = opendir(path);
    if (directory == NULL) {
        ESP_LOGW(TAG, "cannot open %s", path);
        return ESP_FAIL;
    }

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL &&
           library->count < MUSIC_LIBRARY_MAX_TRACKS) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0 ||
            strcasecmp(entry->d_name, "System Volume Information") == 0) {
            continue;
        }

        char full_path[MUSIC_LIBRARY_PATH_MAX];
        const int length = snprintf(full_path, sizeof(full_path), "%s/%s", path,
                                    entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(full_path)) {
            ESP_LOGW(TAG, "path is too long: %s/%s", path, entry->d_name);
            continue;
        }

        struct stat status;
        if (stat(full_path, &status) != 0) {
            continue;
        }
        if (S_ISDIR(status.st_mode) && depth < MAX_SCAN_DEPTH) {
            scan_directory(library, full_path, depth + 1);
        } else if (S_ISREG(status.st_mode) &&
                   has_supported_extension(entry->d_name)) {
            strlcpy(library->paths[library->count], full_path,
                    sizeof(library->paths[library->count]));
            ++library->count;
        }
    }

    closedir(directory);
    return ESP_OK;
}

esp_err_t music_library_scan(music_library_t *library,
                             const char *mount_point)
{
    if (library == NULL || mount_point == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(library, 0, sizeof(*library));
    const esp_err_t error = scan_directory(library, mount_point, 0);
    if (library->count > 1) {
        qsort(library->paths, library->count, sizeof(library->paths[0]),
              compare_paths);
    }

    ESP_LOGI(TAG, "found %u MP3/WAV/FLAC track(s)",
             (unsigned)library->count);
    for (size_t index = 0; index < library->count; ++index) {
        ESP_LOGI(TAG, "%3u: %s", (unsigned)(index + 1),
                 library->paths[index]);
    }
    return error;
}

const char *music_library_track(const music_library_t *library, size_t index)
{
    if (library == NULL || index >= library->count) {
        return NULL;
    }
    return library->paths[index];
}
