#include "epd.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

// 1 bit/pixel, MSB = leftmost. Panel RAM convention: bit 1 = white, bit 0 = black.
static uint8_t s_buffer[SSD1683_HEIGHT][SSD1683_COLS];

const uint8_t *EPD_Buffer(void)
{
    return (const uint8_t *)s_buffer;
}

esp_err_t EPD_Show(void)
{
    return SSD1683_Refresh((const uint8_t *)s_buffer);
}

void EPD_Fill(uint8_t color)
{
    // BLACK -> all RAM bits 0; WHITE -> all RAM bits 1.
    memset(s_buffer, color ? 0x00 : 0xFF, sizeof(s_buffer));
}

void EPD_DrawPixel(uint16_t x, uint16_t y, uint8_t color)
{
    if (x >= SSD1683_WIDTH || y >= SSD1683_HEIGHT) {
        return;
    }
    uint8_t mask = 0x80 >> (x & 7);
    if (color) {                                 // BLACK -> clear bit
        s_buffer[y][x >> 3] &= ~mask;
    } else {                                     // WHITE -> set bit
        s_buffer[y][x >> 3] |= mask;
    }
}

void EPD_DrawHLine(uint16_t x, uint16_t y, uint16_t width, uint8_t color)
{
    for (uint16_t i = 0; i < width; i++) {
        EPD_DrawPixel(x + i, y, color);
    }
}

void EPD_DrawVLine(uint16_t x, uint16_t y, uint16_t height, uint8_t color)
{
    for (uint16_t i = 0; i < height; i++) {
        EPD_DrawPixel(x, y + i, color);
    }
}

void EPD_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint8_t color)
{
    int dx = abs((int)x1 - (int)x0);
    int dy = -abs((int)y1 - (int)y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    int x = x0, y = y0;

    while (1) {
        EPD_DrawPixel(x, y, color);
        if (x == x1 && y == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
}

void EPD_DrawRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t color)
{
    EPD_DrawHLine(x, y, width, color);
    EPD_DrawHLine(x, y + height - 1, width, color);
    EPD_DrawVLine(x, y, height, color);
    EPD_DrawVLine(x + width - 1, y, height, color);
}

void EPD_DrawFilledRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t color)
{
    for (uint16_t i = 0; i < height; i++) {
        EPD_DrawHLine(x, y + i, width, color);
    }
}

// ---- Anti-aliased line: Xiaolin Wu coverage, ordered-dithered to 1 bit ----

static const uint8_t kBayer4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

static inline float epd_fpart(float x)  { return x - floorf(x); }
static inline float epd_rfpart(float x) { return 1.0f - epd_fpart(x); }

static void epd_plot_aa(int x, int y, float coverage, uint8_t color)
{
    if (x < 0 || y < 0 || x >= SSD1683_WIDTH || y >= SSD1683_HEIGHT) {
        return;
    }
    float threshold = (kBayer4[y & 3][x & 3] + 0.5f) / 16.0f;
    if (coverage > threshold) {
        EPD_DrawPixel(x, y, color);
    }
}

void EPD_DrawLineAA(uint16_t x0i, uint16_t y0i, uint16_t x1i, uint16_t y1i, uint8_t color)
{
    float x0 = x0i, y0 = y0i, x1 = x1i, y1 = y1i;

    bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);
    if (steep) {
        float t;
        t = x0; x0 = y0; y0 = t;
        t = x1; x1 = y1; y1 = t;
    }
    if (x0 > x1) {
        float t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }

    float dx = x1 - x0;
    float dy = y1 - y0;
    float gradient = (dx == 0.0f) ? 1.0f : dy / dx;

    float xend = floorf(x0 + 0.5f);
    float yend = y0 + gradient * (xend - x0);
    float xgap = epd_rfpart(x0 + 0.5f);
    int xpxl1 = (int)xend;
    int ypxl1 = (int)floorf(yend);
    if (steep) {
        epd_plot_aa(ypxl1,     xpxl1, epd_rfpart(yend) * xgap, color);
        epd_plot_aa(ypxl1 + 1, xpxl1, epd_fpart(yend)  * xgap, color);
    } else {
        epd_plot_aa(xpxl1, ypxl1,     epd_rfpart(yend) * xgap, color);
        epd_plot_aa(xpxl1, ypxl1 + 1, epd_fpart(yend)  * xgap, color);
    }
    float intery = yend + gradient;

    xend = floorf(x1 + 0.5f);
    yend = y1 + gradient * (xend - x1);
    xgap = epd_fpart(x1 + 0.5f);
    int xpxl2 = (int)xend;
    int ypxl2 = (int)floorf(yend);
    if (steep) {
        epd_plot_aa(ypxl2,     xpxl2, epd_rfpart(yend) * xgap, color);
        epd_plot_aa(ypxl2 + 1, xpxl2, epd_fpart(yend)  * xgap, color);
    } else {
        epd_plot_aa(xpxl2, ypxl2,     epd_rfpart(yend) * xgap, color);
        epd_plot_aa(xpxl2, ypxl2 + 1, epd_fpart(yend)  * xgap, color);
    }

    for (int x = xpxl1 + 1; x < xpxl2; x++) {
        int iy = (int)floorf(intery);
        if (steep) {
            epd_plot_aa(iy,     x, epd_rfpart(intery), color);
            epd_plot_aa(iy + 1, x, epd_fpart(intery),  color);
        } else {
            epd_plot_aa(x, iy,     epd_rfpart(intery), color);
            epd_plot_aa(x, iy + 1, epd_fpart(intery),  color);
        }
        intery += gradient;
    }
}
