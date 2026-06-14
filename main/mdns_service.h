#pragma once

#include "esp_err.h"

esp_err_t mdns_service_start(void);

// Update the mDNS hostname and TXT records to reflect the current
// viewport name / orientation. Call after /config writes either field.
esp_err_t mdns_service_refresh(void);
