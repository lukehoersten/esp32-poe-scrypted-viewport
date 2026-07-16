#pragma once

#include "esp_err.h"

// On-die temperature sensor (ESP32-P4 TSENS). Init once at boot; reads are
// cheap (~µs) and safe from any task. Note this is junction temperature —
// expect ~10-20°C above ambient under load; useful for trending and
// thermal-throttle debugging, not room temperature.
esp_err_t chip_temp_init(void);

// Degrees Celsius, or NAN if the sensor isn't initialized / read failed.
float chip_temp_read(void);
