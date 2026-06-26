# ESP32-S3 Button-Triggered STT — Implementation Plan

> **For agentic workers:** Implement task-by-task. Firmware has no host unit-test
> harness, so each task's gate is a successful `idf.py build` (compile/link).
> Final behavioral verification is the on-hardware integration checklist at the end,
> run by the user (per their "write all, then integration test" choice).

**Goal:** Hold a button to record from INMP441, release to POST WAV to a local
Whisper server, show the recognized Korean text on the SSD1683 e-ink display.

**Architecture:** ESP-IDF components (`mic`, `button`, `net`, `stt`) following the
existing `display` component pattern; `main.c` orchestrates a single-task loop.
Recording buffer lives in PSRAM; multipart upload streams to avoid a second copy.

**Tech Stack:** ESP-IDF v6.0.1, C, `i2s_std.h`, `esp_wifi`, `esp_http_client`,
`cJSON`, PSRAM (`MALLOC_CAP_SPIRAM`).

## Global Constraints (verbatim from spec)

- I2S 신 API `driver/i2s_std.h`만 사용. 구 API `driver/i2s.h` 금지.
- 녹음 버퍼는 PSRAM(`heap_caps_malloc`, `MALLOC_CAP_SPIRAM`). 최대 ~15초.
- 샘플레이트 16kHz, 16비트 PCM, mono. INMP441 32비트 슬롯 → 16비트 변환.
- 버튼 GPIO4, 내부 풀업, 평소 HIGH/누르면 LOW, 소프트웨어 디바운싱 20~30ms.
- 디스플레이 핀(7~12)은 기존 `ssd1683.c` 정의 사용, 변경 금지.
- STT 엔드포인트/Wi-Fi 자격은 `#define`, 실제 값은 비우고 주석. `STT_AUTH_TOKEN`은 선택적.
- form 필드 language=ko, 응답 JSON "text" 파싱.
- 각 단계 ESP_LOGI 로그. Wi-Fi/HTTP 실패 등 에러 처리.
- 마이크 검증용: 녹음 PCM 평균 진폭/샘플 일부 시리얼 출력.

## Build gate (every task)

```
export IDF_PATH=/Users/angigyeom/.espressif/v6.0.1/esp-idf
export IDF_TOOLS_PATH=/Users/angigyeom/.espressif
export IDF_PYTHON_ENV_PATH=/Users/angigyeom/.espressif/tools/python/v6.0.1/venv
export IDF_PYTHON_CHECK_CONSTRAINTS=no
source "$IDF_PATH/export.sh"
idf.py build      # Expected: "Project build complete."
```

## File Structure

```
components/button/{button.c, include/button.h, CMakeLists.txt}
components/mic/{mic.c, include/mic.h, CMakeLists.txt}
components/net/{net.c, include/net.h, CMakeLists.txt}
components/stt/{stt.c, include/stt.h, CMakeLists.txt}
main/app_config.h          (new)
main/main.c                (rewritten: orchestration loop)
main/CMakeLists.txt         (add REQUIRES: button mic net stt)
```

---

### Task 1: `app_config.h` (central config)

**Files:** Create `main/app_config.h`.

**Produces:** macros `WIFI_SSID/WIFI_PASSWORD/WIFI_MAX_RETRY`,
`STT_HOST/STT_PORT/STT_PATH/STT_LANGUAGE/STT_MODEL`, optional `STT_AUTH_TOKEN`,
`MIC_SAMPLE_RATE`, `MIC_MAX_SECONDS`. Real secret values left empty with `// TODO`.

- [ ] Write header with all macros (values empty for secrets, defaults for the rest).
- [ ] `idf.py build` (no consumer yet → must still configure/build clean).

### Task 2: `button` component

**Files:** Create `components/button/{button.c, include/button.h, CMakeLists.txt}`.

**Interfaces — Produces:** `esp_err_t button_init(void);` and
`bool button_is_pressed(void);` (debounced; true = pressed = pin LOW).

- [ ] `button.h`: declare the two functions.
- [ ] `button.c`: `PIN_BTN 4`; `gpio_config` input + `GPIO_PULLUP_ONLY`. Debounced
      read: sample level, require it stable for ~25ms before reporting a change.
- [ ] `CMakeLists.txt`: `REQUIRES esp_driver_gpio`.
- [ ] `idf.py build`.

### Task 3: `mic` component (INMP441, i2s_std)

**Files:** Create `components/mic/{mic.c, include/mic.h, CMakeLists.txt}`.

**Interfaces — Consumes:** `MIC_SAMPLE_RATE` (from app_config via main; mic takes it
as `mic_init(uint32_t sample_rate)` to stay decoupled).
**Produces:** `esp_err_t mic_init(uint32_t sample_rate);`,
`esp_err_t mic_read(int16_t *dst, size_t max_samples, size_t *got);` — reads one I2S
chunk, converts 32→16 bit, writes up to `max_samples` into `dst`, sets `*got`.

- [ ] `mic.h`: declare functions; doc the 16-bit/mono contract.
- [ ] `mic.c`: pins `BCLK 15 / WS 16 / DIN 17`; `i2s_std.h` RX std channel, mono LEFT,
      slot width 32-bit, `MIC_SAMPLE_RATE`. Internal `int32_t chunk[256]` →
      `int16_t = raw >> SHIFT` (SHIFT verified against INMP441 datasheet; start 16,
      apply small gain shift if quiet). Helper `mic_rms()` not exposed — RMS debug
      done in main from the captured buffer.
- [ ] `CMakeLists.txt`: `REQUIRES esp_driver_i2s`.
- [ ] `idf.py build`.

### Task 4: `net` component (Wi-Fi STA)

**Files:** Create `components/net/{net.c, include/net.h, CMakeLists.txt}`.

**Interfaces — Consumes:** `WIFI_SSID/WIFI_PASSWORD/WIFI_MAX_RETRY` (passed from main
to `wifi_init_sta(const char*, const char*, int)` to stay decoupled).
**Produces:** `esp_err_t wifi_init_sta(const char *ssid, const char *pass, int max_retry);`
— blocks until got-IP or retries exhausted; returns `ESP_OK` / `ESP_FAIL`.

- [ ] `net.h`: declare `wifi_init_sta`.
- [ ] `net.c`: standard STA bring-up — `esp_netif_init`, default event loop,
      `esp_netif_create_default_wifi_sta`, `esp_wifi_init`, register WIFI_EVENT +
      IP_EVENT handlers, retry on disconnect up to `max_retry`, FreeRTOS event group
      to wait for `WIFI_CONNECTED_BIT`/`WIFI_FAIL_BIT`. Log IP on success.
- [ ] `CMakeLists.txt`: `REQUIRES esp_wifi esp_netif esp_event nvs_flash`.
- [ ] `idf.py build`.

### Task 5: `stt` component (WAV + multipart POST + JSON)

**Files:** Create `components/stt/{stt.c, include/stt.h, CMakeLists.txt}`.

**Interfaces — Consumes:** config via params:
`stt_transcribe(const stt_config_t *cfg, const int16_t *pcm, size_t samples, char *out, size_t out_len)`
where `stt_config_t { host, port, path, language, model, sample_rate, auth_token (nullable) }`.
**Produces:** the above; returns `ESP_OK` and fills `out` (UTF-8) on success.

- [ ] `stt.h`: define `stt_config_t` and declare `stt_transcribe`.
- [ ] `stt.c`:
      - `wav_header(uint8_t hdr[44], uint32_t data_bytes, uint32_t rate)` — canonical
        16-bit mono PCM header.
      - Build multipart preamble string parts (file/language/model) with a fixed
        boundary; compute `Content-Length = preamble + 44 + samples*2 + closing`.
      - `esp_http_client_init` (URL from host/port/path), method POST,
        header `Content-Type: multipart/form-data; boundary=...`; if `auth_token`,
        add `Authorization: Bearer ...`.
      - `esp_http_client_open(client, content_length)` → `esp_http_client_write`
        preamble, WAV header, PCM in chunks (direct from PSRAM), closing boundary.
      - `esp_http_client_fetch_headers`; check status 200; read body into a buffer;
        self-contained `parse_text()` finds `"text"` and decodes JSON escapes
        (incl. `\uXXXX`/surrogates) → copy to `out`. (IDF v6 dropped the core `json`
        component, so no cJSON dependency.) Log status + parsed text.
- [ ] `CMakeLists.txt`: `REQUIRES esp_http_client`.
- [ ] `idf.py build`.

### Task 6: `main.c` orchestration + wiring

**Files:** Rewrite `main/main.c`; modify `main/CMakeLists.txt` (`REQUIRES display
button mic net stt`; keep littlefs image line).

**Interfaces — Consumes:** all component APIs above + display API (`SSD1683_*`,
`EPD_*`, `EPD_Text*`) + `app_config.h`.

- [ ] `main.c`:
      - `nvs_flash_init()` (erase+retry on version mismatch).
      - `wifi_init_sta(WIFI_SSID, WIFI_PASSWORD, WIFI_MAX_RETRY)`; on fail → show
        "Wi-Fi 연결 실패", continue.
      - `mic_init(MIC_SAMPLE_RATE)`, `button_init()`, `SSD1683_Init()`,
        `SSD1683_Clear()`, mount fonts + `EPD_TextInit(...)`.
      - Allocate PSRAM `int16_t *pcm = heap_caps_malloc(MIC_SAMPLE_RATE*MIC_MAX_SECONDS*2, MALLOC_CAP_SPIRAM)`.
      - Helper `show_text(const char*)`: `EPD_Fill(WHITE)` → `EPD_DrawStringWrap` → `EPD_Show`.
      - Show "준비됨 / 버튼을 누르세요".
      - Loop: wait press → record while pressed into `pcm` (cap at max) → on release,
        log sample count + RMS + first few samples → if too short, show msg & continue
        → `stt_transcribe(...)` → show result or error → repeat.
- [ ] `main/CMakeLists.txt`: update REQUIRES.
- [ ] `idf.py build` → "Project build complete."

---

## Self-Review

- **Spec coverage:** mic(T3)/button(T2)/net(T4)/stt+WAV+JSON(T5)/display+loop(T6)/
  config(T1)/PSRAM+RMS debug+logs+error handling(T6) — all mapped.
- **Type consistency:** `mic_read`, `button_is_pressed`, `wifi_init_sta`,
  `stt_transcribe(stt_config_t*)` names used identically in T6 consumption.
- **No placeholders:** secret values are intentional `// TODO` per spec; everything
  else concrete.

## On-Hardware Integration Verification (user runs)

1. Fill `app_config.h`: `WIFI_SSID`, `WIFI_PASSWORD`, `STT_HOST`.
2. `idf.py -p /dev/cu.usbmodem5C4D0367461 app-flash monitor`.
3. Boot log: Wi-Fi got IP; "준비됨" on e-ink.
4. Hold button, speak Korean, release. Monitor shows: sample count, non-trivial RMS
   (mic alive), POST status 200, parsed text. e-ink shows the text.
5. Negative checks: very short tap → "녹음이 너무 짧습니다"; server down → "서버 연결 실패".
