#pragma once

#include "esp_err.h"

// Initialize the FT5426 touch controller (I2C 0x38, shared bus with the
// panel MCU) and start the polling/dispatch task. Safe to call before
// the panel is wired — returns an error and leaves the rest of the
// firmware running.
esp_err_t touch_init(void);
