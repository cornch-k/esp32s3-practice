# esp32s3-practice

ESP32-S3에서 **버튼을 누르는 동안 마이크로 녹음 → Whisper 서버로 음성 인식 → 인식된 한국어 텍스트를 e-paper에 표시**하는 ESP-IDF 연습 프로젝트입니다.

버튼을 누르고 있는 동안 I2S MEMS 마이크로 PCM을 PSRAM에 녹음하고, 버튼을 떼면 16 kHz 오디오를 OpenAI 호환 Whisper 엔드포인트(`/v1/audio/transcriptions`)로 multipart 전송한 뒤, 돌려받은 텍스트를 FreeType(Noto Sans KR)으로 래스터라이즈해 GDEY042T81 4.2" e-paper에 그립니다.

## 하드웨어

| 부품 | 비고 |
|------|------|
| ESP32-S3-N16R8 | 16 MB Flash / 8 MB Octal PSRAM |
| GDEY042T81 4.2" e-paper | SSD1683 컨트롤러, SPI |
| I2S MEMS 마이크 (INMP441 등) | 24/32-bit I2S |
| 푸시 버튼 | GPIO ↔ GND |

### 배선

실크스크린의 숫자가 GPIO 번호입니다.

**e-paper (SPI2_HOST, 2 MHz)** — `components/display/ssd1683.c`

| 신호 | GPIO |
|------|------|
| SCK | 12 |
| MOSI (SDI) | 11 |
| CS | 10 |
| DC | 9 |
| RST | 8 |
| BUSY | 7 |

**마이크 (I2S)** — `components/mic/mic.c`

| 신호 | GPIO |
|------|------|
| BCLK (SCK) | 15 |
| WS (LRCLK) | 16 |
| DIN (SD) | 17 |

**버튼** — `components/button/button.c`: 신호 → GPIO **4**, 반대쪽 다리 → GND.

## 설정

플래시 전에 `main/app_config.h`의 빈 값을 채웁니다 (site/secret 값은 의도적으로 비워둠).

```c
#define WIFI_SSID        ""              // 본인 Wi-Fi SSID (2.4 GHz)
#define WIFI_PASSWORD    ""              // 본인 Wi-Fi 비밀번호

#define STT_HOST         ""             // Whisper 서버 LAN IP, 예) "192.168.0.42"
#define STT_PORT         12017
#define STT_PATH         "/v1/audio/transcriptions"
#define STT_LANGUAGE     "ko"
#define STT_MODEL        "large-v3-turbo-q5_0"
// #define STT_AUTH_TOKEN "sk-..."      // 주석 해제 시 "Authorization: Bearer ..." 헤더 전송
```

`STT_HOST`/`STT_MODEL`은 본인이 띄운 Whisper(OpenAI API 호환) 서버에 맞춰 조정하세요.

## 빌드 & 플래시

ESP-IDF **v6.0.1** (target `esp32s3`)에서 빌드됩니다. 관리 컴포넌트(`espressif/freetype`, `joltwallet/littlefs`)는 빌드 시 자동으로 받아옵니다.

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

`fonts/NotoSansKR-Regular.otf`는 빌드 시 `storage` LittleFS 파티션 이미지로 패킹되어 함께 플래시됩니다(`main/CMakeLists.txt`의 `littlefs_create_partition_image ... FLASH_IN_PROJECT`). 펌웨어는 런타임에 `/fonts/NotoSansKR-Regular.otf`를 읽어 FreeType으로 글리프를 렌더링합니다.

> 참고: N16R8 보드라 `sdkconfig.defaults`에서 16 MB Flash·Octal PSRAM·main task 스택 32 KB를 켭니다 (FreeType 글리프 래스터라이즈가 스택을 많이 씀).

## 파티션

`partitions.csv`:

| 이름 | 타입 | 크기 |
|------|------|------|
| nvs | nvs | 24 KB |
| phy_init | phy | 4 KB |
| factory | app | 3 MB |
| storage | littlefs | 6 MB (폰트 보관) |

## 구조

```
main/            앱 엔트리(녹음→STT→표시 루프) + app_config.h
components/
  button/        버튼 입력
  mic/           I2S 마이크 캡처
  net/           Wi-Fi STA 연결
  stt/           Whisper(OpenAI 호환) multipart 클라이언트 + JSON text 추출
  display/        SSD1683 패널 + 캔버스 + FreeType 텍스트 렌더링
fonts/           NotoSansKR-Regular.otf (LittleFS로 플래시)
docs/            설계 스펙 / 구현 플랜
```

## 라이선스

이 프로젝트의 코드는 [MIT License](LICENSE)로 배포됩니다.

`fonts/NotoSansKR-Regular.otf`는 [SIL Open Font License 1.1](https://openfontlicense.org/) 하에 배포되는 Google Noto 폰트로, MIT가 아닌 OFL을 따릅니다.
