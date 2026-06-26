#ifndef APP_CONFIG_H
#define APP_CONFIG_H

// Central, user-filled configuration. Secret/site-specific values are intentionally
// left blank — fill them in before flashing. Everything else has a working default.

// ---- Wi-Fi (2.4 GHz) ----
#define WIFI_SSID        ""              // TODO: your Wi-Fi SSID
#define WIFI_PASSWORD    ""              // TODO: your Wi-Fi password
#define WIFI_MAX_RETRY   8

// ---- STT: local Mac mini Whisper server (OpenAI /v1/audio/transcriptions compatible) ----
#define STT_HOST         ""             // TODO: Mac mini LAN IP, e.g. "192.168.0.42"
#define STT_PORT         12017
#define STT_PATH         "/v1/audio/transcriptions"
#define STT_LANGUAGE     "ko"
#define STT_MODEL  "large-v3-turbo-q5_0"
// #define STT_AUTH_TOKEN "sk-..."       // Uncomment to send "Authorization: Bearer ..."

// ---- Audio ----
#define MIC_SAMPLE_RATE  16000          // Hz
#define MIC_MAX_SECONDS  15             // max recording length (sizes the PSRAM buffer)

#endif // APP_CONFIG_H
