#include "ssd1683.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "ssd1683";

// ---- Wiring (ESP32-S3-N16R8; silkscreen numbers are GPIO numbers) ----
#define PIN_SCK    12
#define PIN_MOSI   11          // adapter SDI
#define PIN_CS     10
#define PIN_DC      9
#define PIN_RST     8
#define PIN_BUSY    7
#define SPI_HOST_ID SPI2_HOST
#define SPI_HZ     (2 * 1000 * 1000)    // 2 MHz is reliable over jumper wires

#define EPD_CHUNK            4096
#define EPD_BUSY_TIMEOUT_MS  10000

static spi_device_handle_t s_spi;
static uint8_t *s_dma_buf;       // DMA-capable scratch buffer
static bool s_inited;

// Called by the SPI driver before each transfer: drive D/C from t->user.
static void IRAM_ATTR epd_spi_pre_cb(spi_transaction_t *t)
{
    gpio_set_level(PIN_DC, (int)(intptr_t)t->user);
}

// dc = 0 -> command, dc = 1 -> data. Payloads <=4 B ride in tx_data (no DMA buffer
// needed); larger transfers must come from the DMA-capable scratch buffer.
static esp_err_t epd_send(const uint8_t *data, size_t len, int dc)
{
    if (len == 0) {
        return ESP_OK;
    }
    spi_transaction_t t = {
        .length = len * 8,
        .user = (void *)(intptr_t)dc,
    };
    if (len <= 4) {
        t.flags = SPI_TRANS_USE_TXDATA;
        memcpy(t.tx_data, data, len);
    } else {
        t.tx_buffer = data;
    }
    return spi_device_polling_transmit(s_spi, &t);
}

static inline esp_err_t epd_cmd(uint8_t cmd)     { return epd_send(&cmd, 1, 0); }
static inline esp_err_t epd_data1(uint8_t value) { return epd_send(&value, 1, 1); }

static esp_err_t epd_wait_busy(void)
{
    int waited = 0;
    while (gpio_get_level(PIN_BUSY)) {           // BUSY high = busy
        vTaskDelay(pdMS_TO_TICKS(10));
        if ((waited += 10) >= EPD_BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY stuck high (timeout)");
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

static void epd_reset(void)
{
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 0);                  // active low
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

// Stream a full frame into a RAM bank. If src is NULL the bank is filled with `fill`.
static esp_err_t epd_write_ram(uint8_t cmd, const uint8_t *src, uint8_t fill)
{
    esp_err_t err = epd_cmd(cmd);
    if (err != ESP_OK) {
        return err;
    }
    if (src == NULL) {
        memset(s_dma_buf, fill, EPD_CHUNK);
    }
    for (size_t off = 0; off < SSD1683_BUF_SIZE; off += EPD_CHUNK) {
        size_t n = SSD1683_BUF_SIZE - off;
        if (n > EPD_CHUNK) {
            n = EPD_CHUNK;
        }
        if (src != NULL) {
            memcpy(s_dma_buf, src + off, n);
        }
        if ((err = epd_send(s_dma_buf, n, 1)) != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t epd_update_full(void)
{
    epd_cmd(0x22);             // Display Update Control 2
    epd_data1(0xFF);           // 0xFF = black/white mode (temp + B/W LUT + display).
                               // 0xF7 would be 3-color mode and would read the RED RAM.
    epd_cmd(0x20);             // Master Activation
    return epd_wait_busy();
}

// Drive the whole panel to one solid level and full-refresh (0xFF = white, 0x00 = black).
static esp_err_t epd_refresh_solid(uint8_t bits)
{
    esp_err_t err = epd_write_ram(0x24, NULL, bits);
    if (err != ESP_OK) {
        return err;
    }
    return epd_update_full();
}

// SSD1683 / GDEY042T81 power-on init (LUT from OTP, internal temperature sensor).
static esp_err_t epd_panel_init(void)
{
    epd_reset();
    esp_err_t err = epd_wait_busy();
    if (err != ESP_OK) {
        return err;
    }

    epd_cmd(0x12);             // SWRESET
    if ((err = epd_wait_busy()) != ESP_OK) {
        return err;
    }

    epd_cmd(0x01);             // Driver output control: (height-1) gate lines
    epd_data1((SSD1683_HEIGHT - 1) & 0xFF);
    epd_data1(((SSD1683_HEIGHT - 1) >> 8) & 0xFF);
    epd_data1(0x00);

    epd_cmd(0x11);             // Data entry mode: X+, Y+
    epd_data1(0x03);

    epd_cmd(0x44);             // RAM X window (byte units)
    epd_data1(0x00);
    epd_data1(SSD1683_COLS - 1);

    epd_cmd(0x45);             // RAM Y window (line units)
    epd_data1(0x00);
    epd_data1(0x00);
    epd_data1((SSD1683_HEIGHT - 1) & 0xFF);
    epd_data1(((SSD1683_HEIGHT - 1) >> 8) & 0xFF);

    epd_cmd(0x3C);             // Border waveform
    epd_data1(0x05);

    epd_cmd(0x18);             // Temperature sensor: internal
    epd_data1(0x80);

    epd_cmd(0x21);             // Display update control 1: BW/Red normal, single chip
    epd_data1(0x00);
    epd_data1(0x00);

    epd_cmd(0x4E);             // RAM X address counter = 0
    epd_data1(0x00);
    epd_cmd(0x4F);             // RAM Y address counter = 0
    epd_data1(0x00);
    epd_data1(0x00);

    if ((err = epd_wait_busy()) != ESP_OK) {
        return err;
    }

    // B/W panel: only the BW RAM (0x24) is used. Clear the RED RAM (0x26) once so its
    // power-on garbage can never bleed into the image as noise.
    return epd_write_ram(0x26, NULL, 0x00);
}

esp_err_t SSD1683_Init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    gpio_config_t out = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RST),
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    gpio_config_t in = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_BUSY),
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,                       // write-only panel
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_CHUNK,
    };
    esp_err_t err = spi_bus_initialize(SPI_HOST_ID, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_HZ,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 1,
        .pre_cb = epd_spi_pre_cb,
    };
    if ((err = spi_bus_add_device(SPI_HOST_ID, &devcfg, &s_spi)) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(err));
        spi_bus_free(SPI_HOST_ID);
        return err;
    }

    s_dma_buf = heap_caps_malloc(EPD_CHUNK, MALLOC_CAP_DMA);
    if (s_dma_buf == NULL) {
        spi_bus_remove_device(s_spi);
        spi_bus_free(SPI_HOST_ID);
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    if ((err = epd_panel_init()) != ESP_OK) {
        s_inited = false;
        return err;
    }
    ESP_LOGI(TAG, "ready (%dx%d)", SSD1683_WIDTH, SSD1683_HEIGHT);
    return ESP_OK;
}

esp_err_t SSD1683_Clear(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    // Flash black<->white a few times so the e-ink fully polarizes; otherwise the first
    // image after power-on/sleep comes out gray. Leaves the panel white.
    for (int i = 0; i < SSD1683_CLEAR_CYCLES; i++) {
        esp_err_t err = epd_refresh_solid(0x00);            // all black
        if (err != ESP_OK) {
            return err;
        }
        if ((err = epd_refresh_solid(0xFF)) != ESP_OK) {    // all white
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t SSD1683_Refresh(const uint8_t *framebuffer)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (framebuffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = epd_write_ram(0x24, framebuffer, 0xFF);
    if (err != ESP_OK) {
        return err;
    }
    return epd_update_full();
}

esp_err_t SSD1683_Sleep(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    epd_cmd(0x10);             // Deep sleep mode
    epd_data1(0x01);
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}
