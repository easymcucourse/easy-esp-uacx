#include "utf8_font.h"

#include <stddef.h>
#include <string.h>

/* Arduino uses PROGMEM as an annotation. ESP-IDF maps const data from flash. */
#define PROGMEM
#include "efont/efontEnableAll.h"
#include "efont/efontFontData.h"

bool utf8_font_decode(const char **text, uint16_t *codepoint)
{
    if (text == NULL || *text == NULL || codepoint == NULL || **text == '\0') {
        return false;
    }

    const uint8_t *input = (const uint8_t *)*text;
    if (input[0] < 0x80U) {
        *codepoint = input[0];
        *text += 1;
        return true;
    }
    if ((input[0] & 0xe0U) == 0xc0U &&
        (input[1] & 0xc0U) == 0x80U) {
        *codepoint = (uint16_t)(((input[0] & 0x1fU) << 6) |
                               (input[1] & 0x3fU));
        *text += 2;
        return true;
    }
    if ((input[0] & 0xf0U) == 0xe0U &&
        (input[1] & 0xc0U) == 0x80U &&
        (input[2] & 0xc0U) == 0x80U) {
        *codepoint = (uint16_t)(((input[0] & 0x0fU) << 12) |
                               ((input[1] & 0x3fU) << 6) |
                               (input[2] & 0x3fU));
        *text += 3;
        return true;
    }

    /* efont is UTF-16/BMP-only; malformed and four-byte input uses '?'. */
    *codepoint = '?';
    *text += (input[0] & 0xf8U) == 0xf0U ? 4 : 1;
    return true;
}

bool utf8_font_get_glyph(uint16_t codepoint, uint8_t glyph[32])
{
    if (glyph == NULL) {
        return false;
    }

    size_t low = 0;
    size_t high = sizeof(efontFontList) / sizeof(efontFontList[0]);
    while (low < high) {
        const size_t middle = low + (high - low) / 2U;
        if (efontFontList[middle] < codepoint) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    if (low >= sizeof(efontFontList) / sizeof(efontFontList[0]) ||
        efontFontList[low] != codepoint) {
        memset(glyph, 0, 32);
        return false;
    }

    memcpy(glyph, efontFontData + low * 32U, 32);
    return true;
}
