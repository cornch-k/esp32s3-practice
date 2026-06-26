#include "button.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "button";

// ---- Wiring ----
#define PIN_BTN       4              // signal -> GPIO4, other leg -> GND
#define DEBOUNCE_US   25000          // 25 ms must be stable before a state change commits

static bool    s_stable_pressed;     // debounced state (true = pressed)
static int     s_last_raw;           // last raw level (1 = released, 0 = pressed)
static int64_t s_last_change_us;     // when the raw level last changed

esp_err_t button_init(void)
{
    gpio_config_t cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_BTN),
        .pull_up_en = GPIO_PULLUP_ENABLE,        // idle HIGH, pressed pulls to GND
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(err));
        return err;
    }
    s_last_raw = gpio_get_level(PIN_BTN);
    s_stable_pressed = (s_last_raw == 0);
    s_last_change_us = esp_timer_get_time();
    ESP_LOGI(TAG, "ready (GPIO%d, internal pull-up, active-low)", PIN_BTN);
    return ESP_OK;
}

bool button_is_pressed(void)
{
    int raw = gpio_get_level(PIN_BTN);
    int64_t now = esp_timer_get_time();

    if (raw != s_last_raw) {
        s_last_raw = raw;                        // bouncing: restart the stability window
        s_last_change_us = now;
    } else if (now - s_last_change_us >= DEBOUNCE_US) {
        s_stable_pressed = (raw == 0);           // stable long enough: commit the state
    }
    return s_stable_pressed;
}
