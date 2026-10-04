#include "web_server.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_output.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "music_player.h"
#include "wifi_ota.h"

static const char *TAG = "web_server";
static const music_library_t *s_library;
static httpd_handle_t s_server;

static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>ESP32 Music Player</title>"
    "<style>"
    "body{margin:0;background:#070b14;color:#eef4ff;font-family:Arial,sans-serif}"
    "main{max-width:760px;margin:auto;padding:18px}.card{background:#111a2e;"
    "border:1px solid #25466d;border-radius:14px;padding:14px;margin:12px 0}"
    "h1{font-size:24px;margin:4px 0 12px;color:#46dceb}.state{display:grid;"
    "grid-template-columns:120px 1fr;gap:8px}.key{color:#78d991}.btn{"
    "display:inline-block;margin:6px 6px 6px 0;padding:12px 15px;border:0;"
    "border-radius:10px;background:#2d6ee6;color:white;text-decoration:none;"
    "font-weight:bold}.btn.alt{background:#263952}.tracks{padding-left:24px}"
    ".tracks li{padding:7px;border-bottom:1px solid #1e304f}.tracks a{"
    "color:#eef4ff;text-decoration:none}.tracks .current{background:#163d35;"
    "border-radius:8px}input[type=range]{width:100%}.hint{color:#9fb3d0}"
    "</style></head><body><main><h1>ESP32 Music Player</h1>"
    "<section class=\"card\"><div class=\"state\">"
    "<div class=\"key\">IP</div><div id=\"ip\">-</div>"
    "<div class=\"key\">State</div><div id=\"state\">-</div>"
    "<div class=\"key\">Sound</div><div id=\"sound\">-</div>"
    "<div class=\"key\">File</div><div id=\"file\">-</div>"
    "<div class=\"key\">Format</div><div id=\"format\">-</div>"
    "<div class=\"key\">Volume</div><div id=\"volumeText\">-</div>"
    "</div></section><section class=\"card\">"
    "<button class=\"btn\" onclick=\"cmd('toggle')\">Play / Pause</button>"
    "<button class=\"btn alt\" onclick=\"cmd('prev')\">Prev</button>"
    "<button class=\"btn alt\" onclick=\"cmd('next')\">Next</button>"
    "<button class=\"btn alt\" onclick=\"cmd('restart')\">Restart</button>"
    "<button class=\"btn alt\" onclick=\"cmd('voldown')\">Vol -</button>"
    "<button class=\"btn alt\" onclick=\"cmd('volup')\">Vol +</button>"
    "<input id=\"vol\" type=\"range\" min=\"0\" max=\"1000\" step=\"10\" "
    "onchange=\"cmd('setvol',{v:this.value})\"></section>"
    "<section class=\"card\"><h2>Playlist</h2><ol id=\"tracks\" "
    "class=\"tracks\"></ol><p id=\"err\" class=\"hint\"></p></section>"
    "<script>"
    "const $=id=>document.getElementById(id);"
    "function esc(s){return String(s==null?'':s).replace(/[&<>\\\"]/g,c=>({'&':'&amp;',"
    "'<':'&lt;','>':'&gt;','\\\"':'&quot;'}[c]));}"
    "async function api(path){const r=await fetch(path,{cache:'no-store'});"
    "if(!r.ok)throw new Error(r.status);return await r.json();}"
    "async function refresh(){try{const s=await api('/api/status');"
    "$('ip').textContent=s.ip;$('state').textContent=s.paused?'Paused':"
    "(s.running?'Playing':'Idle');$('sound').textContent=s.sound;"
    "$('file').textContent=s.file;$('format').textContent=s.format;"
    "$('volumeText').textContent=(s.volume_permille/10).toFixed(1)+'%';"
    "$('vol').value=s.volume_permille;let html='';"
    "(s.tracks||[]).forEach(t=>{html+=`<li class=\"${t.current?'current':''}\">"
    "<a href=\"#\" onclick=\"cmd('play',{track:${t.index}});return false;\">"
    "${esc(t.name)}</a></li>`});$('tracks').innerHTML=html;"
    "$('err').textContent='';}catch(e){$('err').textContent='offline / busy';}}"
    "async function cmd(a,p={}){p.action=a;const q=new URLSearchParams(p);"
    "try{await api('/api/control?'+q.toString());}catch(e){}refresh();}"
    "refresh();setInterval(refresh,2000);"
    "</script></main></body></html>";

static const char *base_filename(const char *path)
{
    if (path == NULL) {
        return "NO FILE";
    }
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static esp_err_t send_text(httpd_req_t *request, const char *text)
{
    return httpd_resp_sendstr_chunk(request, text != NULL ? text : "");
}

static esp_err_t send_json_escaped(httpd_req_t *request, const char *text)
{
    if (text == NULL) {
        return ESP_OK;
    }
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        switch (*cursor) {
        case '\\':
            ESP_RETURN_ON_ERROR(send_text(request, "\\\\"), TAG,
                                "send escaped backslash");
            break;
        case '"':
            ESP_RETURN_ON_ERROR(send_text(request, "\\\""), TAG,
                                "send escaped quote");
            break;
        case '\n':
            ESP_RETURN_ON_ERROR(send_text(request, "\\n"), TAG,
                                "send escaped newline");
            break;
        case '\r':
            ESP_RETURN_ON_ERROR(send_text(request, "\\r"), TAG,
                                "send escaped carriage return");
            break;
        case '\t':
            ESP_RETURN_ON_ERROR(send_text(request, "\\t"), TAG,
                                "send escaped tab");
            break;
        default: {
            const char single[] = {*cursor, '\0'};
            ESP_RETURN_ON_ERROR(send_text(request, single), TAG,
                                "send escaped char");
            break;
        }
        }
    }
    return ESP_OK;
}

static int query_int(httpd_req_t *request, const char *key, int fallback)
{
    char query[128];
    char value[16];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        return fallback;
    }
    errno = 0;
    char *end = NULL;
    const long parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < INT_MIN ||
        parsed > INT_MAX) {
        return fallback;
    }
    return (int)parsed;
}

static bool query_string(httpd_req_t *request, const char *key, char *value,
                         size_t value_size)
{
    char query[128];
    if (value == NULL || value_size == 0 ||
        httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query, key, value, value_size) == ESP_OK;
}

static esp_err_t status_json_handler(httpd_req_t *request)
{
    music_player_status_t status = music_player_get_status();
    const char *track_path = status.track_count > 0 && s_library != NULL
                                 ? music_library_track(s_library,
                                                       status.track_index)
                                 : NULL;
    char ip[16];
    char sound_card[40];
    char line[192];
    wifi_ota_get_ip_string(ip, sizeof(ip));
    audio_output_get_status_text(sound_card, sizeof(sound_card));

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");

    snprintf(line, sizeof(line),
             "{\"ip\":\"%s\",\"paused\":%s,\"running\":%s,"
             "\"track_index\":%u,\"track_count\":%u,"
             "\"volume_permille\":%u,\"sound\":\"",
             ip, status.paused ? "true" : "false",
             status.running ? "true" : "false",
             (unsigned)status.track_index, (unsigned)status.track_count,
             status.volume_permille);
    ESP_RETURN_ON_ERROR(send_text(request, line), TAG, "send json prefix");
    ESP_RETURN_ON_ERROR(send_json_escaped(request, sound_card), TAG,
                        "send json sound");
    ESP_RETURN_ON_ERROR(send_text(request, "\",\"file\":\""), TAG,
                        "send json file key");
    ESP_RETURN_ON_ERROR(send_json_escaped(request, base_filename(track_path)),
                        TAG, "send json file");
    ESP_RETURN_ON_ERROR(send_text(request, "\",\"format\":\""), TAG,
                        "send json format key");
    ESP_RETURN_ON_ERROR(send_json_escaped(request, status.format_text), TAG,
                        "send json format");
    ESP_RETURN_ON_ERROR(send_text(request, "\",\"tracks\":["), TAG,
                        "send json tracks key");

    if (s_library != NULL) {
        for (size_t index = 0; index < s_library->count; ++index) {
            snprintf(line, sizeof(line),
                     "%s{\"index\":%u,\"current\":%s,\"name\":\"",
                     index == 0 ? "" : ",", (unsigned)index,
                     index == status.track_index ? "true" : "false");
            ESP_RETURN_ON_ERROR(send_text(request, line), TAG,
                                "send json track open");
            ESP_RETURN_ON_ERROR(send_json_escaped(
                                    request,
                                    base_filename(music_library_track(
                                        s_library, index))),
                                TAG, "send json track name");
            ESP_RETURN_ON_ERROR(send_text(request, "\"}"), TAG,
                                "send json track close");
        }
    }

    ESP_RETURN_ON_ERROR(send_text(request, "]}"), TAG, "send json end");
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t control_json_handler(httpd_req_t *request)
{
    char action[16];
    if (!query_string(request, "action", action, sizeof(action))) {
        httpd_resp_set_status(request, "400 Bad Request");
        httpd_resp_set_type(request, "application/json; charset=utf-8");
        return httpd_resp_sendstr(request, "{\"ok\":false,\"error\":\"missing action\"}");
    }

    if (strcmp(action, "toggle") == 0) {
        music_player_toggle_pause();
    } else if (strcmp(action, "next") == 0) {
        music_player_next();
    } else if (strcmp(action, "prev") == 0) {
        music_player_previous();
    } else if (strcmp(action, "restart") == 0) {
        music_player_restart();
    } else if (strcmp(action, "volup") == 0) {
        music_player_volume_up();
    } else if (strcmp(action, "voldown") == 0) {
        music_player_volume_down();
    } else if (strcmp(action, "setvol") == 0) {
        const int permille = query_int(request, "v", -1);
        if (permille >= 0) {
            music_player_set_volume_permille((unsigned)permille);
        }
    } else if (strcmp(action, "play") == 0) {
        const int track = query_int(request, "track", -1);
        if (track >= 0) {
            music_player_play_index((size_t)track);
        }
    }

    return status_json_handler(request);
}

static esp_err_t legacy_control_get_handler(httpd_req_t *request)
{
    return control_json_handler(request);
}

static esp_err_t root_get_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "public, max-age=3600");
    return httpd_resp_send(request, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_server_start(const music_library_t *library)
{
    if (library == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_server != NULL) {
        return ESP_OK;
    }

    s_library = library;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.core_id = 0;  /* Keep HTTP off Core 1 where FLAC decodes. */

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG,
                        "start HTTP server");

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t control_uri = {
        .uri = "/control",
        .method = HTTP_GET,
        .handler = legacy_control_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t status_api_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_json_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t control_api_uri = {
        .uri = "/api/control",
        .method = HTTP_GET,
        .handler = control_json_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &root_uri), TAG,
                        "register root URI");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &control_uri),
                        TAG, "register control URI");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &status_api_uri),
                        TAG, "register status API URI");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &control_api_uri),
                        TAG, "register control API URI");

    ESP_LOGI(TAG, "HTTP music control server listening on port 80 (static UI + JSON API)");
    return ESP_OK;
}
