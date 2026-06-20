#pragma once

#include "esp_err.h"

// Arms a 30 s one-shot timer; on fire, if the running image is in
// PENDING_VERIFY, calls esp_ota_mark_app_valid_cancel_rollback() so the
// bootloader stops considering this image revertible. Safe to call when
// rollback is disabled or when the image is already marked valid — both
// are no-ops. Call once from app_main after http_api_start.
void ota_arm_healthy_timer(void);

// Returns a short tag for the running image's OTA state, suitable for
// /state JSON. Never NULL.
const char *ota_running_state_str(void);
