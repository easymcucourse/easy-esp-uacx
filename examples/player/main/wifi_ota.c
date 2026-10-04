#include "wifi_ota.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define WIFI_SSID             CONFIG_MUSIC_WIFI_SSID
#define WIFI_PASSWORD         CONFIG_MUSIC_WIFI_PASSWORD
#define OTA_HEADER_SIZE       64
#define OTA_RECEIVE_CHUNK     4096
#define OTA_SOCKET_TIMEOUT_S  20

static const char *TAG = "wifi_ota";
static char s_ip_address[16] = "IP WAIT";

static esp_err_t send_all(int socket_fd, const char *data, size_t length)
{
    size_t sent_total = 0;
    while (sent_total < length) {
        const int sent = send(socket_fd, data + sent_total,
                              length - sent_total, 0);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            return ESP_FAIL;
        }
        sent_total += (size_t)sent;
    }
    return ESP_OK;
}

static esp_err_t receive_ota_header(int socket_fd, size_t *image_size)
{
    char header[OTA_HEADER_SIZE];
    size_t length = 0;

    while (length + 1U < sizeof(header)) {
        char byte;
        const int received = recv(socket_fd, &byte, 1, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (byte == '\n') {
            header[length] = '\0';
            break;
        }
        if (byte != '\r') {
            header[length++] = byte;
        }
    }

    if (length + 1U >= sizeof(header) || strncmp(header, "OTA ", 4) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = strtoull(header + 4, &end, 10);
    if (errno != 0 || end == header + 4 || *end != '\0' || parsed == 0 ||
        parsed > SIZE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    *image_size = (size_t)parsed;
    return ESP_OK;
}

static esp_err_t receive_ota_image(int socket_fd, size_t image_size)
{
    const esp_partition_t *partition =
        esp_ota_get_next_update_partition(NULL);
    if (partition == NULL || image_size > partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t result = esp_ota_begin(partition, image_size, &ota_handle);
    if (result != ESP_OK) {
        return result;
    }
    bool ota_started = true;

    static const char ready[] = "OTA READY\r\n";
    result = send_all(socket_fd, ready, sizeof(ready) - 1U);
    if (result != ESP_OK) {
        goto fail;
    }

    uint8_t *buffer = malloc(OTA_RECEIVE_CHUNK);
    if (buffer == NULL) {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    ESP_LOGI(TAG, "receiving %u bytes into partition %s",
             (unsigned)image_size, partition->label);
    size_t received_total = 0;
    size_t next_progress = 256U * 1024U;
    while (received_total < image_size) {
        size_t remaining = image_size - received_total;
        if (remaining > OTA_RECEIVE_CHUNK) {
            remaining = OTA_RECEIVE_CHUNK;
        }

        const int received = recv(socket_fd, buffer, remaining, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            ESP_LOGE(TAG, "OTA receive stopped at %u/%u bytes: errno=%d",
                     (unsigned)received_total, (unsigned)image_size, errno);
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }

        result = esp_ota_write(ota_handle, buffer, (size_t)received);
        if (result != ESP_OK) {
            break;
        }
        received_total += (size_t)received;
        if (received_total >= next_progress && received_total < image_size) {
            ESP_LOGI(TAG, "OTA progress: %u/%u bytes",
                     (unsigned)received_total, (unsigned)image_size);
            next_progress += 256U * 1024U;
        }
    }
    free(buffer);

    if (result != ESP_OK) {
        goto fail;
    }

    result = esp_ota_end(ota_handle);
    ota_started = false;
    if (result != ESP_OK) {
        goto fail;
    }

    result = esp_ota_set_boot_partition(partition);
    if (result != ESP_OK) {
        goto fail;
    }

    ESP_LOGI(TAG, "OTA complete: %u bytes -> %s",
             (unsigned)image_size, partition->label);
    static const char complete[] = "OTA OK REBOOTING\r\n";
    send_all(socket_fd, complete, sizeof(complete) - 1U);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;

fail:
    if (ota_started) {
        esp_ota_abort(ota_handle);
    }
    return result;
}

static void handle_ota_client(int socket_fd)
{
    const struct timeval timeout = {
        .tv_sec = OTA_SOCKET_TIMEOUT_S,
        .tv_usec = 0,
    };
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
               sizeof(timeout));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
               sizeof(timeout));

    size_t image_size = 0;
    esp_err_t result = receive_ota_header(socket_fd, &image_size);
    if (result != ESP_OK) {
        static const char usage[] = "ERR USE: OTA <SIZE>\r\n";
        send_all(socket_fd, usage, sizeof(usage) - 1U);
        return;
    }

    result = receive_ota_image(socket_fd, image_size);
    if (result != ESP_OK) {
        char response[64];
        const int length = snprintf(response, sizeof(response),
                                    "OTA ERR %s\r\n",
                                    esp_err_to_name(result));
        if (length > 0) {
            const size_t response_length =
                (size_t)length < sizeof(response) ? (size_t)length
                                                  : sizeof(response) - 1U;
            send_all(socket_fd, response, response_length);
        }
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(result));
    }
}

static void ota_server_task(void *argument)
{
    (void)argument;

    for (;;) {
        const int listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listen_socket < 0) {
            ESP_LOGE(TAG, "cannot create OTA socket: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        const int reuse = 1;
        setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse));
        const struct sockaddr_in address = {
            .sin_family = AF_INET,
            .sin_port = htons(WIFI_OTA_TCP_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(listen_socket, (const struct sockaddr *)&address,
                 sizeof(address)) != 0 ||
            listen(listen_socket, 1) != 0) {
            ESP_LOGE(TAG, "cannot listen on OTA port %d: errno=%d",
                     WIFI_OTA_TCP_PORT, errno);
            close(listen_socket);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "OTA TCP server listening on port %d",
                 WIFI_OTA_TCP_PORT);
        for (;;) {
            struct sockaddr_in source_address;
            socklen_t source_length = sizeof(source_address);
            const int client = accept(listen_socket,
                                      (struct sockaddr *)&source_address,
                                      &source_length);
            if (client < 0) {
                ESP_LOGE(TAG, "OTA accept failed: errno=%d", errno);
                break;
            }

            char address_text[INET_ADDRSTRLEN] = {0};
            inet_ntoa_r(source_address.sin_addr, address_text,
                        sizeof(address_text));
            ESP_LOGI(TAG, "OTA client connected from %s", address_text);
            handle_ota_client(client);
            shutdown(client, SHUT_RDWR);
            close(client);
        }
        close(listen_socket);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "connecting to Wi-Fi SSID %s", WIFI_SSID);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %u); reconnecting",
                 event != NULL ? (unsigned)event->reason : 0U);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        snprintf(s_ip_address, sizeof(s_ip_address), IPSTR,
                 IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Wi-Fi connected; IP=" IPSTR ", OTA port=%d",
                 IP2STR(&event->ip_info.ip), WIFI_OTA_TCP_PORT);
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        result = nvs_flash_erase();
        if (result == ESP_OK) {
            result = nvs_flash_init();
        }
    }
    return result;
}

esp_err_t wifi_ota_start(void)
{
    if (WIFI_SSID[0] == '\0') {
        snprintf(s_ip_address, sizeof(s_ip_address), "WIFI OFF");
        ESP_LOGW(TAG,
                 "Wi-Fi disabled: configure Music Player > Wi-Fi SSID "
                 "with idf.py menuconfig");
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t result = init_nvs();
    if (result != ESP_OK) {
        return result;
    }

    result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }

    const wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&initialization);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL);
    if (result != ESP_OK) {
        return result;
    }

    wifi_config_t configuration = {0};
    snprintf((char *)configuration.sta.ssid,
             sizeof(configuration.sta.ssid), "%s", WIFI_SSID);
    snprintf((char *)configuration.sta.password,
             sizeof(configuration.sta.password), "%s", WIFI_PASSWORD);
    configuration.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    configuration.sta.pmf_cfg.capable = true;
    configuration.sta.pmf_cfg.required = false;

    result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_wifi_set_config(WIFI_IF_STA, &configuration);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_wifi_start();
    if (result != ESP_OK) {
        return result;
    }

    if (xTaskCreate(ota_server_task, "tcp_ota", 6144, NULL, 4, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void wifi_ota_get_ip_string(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    snprintf(buffer, buffer_size, "%s", s_ip_address);
}
