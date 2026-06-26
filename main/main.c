#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_littlefs.h"

#include "app_config.h"
#include "ssd1683.h"
#include "epd.h"
#include "epd_text.h"
#include "button.h"
#include "mic.h"
#include "net.h"
#include "stt.h"

static const char *TAG = "app";

#define RECORD_MAX_SAMPLES  ((size_t)MIC_SAMPLE_RATE * MIC_MAX_SECONDS)
#define MIN_RECORD_SAMPLES  (MIC_SAMPLE_RATE / 3)     // ignore < ~0.3 s taps
#define RESULT_TEXT_MAX     1024

static int16_t *s_pcm;                  // PSRAM recording buffer
static char     s_result[RESULT_TEXT_MAX];

static esp_err_t mount_fonts(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/fonts",
        .partition_label = "storage",
        .format_if_mount_failed = false,
        .dont_mount = false,
    };
    return esp_vfs_littlefs_register(&conf);
}

// Clear to white and show one wrapped message (full e-ink refresh, ~1.6 s).
static void show_text(const char *msg)
{
    EPD_Fill(EPD_WHITE);
    EPD_SetFontSize(28);
    EPD_DrawStringWrap(12, 12, SSD1683_WIDTH - 24, msg, EPD_BLACK);
    EPD_Show();
}

// Record into the PSRAM buffer while the button is held; returns sample count.
static size_t record_while_pressed(void)
{
    size_t total = 0;
    while (button_is_pressed() && total < RECORD_MAX_SAMPLES) {
        size_t got = 0;
        esp_err_t err = mic_read(s_pcm + total, RECORD_MAX_SAMPLES - total, &got);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mic_read: %s", esp_err_to_name(err));
            continue;
        }
        total += got;
    }
    return total;
}

// Mic sanity check: average amplitude (RMS), peak, and the first few samples.
static void debug_dump(size_t n)
{
    if (n == 0) {
        ESP_LOGW(TAG, "no samples captured");
        return;
    }
    double sumsq = 0;
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = s_pcm[i];
        sumsq += (double)v * v;
        int32_t a = (v < 0) ? -v : v;
        if (a > peak) {
            peak = a;
        }
    }
    double rms = sqrt(sumsq / (double)n);
    ESP_LOGI(TAG, "captured %u samples (%.2f s), RMS=%.1f peak=%ld",
             (unsigned)n, (double)n / MIC_SAMPLE_RATE, rms, (long)peak);
    ESP_LOGI(TAG, "first samples: %d %d %d %d %d %d %d %d",
             s_pcm[0], s_pcm[1], s_pcm[2], s_pcm[3],
             (n > 4) ? s_pcm[4] : 0, (n > 5) ? s_pcm[5] : 0,
             (n > 6) ? s_pcm[6] : 0, (n > 7) ? s_pcm[7] : 0);
}

void app_main(void)
{
    // ---- NVS (required by Wi-Fi) ----
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);

    // ---- Fonts + display ----
    ESP_ERROR_CHECK(mount_fonts());
    ESP_ERROR_CHECK(EPD_TextInit("/fonts/NotoSansKR-Regular.otf"));
    ESP_ERROR_CHECK(SSD1683_Init());
    ESP_ERROR_CHECK(SSD1683_Clear());      // condition the panel so blacks come out solid

    // ---- Peripherals ----
    ESP_ERROR_CHECK(mic_init(MIC_SAMPLE_RATE));
    ESP_ERROR_CHECK(button_init());

    // ---- PSRAM recording buffer ----
    s_pcm = heap_caps_malloc(RECORD_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (s_pcm == NULL) {
        ESP_LOGE(TAG, "failed to allocate %u-byte PSRAM record buffer",
                 (unsigned)(RECORD_MAX_SAMPLES * sizeof(int16_t)));
        show_text("메모리 할당 실패");
        return;
    }
    ESP_LOGI(TAG, "record buffer: %u bytes in PSRAM (%d s max)",
             (unsigned)(RECORD_MAX_SAMPLES * sizeof(int16_t)), MIC_MAX_SECONDS);

    // ---- Wi-Fi ----
    bool wifi_ok = (wifi_init_sta(WIFI_SSID, WIFI_PASSWORD, WIFI_MAX_RETRY) == ESP_OK);
    if (wifi_ok) {
        show_text("준비됨\n버튼을 누르고 말하세요");
    } else {
        show_text("Wi-Fi 연결 실패\n설정을 확인하세요");
    }

    const stt_config_t stt_cfg = {
        .host = STT_HOST,
        .port = STT_PORT,
        .path = STT_PATH,
        .language = STT_LANGUAGE,
        .model = STT_MODEL,
        .sample_rate = MIC_SAMPLE_RATE,
#ifdef STT_AUTH_TOKEN
        .auth_token = STT_AUTH_TOKEN,
#else
        .auth_token = NULL,
#endif
    };

    ESP_LOGI(TAG, "ready; waiting for button");
    while (1) {
        if (!button_is_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        ESP_LOGI(TAG, "recording...");
        size_t n = record_while_pressed();
        ESP_LOGI(TAG, "recording stopped");
        debug_dump(n);

        if (n < MIN_RECORD_SAMPLES) {
            show_text("녹음이 너무 짧습니다");
            continue;
        }
        if (!wifi_ok) {
            show_text("Wi-Fi 미연결\n전송할 수 없습니다");
            continue;
        }

        show_text("인식 중...");
        esp_err_t err = stt_transcribe(&stt_cfg, s_pcm, n, s_result, sizeof(s_result));
        if (err != ESP_OK) {
            show_text("인식 실패\n서버를 확인하세요");
            continue;
        }
        if (s_result[0] == '\0') {
            show_text("(인식된 텍스트 없음)");
        } else {
            show_text(s_result);
        }
    }
}
