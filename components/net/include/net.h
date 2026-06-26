#ifndef NET_H
#define NET_H

#include "esp_err.h"

// Connect to a Wi-Fi access point in station mode. Blocks until an IP is obtained
// or the retry budget is exhausted. Returns ESP_OK on success, ESP_FAIL otherwise.
// NVS must already be initialized (esp_wifi stores calibration data there).
esp_err_t wifi_init_sta(const char *ssid, const char *password, int max_retry);

#endif // NET_H
