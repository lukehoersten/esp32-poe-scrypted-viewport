#pragma once

#include "esp_err.h"

// BOOT button polling task. Pin is hard-coded in button.c and may need
// to be adjusted for the Waveshare ESP32-P4-ETH (see TODO in button.c).
// Returns OK if the task started; the firmware keeps running either way.
esp_err_t button_init(void);
