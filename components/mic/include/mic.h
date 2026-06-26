#ifndef MIC_H
#define MIC_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// INMP441 I2S MEMS microphone via the new i2s_std driver. The mic is read as mono
// (LEFT slot, L/R tied to GND); its 24-bit samples arrive left-justified in 32-bit
// slots and are converted to 16-bit signed PCM here.

esp_err_t mic_init(uint32_t sample_rate);

// Read one batch from the I2S RX channel. Writes up to `max_samples` 16-bit PCM
// samples into `dst` and sets *got to how many were produced. Blocks until a chunk
// is available or the internal timeout elapses (then returns ESP_OK with *got small).
esp_err_t mic_read(int16_t *dst, size_t max_samples, size_t *got);

#endif // MIC_H
