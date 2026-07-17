#pragma once

#include "esp_err.h"

// Read the persisted config into viewport_state. Safe to call on a fresh
// device: missing keys keep their first-boot defaults from viewport_state_init.
// Recomputes `configured` (true iff a scrypted URL is present); run state
// is untouched (stays at the boot default).
esp_err_t nvs_config_load(void);

// Persist the current viewport_state to NVS atomically. The caller is expected
// to have already mutated viewport_state under viewport_state_lock().
esp_err_t nvs_config_save(void);
