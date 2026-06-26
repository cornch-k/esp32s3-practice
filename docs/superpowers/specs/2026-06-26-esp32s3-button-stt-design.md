# 설계: ESP32-S3 버튼 트리거 STT 디바이스 (연습용)

작성일: 2026-06-26

## 목표

버튼을 누르고 있는 동안 INMP441 마이크로 음성을 녹음하고, 버튼을 떼면 로컬
Mac mini의 Whisper(OpenAI API 호환) 서버로 텍스트 변환한 뒤, 그 결과를 SSD1683
e-ink 디스플레이에 한 번에 표시하는 ESP-IDF(C) 펌웨어.

## 개발 환경 / 제약

- 프레임워크: ESP-IDF v6.0.1, C. 타겟: ESP32-S3-N16R8 (Octal PSRAM 탑재).
- I2S는 신 API `driver/i2s_std.h`만 사용. 구 API `driver/i2s.h` 금지.
- 녹음 버퍼는 반드시 PSRAM(`heap_caps_malloc`, `MALLOC_CAP_SPIRAM`).
- 인터넷 불필요. 같은 LAN 안의 IP 직접 통신 전제.

## 하드웨어 핀 배정

| 장치 | 신호 | GPIO | 비고 |
|------|------|------|------|
| INMP441 | SCK(BCLK) | 15 | I2S 비트클럭 |
| INMP441 | WS(LRCLK) | 16 | 워드셀렉트 |
| INMP441 | SD(DATA) | 17 | I2S 데이터 (입력) |
| INMP441 | L/R | GND | 왼쪽 채널 고정 |
| 버튼 | 신호 | 4 | 내부 풀업, 평소 HIGH, 누르면 LOW |
| SSD1683 | BUSY/RST/DC/CS/SCK/MOSI | 7/8/9/10/11/12 | **기존 `ssd1683.c`에 이미 정의·검증됨 (변경 없음)** |

마이크/버튼 핀은 각 컴포넌트 `.c` 상단에 `#define` + 주석으로 둔다(기존 display 패턴).

## 컴포넌트 구조

기존 `components/display/` 패턴(컴포넌트별 `include/` 헤더 + `.c`)을 따른다.

```
components/
  display/   (기존, 변경 없음) ssd1683 / epd / epd_text
  mic/       INMP441 I2S 입력 (i2s_std.h)
  button/    GPIO4 버튼 + 소프트웨어 디바운싱
  net/       Wi-Fi STA 연결
  stt/       WAV 생성 + multipart HTTP POST + JSON("text") 파싱
main/
  main.c       오케스트레이션 (init + 메인 루프)
  app_config.h 내가 채울 #define 모음 (Wi-Fi / STT / 오디오)
```

### 공개 API (개략)

| 컴포넌트 | 헤더 | API | REQUIRES |
|---------|------|-----|----------|
| mic | `mic.h` | `esp_err_t mic_init(void);`<br>`esp_err_t mic_read(int16_t *dst, size_t max, size_t *got);` | `esp_driver_i2s` |
| button | `button.h` | `esp_err_t button_init(void);`<br>`bool button_is_pressed(void);` (디바운스 반영) | `esp_driver_gpio` |
| net | `net.h` | `esp_err_t wifi_init_sta(void);` (재시도 후 결과 반환) | `esp_wifi esp_netif nvs_flash` |
| stt | `stt.h` | `esp_err_t stt_transcribe(const int16_t *pcm, size_t samples, char *out, size_t out_len);` | `esp_http_client json` |

## 설정 (`main/app_config.h`)

내가 직접 채울 값은 비워두고 주석으로 표시. `STT_AUTH_TOKEN`은 `#ifdef`로 선택 적용.

```c
// Wi-Fi
#define WIFI_SSID        ""              // TODO
#define WIFI_PASSWORD    ""              // TODO
#define WIFI_MAX_RETRY   8

// STT (로컬 Mac mini Whisper, OpenAI API 호환)
#define STT_HOST         ""             // TODO 예) "192.168.0.42"
#define STT_PORT         12017
#define STT_PATH         "/v1/audio/transcriptions"
#define STT_LANGUAGE     "ko"
#define STT_MODEL        "whisper-1"
// #define STT_AUTH_TOKEN "sk-..."       // 주석 해제 시 Authorization: Bearer 추가

// 오디오
#define MIC_SAMPLE_RATE  16000
#define MIC_MAX_SECONDS  15
```

## 데이터 흐름 (단일 태스크, app_main)

```
부팅:
  nvs_flash_init → esp_netif/event loop init → wifi_init_sta() (재시도)
  mic_init() → button_init() → SSD1683_Init() + SSD1683_Clear() + EPD_TextInit()
  PSRAM 녹음 버퍼 1회 할당: int16_t[MIC_SAMPLE_RATE * MIC_MAX_SECONDS]
  디스플레이에 "준비됨 / 버튼을 누르세요" 표시

루프:
  1) button_is_pressed()가 true 될 때까지 폴링
  2) 눌린 동안: mic_read()로 PCM을 PSRAM 버퍼에 누적
       - 버퍼 가득 차면 자동 종료
       - 디버그: 주기적으로 평균 진폭(RMS 근사)/샘플 일부 ESP_LOGI
  3) 버튼 떼면 녹음 종료 → 누적 샘플 수 확정
  4) stt_transcribe(pcm, samples, out, len)
       내부: WAV 헤더 → multipart 스트리밍 POST → 응답 JSON "text" 파싱
  5) EPD_Fill(WHITE) → EPD_DrawStringWrap(결과) → EPD_Show()
  6) 루프 처음으로 복귀
```

## 핵심 기술 포인트

### INMP441 32→16비트 변환
- `i2s_std.h`, mono(LEFT slot), 슬롯 비트폭 32비트 수신. 24비트 데이터가 32비트
  슬롯에 MSB 정렬로 들어옴.
- 내부 RAM 청크 버퍼(`int32_t chunk[N]`)로 읽고 → 16비트 추출 → PSRAM에 누적
  (480KB 버퍼를 DMA 대상으로 직접 쓰지 않음).
- 정확한 시프트 양/게인은 **INMP441 데이터시트로 검증** 후 확정(추측 금지).
  기준선: 상위 16비트 추출(`raw >> 16` 대비 게인 보정), 임플 시 데이터시트 대조.

### PSRAM 녹음 버퍼
- 16kHz × 16bit × mono × 15s = 480,000 B = `int16_t[240000]`. 부팅 시 1회 할당·재사용.

### WAV + multipart POST (메모리 효율: 480KB 재복사 회피)
- 44바이트 표준 PCM WAV 헤더(16kHz/16bit/mono) 동적 구성.
- `esp_http_client_open()` → multipart preamble(boundary + file/language/model 파트
  헤더) → WAV 헤더 → PSRAM의 PCM을 청크로 직접 write → 종료 boundary.
- `Content-Length`는 전체 합산. `Content-Type: multipart/form-data; boundary=...`.
- `STT_AUTH_TOKEN` 정의 시에만 `Authorization: Bearer` 추가.

### JSON 파싱
- IDF v6 코어에서 `json`(cJSON) 컴포넌트가 제거됨(컴포넌트 매니저로 이동). 외부
  의존성/네트워크 없이 빌드되도록 `stt.c`에 자체 포함형 추출기를 둠: 응답에서
  `"text"` 값을 찾아 JSON 이스케이프(`\"`,`\\`,`\n`,`\uXXXX`+서러게이트)를 디코딩,
  원시 UTF-8 바이트는 그대로 통과. 평탄한 `{"text": "..."}` 응답에 충분.

## 에러 처리 & 검증

| 상황 | 처리 |
|------|------|
| Wi-Fi 연결 실패(재시도 초과) | ESP_LOGE + "Wi-Fi 연결 실패" 표시, 루프 유지 |
| 녹음 너무 짧음(<0.3초) | 전송 생략 + "녹음이 너무 짧습니다" 표시 |
| HTTP 연결 실패 / 비-200 | 상태코드 로그 + "서버 연결 실패" 표시 |
| JSON에 text 없음/파싱 실패 | "인식 실패" 표시 |
| 메모리 할당 실패 | ESP_LOGE + 안전 종료/표시 |

- **마이크 검증:** 녹음 중 평균 진폭(RMS)·샘플 일부를 시리얼 출력 → 마이크가 실제로
  소리를 잡는지 첫 테스트에서 확인.
- **단계별 로그:** Wi-Fi 연결, 녹음 시작/종료, 샘플 수, POST 시작/응답 코드,
  파싱 결과, 디스플레이 갱신마다 `ESP_LOGI`.

## 빌드 영향

- `nvs`/`phy_init`/`factory(3MB)` 파티션 이미 존재 → Wi-Fi에 충분, 변경 불필요.
- Octal PSRAM, 메인 태스크 스택 32KB 이미 설정됨.
- 새 컴포넌트 CMakeLists 4개 추가, `main/CMakeLists.txt`의 REQUIRES에 신규 컴포넌트 추가.

## 범위 밖 (YAGNI)

- 실시간/스트리밍 STT, 부분 화면 갱신, 재생, NVS에 Wi-Fi 자격 저장, 다국어 토글.
  모두 이번 연습 범위 밖.
