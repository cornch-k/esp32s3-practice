#ifndef BUTTON_H
#define BUTTON_H

#include <stdbool.h>
#include "esp_err.h"

// Tact switch on a GPIO with the chip's internal pull-up: idle HIGH, pressed LOW.
// State is software-debounced; poll button_is_pressed() from the main loop.

esp_err_t button_init(void);

// Debounced state: true while the button is held down (pin driven LOW).
bool button_is_pressed(void);

#endif // BUTTON_H
