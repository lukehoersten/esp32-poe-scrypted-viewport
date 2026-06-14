#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Initialize the local-screen renderer. Allocates a PSRAM scratch buffer
// sized for the panel's effective resolution and primes the embedded
// bitmap font.
esp_err_t local_screens_init(void);

// Render the info screen — a multi-line dump of the full GET /config and
// GET /state output, formatted as label/value pairs. Long values (e.g. the
// scrypted URL) are truncated to fit. Font scale is auto-picked.
// Shown on first boot and as a 15s touch-long-press overlay
// (`local_screens_overlay`).
esp_err_t local_screens_show_info(void);

// Render the loading screen — centered "Loading…" — shown on every wake
// until the next /frame arrives.
esp_err_t local_screens_show_loading(void);

// Show the info screen for `duration_ms`, then drop the backlight off.
// Triggered by a ≥1.5s touch long-press in any state.
esp_err_t local_screens_overlay(uint32_t duration_ms);

// True while the info overlay is currently shown (timer armed). Used by
// the touch handler to make a tap dismiss the overlay instead of toggling.
bool local_screens_overlay_active(void);

// Dismiss the overlay now: cancel the timer and drop the backlight.
void local_screens_overlay_dismiss(void);
