#pragma once

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

// Repaint the current best screen for `state`. Used after the BOOT-button
// 15-second overlay expires.
//   UNCONFIGURED -> IP screen
//   ASLEEP       -> (caller handles backlight; this paints a black FB)
//   AWAKE        -> black FB; Scrypted's next /frame restores live content
esp_err_t local_screens_restore_for_state(void);
