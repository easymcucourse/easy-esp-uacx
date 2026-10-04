#include "st7789_display.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "board_pins.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "utf8_font.h"

#define RGB565(red, green, blue)                                           \
    ((uint16_t)((((uint16_t)(red) & 0xf8U) << 8) |                        \
                (((uint16_t)(green) & 0xfcU) << 3) |                      \
                ((uint16_t)(blue) >> 3)))

typedef struct {
    char character;
    uint8_t rows[7];
} glyph_t;

static const char *TAG = "st7789";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static SemaphoreHandle_t s_transfer_done;
static SemaphoreHandle_t s_display_lock;
static bool s_ready;

/* Rows are rendered here, then copied into the large internal DMA buffer. */
static uint16_t s_scanline[MUSIC_LCD_WIDTH] __attribute__((aligned(4)));

/* Large internal DMA buffer shared by full-width batches and small updates. */
#define LCD_DMA_BUFFER_ROWS 66
#define CD_SIZE             48
#define CD_X                (MUSIC_LCD_WIDTH - CD_SIZE - 8)
#define CD_Y                102
static uint16_t *s_dma_buffer;
static int s_batch_start_y = -1;
static int s_batch_rows;
static unsigned s_cd_frame;

static const glyph_t FONT[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'%', {0x19, 0x1a, 0x04, 0x04, 0x04, 0x0b, 0x13}},
    {'-', {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c}},
    {'/', {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}},
    {':', {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00}},
    {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}},
    {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}},
    {'3', {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}},
    {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}},
    {'5', {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}},
    {'6', {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}},
    {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}},
    {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x1c}},
    {'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'B', {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}},
    {'C', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}},
    {'D', {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}},
    {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}},
    {'F', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}},
    {'G', {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}},
    {'H', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'I', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f}},
    {'J', {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0e}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}},
    {'M', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}},
    {'Q', {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}},
    {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}},
    {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}},
    {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}},
    {'X', {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}},
};

static const uint8_t *glyph_rows(char character)
{
    character = (char)toupper((unsigned char)character);
    for (size_t index = 0; index < sizeof(FONT) / sizeof(FONT[0]); ++index) {
        if (FONT[index].character == character) {
            return FONT[index].rows;
        }
    }
    return FONT[0].rows;
}

static bool lcd_transfer_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                 esp_lcd_panel_io_event_data_t *event_data,
                                 void *user_context)
{
    (void)panel_io;
    (void)event_data;
    (void)user_context;
    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_transfer_done, &high_task_woken);
    return high_task_woken == pdTRUE;
}

static void render_text_row(uint16_t *pixels, int screen_y, const char *text,
                            int x, int y, int scale, uint16_t color)
{
    if (text == NULL || screen_y < y || screen_y >= y + 7 * scale) {
        return;
    }

    const int font_row = (screen_y - y) / scale;
    for (size_t index = 0; text[index] != '\0'; ++index) {
        const uint8_t *rows = glyph_rows(text[index]);
        for (int column = 0; column < 5; ++column) {
            if ((rows[font_row] & (1U << (4 - column))) == 0) {
                continue;
            }
            const int pixel_x = x + (int)index * 6 * scale + column * scale;
            for (int dx = 0; dx < scale; ++dx) {
                if (pixel_x + dx >= 0 && pixel_x + dx < MUSIC_LCD_WIDTH) {
                    pixels[pixel_x + dx] = color;
                }
            }
        }
    }
}

static void render_utf8_text_row(uint16_t *pixels, int screen_y,
                                 const char *text, int x, int y,
                                 uint16_t color)
{
    if (text == NULL || screen_y < y || screen_y >= y + 16) {
        return;
    }

    const int glyph_row = screen_y - y;
    const char *cursor = text;
    while (*cursor != '\0' && x < MUSIC_LCD_WIDTH) {
        uint16_t codepoint;
        if (!utf8_font_decode(&cursor, &codepoint)) {
            break;
        }
        const int width = codepoint < 0x0100U ? 8 : 16;
        uint8_t glyph[32];
        if (!utf8_font_get_glyph(codepoint, glyph)) {
            (void)utf8_font_get_glyph('?', glyph);
        }
        const uint16_t row_bits =
            ((uint16_t)glyph[glyph_row * 2] << 8) |
            glyph[glyph_row * 2 + 1];
        for (int column = 0; column < width && x + column < MUSIC_LCD_WIDTH;
             ++column) {
            if ((row_bits & (0x8000U >> column)) != 0) {
                pixels[x + column] = color;
            }
        }
        x += width;
    }
}

static int centered_text_x(const char *text, int scale)
{
    const int width = (int)strlen(text) * 6 * scale - scale;
    return (MUSIC_LCD_WIDTH - width) / 2;
}

static void fill_line(uint16_t *pixels, uint16_t color)
{
    for (int x = 0; x < MUSIC_LCD_WIDTH; ++x) {
        pixels[x] = color;
    }
}

static void draw_horizontal_rule(uint16_t *pixels, int y, int rule_y,
                                 uint16_t color)
{
    if (y == rule_y) {
        fill_line(pixels, color);
    }
}

static void draw_cd_row(uint16_t *pixels, int screen_y, int x, int y,
                        unsigned frame)
{
    if (screen_y < y || screen_y >= y + CD_SIZE) {
        return;
    }
    static const int marker_x[4] = {23, 39, 23, 7};
    static const int marker_y[4] = {7, 23, 39, 23};
    const int local_y = screen_y - y;
    for (int local_x = 0; local_x < CD_SIZE; ++local_x) {
        const int dx = local_x * 2 - (CD_SIZE - 1);
        const int dy = local_y * 2 - (CD_SIZE - 1);
        const int radius2 = dx * dx + dy * dy;
        uint16_t color = RGB565(13, 20, 36);
        if (radius2 <= 2209) {
            color = radius2 < 100 ? RGB565(8, 15, 26)
                                  : (radius2 < 420 ? RGB565(100, 120, 135)
                                                   : RGB565(190, 205, 215));
            if ((radius2 > 720 && radius2 < 790) ||
                (radius2 > 1320 && radius2 < 1400) ||
                (radius2 > 1840 && radius2 < 1920)) {
                color = RGB565(115, 140, 155);
            }
        }
        if (local_x >= marker_x[frame & 3U] - 2 &&
            local_x <= marker_x[frame & 3U] + 2 &&
            local_y >= marker_y[frame & 3U] - 2 &&
            local_y <= marker_y[frame & 3U] + 2 && radius2 <= 2209) {
            color = RGB565(70, 220, 235);
        }
        pixels[x + local_x] = color;
    }
}

static esp_err_t flush_scanline_batch(void)
{
    if (s_batch_rows == 0) {
        return ESP_OK;
    }

    while (xSemaphoreTake(s_transfer_done, 0) == pdTRUE) {
    }
    esp_err_t error = esp_lcd_panel_draw_bitmap(
        s_panel, 0, s_batch_start_y, MUSIC_LCD_WIDTH,
        s_batch_start_y + s_batch_rows, s_dma_buffer);
    if (error != ESP_OK) {
        s_batch_rows = 0;
        s_batch_start_y = -1;
        return error;
    }
    if (xSemaphoreTake(s_transfer_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        s_batch_rows = 0;
        s_batch_start_y = -1;
        return ESP_ERR_TIMEOUT;
    }
    s_batch_rows = 0;
    s_batch_start_y = -1;
    return ESP_OK;
}

static esp_err_t draw_scanline(int y)
{
    if (s_batch_rows > 0 && y != s_batch_start_y + s_batch_rows) {
        const esp_err_t error = flush_scanline_batch();
        if (error != ESP_OK) {
            return error;
        }
    }
    if (s_batch_rows == 0) {
        s_batch_start_y = y;
    }

    memcpy(s_dma_buffer + s_batch_rows * MUSIC_LCD_WIDTH, s_scanline,
           sizeof(s_scanline));
    ++s_batch_rows;
    return s_batch_rows >= LCD_DMA_BUFFER_ROWS
               ? flush_scanline_batch()
               : ESP_OK;
}

esp_err_t st7789_display_show(const char *status, const char *detail)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_display_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const uint16_t navy = RGB565(8, 18, 38);
    const uint16_t blue = RGB565(20, 76, 170);
    const uint16_t cyan = RGB565(68, 215, 235);
    const uint16_t white = RGB565(245, 248, 255);
    const uint16_t green = RGB565(70, 220, 145);
    esp_err_t error = ESP_OK;

    for (int y = 0; y < MUSIC_LCD_HEIGHT; ++y) {
        const uint16_t background = y < 66 ? blue : navy;
        for (int x = 0; x < MUSIC_LCD_WIDTH; ++x) {
            s_scanline[x] = background;
        }

        render_text_row(s_scanline, y, "ESP32 MUSIC",
                        centered_text_x("ESP32 MUSIC", 2), 24, 2, white);
        render_text_row(s_scanline, y, status, centered_text_x(status, 3),
                        91, 3, green);
        render_text_row(s_scanline, y, detail, centered_text_x(detail, 2),
                        141, 2, white);
        render_text_row(s_scanline, y, "SPI3 LCD / SPI2 SD",
                        centered_text_x("SPI3 LCD / SPI2 SD", 1),
                        207, 1, cyan);

        error = draw_scanline(y);
        if (error != ESP_OK) {
            break;
        }
    }

    if (error == ESP_OK) {
        error = flush_scanline_batch();
    }
    xSemaphoreGive(s_display_lock);
    return error;
}

esp_err_t st7789_display_show_player(const char *hardware_info,
                                     const char *ip_address,
                                     const char *sound_card,
                                     const char *filename,
                                     const char *media_info)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (hardware_info == NULL) {
        hardware_info = "ESP32S3 SD20M";
    }
    if (ip_address == NULL || ip_address[0] == '\0') {
        ip_address = "IP WAIT";
    }
    if (sound_card == NULL || sound_card[0] == '\0') {
        sound_card = "I2S PCM5100A";
    }
    if (filename == NULL || filename[0] == '\0') {
        filename = "NO FILE";
    }
    if (media_info == NULL || media_info[0] == '\0') {
        media_info = "WAIT FORMAT";
    }
    if (xSemaphoreTake(s_display_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const uint16_t background = RGB565(6, 9, 18);
    const uint16_t top = RGB565(12, 34, 62);
    const uint16_t panel = RGB565(13, 20, 36);
    const uint16_t rule = RGB565(38, 80, 120);
    const uint16_t white = RGB565(238, 244, 255);
    const uint16_t cyan = RGB565(70, 220, 235);
    const uint16_t green = RGB565(90, 230, 150);
    esp_err_t error = ESP_OK;

    for (int y = 0; y < MUSIC_LCD_HEIGHT; ++y) {
        uint16_t line_color = background;
        if (y < 42) {
            line_color = top;
        } else if (y >= 58 && y < 154) {
            line_color = panel;
        }
        fill_line(s_scanline, line_color);
        draw_horizontal_rule(s_scanline, y, 42, rule);
        draw_horizontal_rule(s_scanline, y, 154, rule);

        render_text_row(s_scanline, y, hardware_info, 8, 5, 1, cyan);
        render_text_row(s_scanline, y, ip_address,
                        MUSIC_LCD_WIDTH - (int)strlen(ip_address) * 6 - 8,
                        5, 1, green);
        render_text_row(s_scanline, y, sound_card, 8, 24, 1, white);
        render_text_row(s_scanline, y, "NOW PLAYING", 8, 63, 1, cyan);
        render_utf8_text_row(s_scanline, y, filename, 8, 84, white);
        render_text_row(s_scanline, y, media_info, 8, 122, 1, green);
        render_text_row(s_scanline, y, "FILES", 8, 162, 1, cyan);
        draw_cd_row(s_scanline, y, CD_X, CD_Y, s_cd_frame);

        error = draw_scanline(y);
        if (error != ESP_OK) {
            break;
        }
    }

    if (error == ESP_OK) {
        error = flush_scanline_batch();
    }
    xSemaphoreGive(s_display_lock);
    return error;
}

esp_err_t st7789_display_update_track_info(const char *filename,
                                           const char *media_info)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (filename == NULL || filename[0] == '\0') {
        filename = "NO FILE";
    }
    if (media_info == NULL || media_info[0] == '\0') {
        media_info = "WAIT FORMAT";
    }
    if (xSemaphoreTake(s_display_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const uint16_t panel = RGB565(13, 20, 36);
    const uint16_t rule = RGB565(38, 80, 120);
    const uint16_t white = RGB565(238, 244, 255);
    const uint16_t cyan = RGB565(70, 220, 235);
    const uint16_t green = RGB565(90, 230, 150);
    esp_err_t error = ESP_OK;

    for (int y = 58; y < 155; ++y) {
        fill_line(s_scanline, panel);
        draw_horizontal_rule(s_scanline, y, 154, rule);
        render_text_row(s_scanline, y, "NOW PLAYING", 8, 63, 1, cyan);
        render_utf8_text_row(s_scanline, y, filename, 8, 84, white);
        render_text_row(s_scanline, y, media_info, 8, 122, 1, green);
        draw_cd_row(s_scanline, y, CD_X, CD_Y, s_cd_frame);
        error = draw_scanline(y);
        if (error != ESP_OK) {
            break;
        }
    }

    if (error == ESP_OK) {
        error = flush_scanline_batch();
    }
    xSemaphoreGive(s_display_lock);
    return error;
}

esp_err_t st7789_display_update_status_bar(const char *hardware_info,
                                           const char *ip_address,
                                           const char *sound_card)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (hardware_info == NULL) {
        hardware_info = "ESP32S3 SD20M";
    }
    if (ip_address == NULL || ip_address[0] == '\0') {
        ip_address = "IP WAIT";
    }
    if (sound_card == NULL || sound_card[0] == '\0') {
        sound_card = "I2S PCM5100A";
    }
    if (xSemaphoreTake(s_display_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const uint16_t background = RGB565(6, 9, 18);
    const uint16_t top = RGB565(12, 34, 62);
    const uint16_t rule = RGB565(38, 80, 120);
    const uint16_t white = RGB565(238, 244, 255);
    const uint16_t cyan = RGB565(70, 220, 235);
    const uint16_t green = RGB565(90, 230, 150);
    esp_err_t error = ESP_OK;

    for (int y = 0; y < 43; ++y) {
        uint16_t line_color = background;
        if (y < 42) {
            line_color = top;
        }
        fill_line(s_scanline, line_color);
        draw_horizontal_rule(s_scanline, y, 42, rule);

        render_text_row(s_scanline, y, hardware_info, 8, 5, 1, cyan);
        render_text_row(s_scanline, y, ip_address,
                        MUSIC_LCD_WIDTH - (int)strlen(ip_address) * 6 - 8,
                        5, 1, green);
        render_text_row(s_scanline, y, sound_card, 8, 24, 1, white);

        error = draw_scanline(y);
        if (error != ESP_OK) {
            break;
        }
    }

    if (error == ESP_OK) {
        error = flush_scanline_batch();
    }
    xSemaphoreGive(s_display_lock);
    return error;
}

esp_err_t st7789_display_update_file_list(const char *const *filenames,
                                          size_t count,
                                          size_t selected_row)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_display_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const uint16_t background = RGB565(6, 9, 18);
    const uint16_t selected = RGB565(20, 54, 72);
    const uint16_t white = RGB565(238, 244, 255);
    const uint16_t cyan = RGB565(70, 220, 235);
    esp_err_t error = ESP_OK;
    if (count > 4) {
        count = 4;
    }
    for (int y = 155; y < MUSIC_LCD_HEIGHT; ++y) {
        const int row = (y - 174) / 16;
        const bool in_selected_row = y >= 174 && row >= 0 &&
                                     (size_t)row < count &&
                                     (size_t)row == selected_row;
        fill_line(s_scanline, in_selected_row ? selected : background);
        if (in_selected_row) {
            for (int x = 4; x < 8; ++x) {
                s_scanline[x] = cyan;
            }
        }
        render_text_row(s_scanline, y, "FILES", 8, 162, 1, cyan);
        for (size_t index = 0; index < count; ++index) {
            const int text_y = 174 + (int)index * 16;
            render_utf8_text_row(s_scanline, y, filenames[index], 16,
                                 text_y, index == selected_row ? cyan : white);
        }
        error = draw_scanline(y);
        if (error != ESP_OK) {
            break;
        }
    }
    if (error == ESP_OK) {
        error = flush_scanline_batch();
    }
    xSemaphoreGive(s_display_lock);
    return error;
}

esp_err_t st7789_display_update_cd(bool playing)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_display_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const uint16_t panel = RGB565(13, 20, 36);
    if (playing) {
        s_cd_frame = (s_cd_frame + 1U) & 3U;
    }
    for (int row = 0; row < CD_SIZE; ++row) {
        uint16_t *pixels = s_dma_buffer + row * CD_SIZE;
        for (int x = 0; x < CD_SIZE; ++x) {
            pixels[x] = panel;
        }
        draw_cd_row(pixels, CD_Y + row, 0, CD_Y, s_cd_frame);
    }

    while (xSemaphoreTake(s_transfer_done, 0) == pdTRUE) {
    }
    esp_err_t error = esp_lcd_panel_draw_bitmap(
        s_panel, CD_X, CD_Y, CD_X + CD_SIZE, CD_Y + CD_SIZE, s_dma_buffer);
    if (error == ESP_OK &&
        xSemaphoreTake(s_transfer_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        error = ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(s_display_lock);
    return error;
}

static void delete_panel_and_io(void)
{
    if (s_panel != NULL) {
        (void)esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    if (s_panel_io != NULL) {
        (void)esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
    }
}

static esp_err_t create_and_test_panel(int frequency_hz)
{
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = MUSIC_LCD_PIN_DC,
        .cs_gpio_num = MUSIC_LCD_PIN_CS,
        .pclk_hz = frequency_hz,
        .spi_mode = 0,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = lcd_transfer_done_cb,
    };
    esp_err_t error = esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)MUSIC_LCD_SPI_HOST,
        &io_config, &s_panel_io);
    if (error != ESP_OK) {
        return error;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = MUSIC_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    error = esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel);
    if (error != ESP_OK) {
        delete_panel_and_io();
        return error;
    }

    if ((error = esp_lcd_panel_reset(s_panel)) != ESP_OK ||
        (error = esp_lcd_panel_init(s_panel)) != ESP_OK ||
        (error = esp_lcd_panel_swap_xy(s_panel, true)) != ESP_OK ||
        (error = esp_lcd_panel_mirror(s_panel, false, true)) != ESP_OK ||
        (error = esp_lcd_panel_invert_color(s_panel, true)) != ESP_OK ||
        (error = esp_lcd_panel_set_gap(s_panel, 0, 0)) != ESP_OK ||
        (error = esp_lcd_panel_disp_on_off(s_panel, true)) != ESP_OK) {
        delete_panel_and_io();
        return error;
    }

    for (size_t index = 0;
         index < MUSIC_LCD_WIDTH * LCD_DMA_BUFFER_ROWS; ++index) {
        s_dma_buffer[index] =
            (index / MUSIC_LCD_WIDTH) & 1U ? RGB565(25, 80, 180)
                                                : RGB565(10, 25, 55);
    }
    const int64_t started_us = esp_timer_get_time();
    const int test_transfers = 3;
    for (int test = 0; test < test_transfers; ++test) {
        while (xSemaphoreTake(s_transfer_done, 0) == pdTRUE) {
        }
        error = esp_lcd_panel_draw_bitmap(
            s_panel, 0, 0, MUSIC_LCD_WIDTH, LCD_DMA_BUFFER_ROWS,
            s_dma_buffer);
        if (error != ESP_OK ||
            xSemaphoreTake(s_transfer_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            if (error == ESP_OK) {
                error = ESP_ERR_TIMEOUT;
            }
            delete_panel_and_io();
            return error;
        }
    }
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    const uint64_t transferred =
        (uint64_t)MUSIC_LCD_WIDTH * LCD_DMA_BUFFER_ROWS *
        sizeof(uint16_t) * test_transfers;
    ESP_LOGI(TAG,
             "LCD DMA test: requested=%d MHz, %u bytes in %lld us "
             "(%llu kbit/s)",
             frequency_hz / 1000000, (unsigned)transferred,
             (long long)elapsed_us,
             (unsigned long long)(transferred * 8U * 1000U /
                                  (uint64_t)elapsed_us));
    return ESP_OK;
}

static void release_resources(bool bus_initialized)
{
    delete_panel_and_io();
    if (bus_initialized) {
        (void)spi_bus_free(MUSIC_LCD_SPI_HOST);
    }
    if (s_display_lock != NULL) {
        vSemaphoreDelete(s_display_lock);
        s_display_lock = NULL;
    }
    if (s_transfer_done != NULL) {
        vSemaphoreDelete(s_transfer_done);
        s_transfer_done = NULL;
    }
    if (s_dma_buffer != NULL) {
        heap_caps_free(s_dma_buffer);
        s_dma_buffer = NULL;
    }
}

esp_err_t st7789_display_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_transfer_done = xSemaphoreCreateBinary();
    s_display_lock = xSemaphoreCreateMutex();
    s_dma_buffer = heap_caps_malloc(
        MUSIC_LCD_WIDTH * LCD_DMA_BUFFER_ROWS * sizeof(uint16_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_transfer_done == NULL || s_display_lock == NULL ||
        s_dma_buffer == NULL) {
        release_resources(false);
        return ESP_ERR_NO_MEM;
    }

    const spi_bus_config_t bus_config = {
        .sclk_io_num = MUSIC_LCD_PIN_SCLK,
        .mosi_io_num = MUSIC_LCD_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = MUSIC_LCD_WIDTH * LCD_DMA_BUFFER_ROWS *
                               sizeof(uint16_t) + 8,
    };
    esp_err_t error = spi_bus_initialize(MUSIC_LCD_SPI_HOST, &bus_config,
                                         SPI_DMA_CH_AUTO);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "SPI3 bus init failed: %s", esp_err_to_name(error));
        release_resources(false);
        return error;
    }

    static const int test_frequencies_hz[] = {
        80 * 1000 * 1000,
        60 * 1000 * 1000,
        40 * 1000 * 1000,
        20 * 1000 * 1000,
    };
    int selected_frequency_hz = 0;
    for (size_t index = 0;
         index < sizeof(test_frequencies_hz) / sizeof(test_frequencies_hz[0]);
         ++index) {
        error = create_and_test_panel(test_frequencies_hz[index]);
        if (error == ESP_OK) {
            selected_frequency_hz = test_frequencies_hz[index];
            break;
        }
        ESP_LOGW(TAG, "LCD DMA test failed at %d MHz: %s",
                 test_frequencies_hz[index] / 1000000,
                 esp_err_to_name(error));
    }
    if (selected_frequency_hz == 0) {
        ESP_LOGE(TAG, "LCD failed at all tested SPI frequencies");
        release_resources(true);
        return error;
    }

    s_ready = true;
    ESP_LOGI(TAG,
             "ST7789 ready: native 240x320, logical 320x240, left 90 degrees; "
             "SPI=%d MHz DMA=%u bytes SCLK=%d MOSI=%d RST=%d DC=%d CS=%d",
             selected_frequency_hz / 1000000,
             (unsigned)(MUSIC_LCD_WIDTH * LCD_DMA_BUFFER_ROWS *
                        sizeof(uint16_t)),
             MUSIC_LCD_PIN_SCLK, MUSIC_LCD_PIN_MOSI, MUSIC_LCD_PIN_RST,
             MUSIC_LCD_PIN_DC, MUSIC_LCD_PIN_CS);
    return ESP_OK;
}
