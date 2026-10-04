#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Decode one UTF-8 character and advance text. Supports the BMP used by efont. */
bool utf8_font_decode(const char **text, uint16_t *codepoint);

/* Look up a 16x16 efont bitmap. Each pair of bytes is one big-endian row. */
bool utf8_font_get_glyph(uint16_t codepoint, uint8_t glyph[32]);
