#ifndef EPD_H
#define EPD_H

#include <stdint.h>
#include "esp_err.h"
#include "ssd1683.h"     // panel geometry + SSD1683_Refresh

// Graphics layer: owns the frame buffer and draws into it. Nothing here talks to
// SPI; call EPD_Show() to push the buffer to the panel.

// Colors: foreground ink is black, background is white.
#define EPD_WHITE   0
#define EPD_BLACK   1

void EPD_Fill(uint8_t color);
void EPD_DrawPixel(uint16_t x, uint16_t y, uint8_t color);
void EPD_DrawHLine(uint16_t x, uint16_t y, uint16_t width, uint8_t color);
void EPD_DrawVLine(uint16_t x, uint16_t y, uint16_t height, uint8_t color);
void EPD_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint8_t color);
// Anti-aliased line (Xiaolin Wu) dithered to 1-bit. Draw onto a white background.
void EPD_DrawLineAA(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint8_t color);
void EPD_DrawRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t color);
void EPD_DrawFilledRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t color);

// Raw 1-bpp buffer (SSD1683_BUF_SIZE bytes), for code that needs direct access.
const uint8_t *EPD_Buffer(void);

// Push the current frame buffer to the panel (full refresh).
esp_err_t EPD_Show(void);

#endif
