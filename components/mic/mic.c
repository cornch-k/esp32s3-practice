#include "mic.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "mic";

// ---- Wiring (INMP441) ----
#define PIN_BCLK   15          // SCK  (bit clock)
#define PIN_WS     16          // WS   (word select / LRCLK)
#define PIN_DIN    17          // SD   (serial data out of the mic)

#define MIC_CHUNK_SAMPLES   256            // 32-bit slots pulled per i2s read
#define MIC_READ_TIMEOUT_MS 100
#define MIC_SHIFT           13             // 32-bit slot -> 16-bit PCM; lower = louder
                                           // (>>16 is ~unity; INMP441 is quiet so we boost)

static i2s_chan_handle_t s_rx;
static int32_t s_raw[MIC_CHUNK_SAMPLES];   // raw 32-bit slots straight off the bus

esp_err_t mic_init(uint32_t sample_rate)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;             // deeper DMA buffering to ride out hiccups
    chan_cfg.dma_frame_num = 256;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);   // RX only
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_BCLK,
            .ws   = PIN_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    // INMP441 with L/R tied to GND drives the LEFT slot. (If it ever reads silent,
    // try I2S_STD_SLOT_RIGHT — some boards strap L/R the other way.)
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    if ((err = i2s_channel_init_std_mode(s_rx, &std_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode: %s", esp_err_to_name(err));
        return err;
    }
    if ((err = i2s_channel_enable(s_rx)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "ready (%lu Hz, BCLK=%d WS=%d DIN=%d)",
             (unsigned long)sample_rate, PIN_BCLK, PIN_WS, PIN_DIN);
    return ESP_OK;
}

esp_err_t mic_read(int16_t *dst, size_t max_samples, size_t *got)
{
    *got = 0;
    size_t want = (max_samples < MIC_CHUNK_SAMPLES) ? max_samples : MIC_CHUNK_SAMPLES;
    if (want == 0) {
        return ESP_OK;
    }

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_rx, s_raw, want * sizeof(int32_t),
                                     &bytes_read, pdMS_TO_TICKS(MIC_READ_TIMEOUT_MS));
    // A timeout still hands back whatever partial data was captured; only a hard
    // error aborts. This keeps the audio contiguous instead of dropping chunks.
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        return err;
    }

    size_t n = bytes_read / sizeof(int32_t);
    for (size_t i = 0; i < n; i++) {
        // 24-bit sample is left-justified in the 32-bit slot; shift to 16-bit with
        // extra gain (INMP441 is quiet), clamping so loud peaks don't wrap.
        int32_t s = s_raw[i] >> MIC_SHIFT;
        if (s > INT16_MAX)      s = INT16_MAX;
        else if (s < INT16_MIN) s = INT16_MIN;
        dst[i] = (int16_t)s;
    }
    *got = n;
    return ESP_OK;
}
