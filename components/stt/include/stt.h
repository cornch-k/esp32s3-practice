#ifndef STT_H
#define STT_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Configuration for one transcription request. Strings are borrowed (not copied),
// so they must outlive the call. `auth_token` may be NULL to omit the header.
typedef struct {
    const char *host;        // e.g. "192.168.0.42"
    int         port;        // e.g. 12017
    const char *path;        // e.g. "/v1/audio/transcriptions"
    const char *language;    // e.g. "ko"
    const char *model;       // e.g. "whisper-1"
    uint32_t    sample_rate; // PCM sample rate, e.g. 16000
    const char *auth_token;  // bearer token, or NULL for none
} stt_config_t;

// Wrap the 16-bit mono PCM in a WAV container, POST it as multipart/form-data to an
// OpenAI-compatible Whisper endpoint, and copy the recognized "text" field into
// `out` (UTF-8, NUL-terminated, truncated to out_len). Returns ESP_OK on success.
esp_err_t stt_transcribe(const stt_config_t *cfg, const int16_t *pcm, size_t samples,
                         char *out, size_t out_len);

#endif // STT_H
