#include "stt.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "stt";

#define BOUNDARY         "----esp32s3sttboundaryZ7Wq3kPmT0aXc"
#define HTTP_TIMEOUT_MS  120000        // model cold-load + transcription can take a while
#define PCM_CHUNK        4096
#define RESP_BUF_SIZE    4096

// Build a canonical 44-byte 16-bit mono PCM WAV header in `h`.
static void wav_header(uint8_t h[44], uint32_t data_bytes, uint32_t rate)
{
    uint32_t byte_rate = rate * 2;            // mono, 16-bit -> 2 bytes/sample
    uint32_t riff_size = 36 + data_bytes;

    memcpy(h, "RIFF", 4);
    h[4] = riff_size & 0xFF;        h[5] = (riff_size >> 8) & 0xFF;
    h[6] = (riff_size >> 16) & 0xFF; h[7] = (riff_size >> 24) & 0xFF;
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    h[16] = 16; h[17] = 0; h[18] = 0; h[19] = 0;   // fmt chunk size = 16
    h[20] = 1;  h[21] = 0;                          // audio format = PCM
    h[22] = 1;  h[23] = 0;                          // channels = 1
    h[24] = rate & 0xFF;        h[25] = (rate >> 8) & 0xFF;
    h[26] = (rate >> 16) & 0xFF; h[27] = (rate >> 24) & 0xFF;
    h[28] = byte_rate & 0xFF;        h[29] = (byte_rate >> 8) & 0xFF;
    h[30] = (byte_rate >> 16) & 0xFF; h[31] = (byte_rate >> 24) & 0xFF;
    h[32] = 2;  h[33] = 0;                          // block align = 2
    h[34] = 16; h[35] = 0;                          // bits per sample = 16
    memcpy(h + 36, "data", 4);
    h[40] = data_bytes & 0xFF;        h[41] = (data_bytes >> 8) & 0xFF;
    h[42] = (data_bytes >> 16) & 0xFF; h[43] = (data_bytes >> 24) & 0xFF;
}

// Read 4 hex digits into a code point.
static unsigned hex4(const char *s)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    }
    return v;
}

// Encode one Unicode code point as UTF-8; returns bytes written (1..4).
static int utf8_encode(unsigned cp, char *dst)
{
    if (cp < 0x80) {
        dst[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    dst[0] = (char)(0xF0 | (cp >> 18));
    dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// Extract the value of the top-level "text" key from a flat JSON object, decoding
// JSON string escapes (incl. \uXXXX and surrogate pairs) into `out`. Raw UTF-8 bytes
// pass through unchanged. Returns ESP_OK if the field is found.
static esp_err_t parse_text(const char *json, char *out, size_t out_len)
{
    const char *p = strstr(json, "\"text\"");
    if (p == NULL) {
        ESP_LOGE(TAG, "response has no \"text\" field");
        return ESP_FAIL;
    }
    p += 6;                                       // past the "text" key
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') {
        ESP_LOGE(TAG, "malformed JSON near \"text\"");
        return ESP_FAIL;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') {
        ESP_LOGE(TAG, "\"text\" value is not a string");
        return ESP_FAIL;
    }
    p++;                                          // first char of the string value

    size_t o = 0;
    while (*p != '\0' && *p != '"' && o + 4 < out_len) {
        if (*p != '\\') {
            out[o++] = *p++;                      // raw byte (incl. multi-byte UTF-8)
            continue;
        }
        p++;                                      // consume the backslash
        switch (*p) {
            case '"':  out[o++] = '"';  p++; break;
            case '\\': out[o++] = '\\'; p++; break;
            case '/':  out[o++] = '/';  p++; break;
            case 'b':  out[o++] = '\b'; p++; break;
            case 'f':  out[o++] = '\f'; p++; break;
            case 'n':  out[o++] = '\n'; p++; break;
            case 'r':  out[o++] = '\r'; p++; break;
            case 't':  out[o++] = '\t'; p++; break;
            case 'u': {
                unsigned cp = hex4(p + 1);
                p += 5;                            // past 'u' + 4 hex digits
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    unsigned lo = hex4(p + 2);
                    p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                o += utf8_encode(cp, out + o);
                break;
            }
            case '\0': break;                      // trailing backslash: stop
            default:   out[o++] = *p++; break;     // unknown escape: keep the char
        }
    }
    out[o] = '\0';
    return ESP_OK;
}

esp_err_t stt_transcribe(const stt_config_t *cfg, const int16_t *pcm, size_t samples,
                         char *out, size_t out_len)
{
    if (cfg == NULL || pcm == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    uint32_t data_bytes = (uint32_t)(samples * sizeof(int16_t));
    uint8_t wav[44];
    wav_header(wav, data_bytes, cfg->sample_rate);

    // ---- multipart preamble: text fields + the file part header ----
    char preamble[640];
    int plen = snprintf(preamble, sizeof(preamble),
        "--" BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n"
        "--" BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"language\"\r\n\r\n%s\r\n"
        "--" BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n"
        "--" BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        cfg->model, cfg->language);
    if (plen < 0 || plen >= (int)sizeof(preamble)) {
        ESP_LOGE(TAG, "preamble overflow");
        return ESP_ERR_INVALID_SIZE;
    }
    static const char closing[] = "\r\n--" BOUNDARY "--\r\n";
    int clen = (int)(sizeof(closing) - 1);

    int content_length = plen + (int)sizeof(wav) + (int)data_bytes + clen;

    char url[160];
    snprintf(url, sizeof(url), "http://%s:%d%s", cfg->host, cfg->port, cfg->path);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type",
                               "multipart/form-data; boundary=" BOUNDARY);
    char authhdr[320];
    if (cfg->auth_token != NULL && cfg->auth_token[0] != '\0') {
        snprintf(authhdr, sizeof(authhdr), "Bearer %s", cfg->auth_token);
        esp_http_client_set_header(client, "Authorization", authhdr);
    }

    ESP_LOGI(TAG, "POST %s  (%d bytes, %u samples)",
             url, content_length, (unsigned)samples);

    esp_err_t err = esp_http_client_open(client, content_length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open: %s (server reachable on the LAN?)", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    // ---- stream the body: preamble, WAV header, PCM (chunked from PSRAM), closing ----
    bool ok = (esp_http_client_write(client, preamble, plen) == plen);
    ok = ok && (esp_http_client_write(client, (const char *)wav, sizeof(wav)) == (int)sizeof(wav));

    const char *body = (const char *)pcm;
    int remaining = (int)data_bytes;
    while (ok && remaining > 0) {
        int chunk = (remaining > PCM_CHUNK) ? PCM_CHUNK : remaining;
        if (esp_http_client_write(client, body, chunk) != chunk) {
            ok = false;
            break;
        }
        body += chunk;
        remaining -= chunk;
    }
    ok = ok && (esp_http_client_write(client, closing, clen) == clen);

    if (!ok) {
        ESP_LOGE(TAG, "body write failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int64_t resp_len = esp_http_client_fetch_headers(client);
    if (resp_len < 0) {
        ESP_LOGE(TAG, "fetch_headers failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    int status = esp_http_client_get_status_code(client);
    bool chunked = esp_http_client_is_chunked_response(client);
    ESP_LOGI(TAG, "HTTP status %d (content-length=%lld, chunked=%d)",
             status, (long long)resp_len, (int)chunked);

    char *resp = malloc(RESP_BUF_SIZE);
    if (resp == NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    int total = 0;
    while (total < RESP_BUF_SIZE - 1) {
        int r = esp_http_client_read(client, resp + total, RESP_BUF_SIZE - 1 - total);
        if (r <= 0) {
            break;
        }
        total += r;
    }
    resp[total] = '\0';
    ESP_LOGI(TAG, "response body (%d bytes): %s", total, resp);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "server returned %d: %.*s", status, total, resp);
        free(resp);
        return ESP_FAIL;
    }

    err = parse_text(resp, out, out_len);
    free(resp);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "text: %s", out);
    }
    return err;
}
