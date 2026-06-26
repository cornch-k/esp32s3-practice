#ifndef SSD1683_H
#define SSD1683_H

#include <stdint.h>
#include "esp_err.h"

// Good Display GDEY042T81 (4.2", 400x300, B/W) on the SSD1683 controller.
// This module is the panel hardware layer only: SPI/GPIO + refresh of a frame
// buffer that someone else owns (see epd_canvas). No drawing primitives here.

#define SSD1683_WIDTH     400
#define SSD1683_HEIGHT    300
#define SSD1683_COLS      (SSD1683_WIDTH / 8)               // bytes per row = 50
#define SSD1683_BUF_SIZE  (SSD1683_COLS * SSD1683_HEIGHT)   // 15000 bytes

// Conditioning passes done by SSD1683_Clear(): each cycle = one black + one white
// full refresh. More = darker/cleaner first image, but slower (each refresh ~1.6 s).
#define SSD1683_CLEAR_CYCLES  2

// Bring up SPI + GPIO and run the panel power-on sequence.
esp_err_t SSD1683_Init(void);

// Condition the e-ink (black/white passes) so the next image is solid; leaves white.
esp_err_t SSD1683_Clear(void);

// Full refresh from a SSD1683_BUF_SIZE frame buffer (RAM bit 1 = white, 0 = black).
esp_err_t SSD1683_Refresh(const uint8_t *framebuffer);

// Enter deep sleep. Call SSD1683_Init() again (HW reset) before next use.
esp_err_t SSD1683_Sleep(void);

#endif
