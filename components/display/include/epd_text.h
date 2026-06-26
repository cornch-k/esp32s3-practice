#ifndef EPD_TEXT_H
#define EPD_TEXT_H

#include <stdint.h>
#include "esp_err.h"

// Text layer: renders UTF-8 strings with a TTF/OTF font (via FreeType) straight onto
// the canvas (epd.h). Any glyph present in the loaded font works — ASCII or CJK.

// Load a TTF/OTF font from a mounted path, e.g. "/fonts/NotoSansKR-Regular.otf".
esp_err_t EPD_TextInit(const char *font_path);

// Pixel height for following text (re-applied per call is fine).
void EPD_SetFontSize(uint16_t pixel_height);

// Cursor in pixels; (x, y) is the top-left of the text line.
void EPD_SetCursor(uint16_t x, uint16_t y);
void EPD_GetCursor(uint16_t *x, uint16_t *y);

// Draw a UTF-8 string at the cursor (handles '\n'); color is EPD_BLACK / EPD_WHITE.
void EPD_DrawString(const char *utf8, uint8_t color);
void EPD_DrawStringAt(uint16_t x, uint16_t y, const char *utf8, uint8_t color);

// Draw UTF-8 text from (x, y), word-wrapping to stay within `width` pixels.
// Latin wraps at spaces, CJK wraps between characters; also honors '\n'.
void EPD_DrawStringWrap(uint16_t x, uint16_t y, uint16_t width, const char *utf8, uint8_t color);

#endif
