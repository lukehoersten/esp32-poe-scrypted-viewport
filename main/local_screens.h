#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Initialize the local-screen renderer. Allocates a PSRAM scratch buffer
// sized for the panel's effective resolution and primes the embedded
// bitmap font.
esp_err_t local_screens_init(void);

// Render the identity / "who am I" screen — four centered lines:
//   <viewport name>            ("viewport" if unconfigured)
//   viewport-<name>.local       (mDNS hostname)
//   <current IP>                ("no network" if no DHCP lease)
//   <state>                     ("awake" / "asleep" / "unconfigured")
// Font scale is auto-picked to fit the longest line within 90% of width.
// Shown on first boot, after factory reset, and as a 15s BOOT-button overlay.
esp_err_t local_screens_show_ip(void);

// Render the loading screen — centered "Loading…" — shown on every wake
// until the next /frame arrives.
esp_err_t local_screens_show_loading(void);

// Show the identity ("who am I") screen for `duration_ms`, then drop the
// backlight off. Used by the BOOT button short-press (any state) and by
// the touch handler when the device is unconfigured.
esp_err_t local_screens_overlay(uint32_t duration_ms);

// True while the identity overlay is currently shown (timer armed).
// Used by the touch handler to make a second tap dismiss the overlay
// instead of re-arming it.
bool local_screens_overlay_active(void);

// Dismiss the overlay now: cancel the timer and drop the backlight.
void local_screens_overlay_dismiss(void);
