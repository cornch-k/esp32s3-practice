#include "epd_text.h"
#include "epd.h"

#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ft2build.h"
#include FT_FREETYPE_H

static const char *TAG = "epd_text";

static FT_Library s_lib;
static FT_Face    s_face;
static uint8_t   *s_font_buf;          // PSRAM; must outlive the FT_Face

static int s_cursor_x, s_cursor_y;     // pen position (top-left of line)
static int s_margin_x;                 // x to return to on '\n'
static int s_baseline;                 // ascender, in pixels
static int s_line_height;              // line advance, in pixels

esp_err_t EPD_TextInit(const char *font_path)
{
    FILE *f = fopen(font_path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot open %s", font_path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return ESP_FAIL;
    }

    s_font_buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (s_font_buf == NULL) {
        ESP_LOGE(TAG, "no PSRAM for %ld-byte font", size);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(s_font_buf, 1, size, f);
    fclose(f);
    if (got != (size_t)size) {
        ESP_LOGE(TAG, "short read (%u/%ld)", (unsigned)got, size);
        return ESP_FAIL;
    }

    if (FT_Init_FreeType(&s_lib)) {
        ESP_LOGE(TAG, "FT_Init_FreeType failed");
        return ESP_FAIL;
    }
    if (FT_New_Memory_Face(s_lib, s_font_buf, size, 0, &s_face)) {
        ESP_LOGE(TAG, "FT_New_Memory_Face failed");
        return ESP_FAIL;
    }

    EPD_SetFontSize(24);
    ESP_LOGI(TAG, "loaded %s (%ld bytes, %ld glyphs)", font_path, size, (long)s_face->num_glyphs);
    return ESP_OK;
}

void EPD_SetFontSize(uint16_t pixel_height)
{
    if (s_face == NULL) {
        return;
    }
    FT_Set_Pixel_Sizes(s_face, 0, pixel_height);
    s_baseline    = s_face->size->metrics.ascender >> 6;   // 26.6 fixed -> pixels
    s_line_height = s_face->size->metrics.height >> 6;
}

void EPD_SetCursor(uint16_t x, uint16_t y)
{
    s_cursor_x = x;
    s_cursor_y = y;
    s_margin_x = x;
}

void EPD_GetCursor(uint16_t *x, uint16_t *y)
{
    if (x) *x = s_cursor_x;
    if (y) *y = s_cursor_y;
}

// Decode one UTF-8 code point and advance *pp. Returns 0 at end of string.
static uint32_t utf8_next(const char **pp)
{
    const unsigned char *s = (const unsigned char *)(*pp);
    if (s[0] == 0) {
        return 0;
    }
    uint32_t cp;
    int n;
    if (s[0] < 0x80)             { cp = s[0];        n = 1; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; n = 2; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; n = 3; }
    else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07; n = 4; }
    else                         { cp = s[0];        n = 1; }   // invalid lead byte
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { n = i; break; }            // truncated sequence
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *pp += n;
    return cp;
}

// Render one glyph at *pen_x / pen_y (top-of-line) and advance *pen_x.
static void render_glyph(uint32_t cp, int *pen_x, int pen_y, uint8_t color)
{
    // Render as 1-bit (hinted) — crisp on a black/white panel, no thresholding.
    if (FT_Load_Char(s_face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO)) {
        return;
    }
    FT_GlyphSlot g = s_face->glyph;
    const FT_Bitmap *bm = &g->bitmap;
    int ox = *pen_x + g->bitmap_left;
    int oy = pen_y + s_baseline - g->bitmap_top;

    for (unsigned row = 0; row < bm->rows; row++) {
        const uint8_t *src = bm->buffer + (size_t)row * bm->pitch;
        for (unsigned col = 0; col < bm->width; col++) {
            if (src[col >> 3] & (0x80 >> (col & 7))) {
                EPD_DrawPixel(ox + col, oy + row, color);
            }
        }
    }
    *pen_x += g->advance.x >> 6;
}

void EPD_DrawString(const char *utf8, uint8_t color)
{
    if (s_face == NULL || utf8 == NULL) {
        return;
    }
    const char *p = utf8;
    uint32_t cp;
    while ((cp = utf8_next(&p)) != 0) {
        if (cp == '\n') {
            s_cursor_x = s_margin_x;
            s_cursor_y += s_line_height;
            continue;
        }
        render_glyph(cp, &s_cursor_x, s_cursor_y, color);
    }
}

// Characters that may break on any boundary (no spaces): Hangul, Kana, CJK ideographs.
static bool is_cjk(uint32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x11FF) ||   // Hangul Jamo
           (cp >= 0x2E80 && cp <= 0x9FFF) ||   // CJK radicals .. Unified Ideographs
           (cp >= 0xAC00 && cp <= 0xD7A3) ||   // Hangul Syllables
           (cp >= 0x3040 && cp <= 0x30FF) ||   // Hiragana / Katakana
           (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK Compatibility Ideographs
           (cp >= 0xFF00 && cp <= 0xFFEF);     // Halfwidth/Fullwidth forms
}

static int glyph_advance(uint32_t cp)
{
    if (FT_Load_Char(s_face, cp, FT_LOAD_DEFAULT)) {
        return 0;
    }
    return s_face->glyph->advance.x >> 6;
}

void EPD_DrawStringWrap(uint16_t x, uint16_t y, uint16_t width, const char *utf8, uint8_t color)
{
    if (s_face == NULL || utf8 == NULL) {
        return;
    }
    const int right = x + width;
    int pen_x = x, pen_y = y;
    const char *p = utf8;

    while (*p) {
        if ((unsigned char)*p == '\n') {
            pen_x = x;
            pen_y += s_line_height;
            p++;
            continue;
        }

        const char *q = p;
        uint32_t cp0 = utf8_next(&q);

        // Spaces: collapse at the start of a line, otherwise just advance the pen.
        if (cp0 == ' ' || cp0 == '\t') {
            if (pen_x > x) {
                pen_x += glyph_advance(cp0);
            }
            p = q;
            continue;
        }

        // Find the next unit and its width: a single CJK char, or a Latin "word".
        const char *end;
        int unit_w = 0;
        if (is_cjk(cp0)) {
            unit_w = glyph_advance(cp0);
            end = q;
        } else {
            const char *rn = p;
            for (;;) {
                const char *before = rn;
                uint32_t c = utf8_next(&rn);
                if (c == 0 || c == ' ' || c == '\t' || c == '\n' || is_cjk(c)) {
                    end = before;
                    break;
                }
                unit_w += glyph_advance(c);
            }
        }

        // Wrap before drawing if the unit won't fit (but never on an empty line).
        if (pen_x + unit_w > right && pen_x > x) {
            pen_x = x;
            pen_y += s_line_height;
        }

        for (const char *d = p; d < end; ) {
            uint32_t c = utf8_next(&d);
            render_glyph(c, &pen_x, pen_y, color);
        }
        p = end;
    }
}

void EPD_DrawStringAt(uint16_t x, uint16_t y, const char *utf8, uint8_t color)
{
    EPD_SetCursor(x, y);
    EPD_DrawString(utf8, color);
}
