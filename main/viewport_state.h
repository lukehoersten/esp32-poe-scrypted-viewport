#pragma once

#include <stdbool.h>
#include <stdint.h>

#define VIEWPORT_VERSION       "1.4.0"
#define VIEWPORT_PANEL_WIDTH   800
#define VIEWPORT_PANEL_HEIGHT  480

typedef enum {
    VIEWPORT_ORIENTATION_PORTRAIT = 0,  // effective: 480x800
    VIEWPORT_ORIENTATION_LANDSCAPE,     // effective: 800x480
} viewport_orientation_t;

typedef enum {
    VIEWPORT_STATE_ASLEEP = 0,   // boot default; backlight off
    VIEWPORT_STATE_AWAKE,
} viewport_run_state_t;

// Read-mostly snapshot of device state used by /state. Counters are
// updated under a mutex by the modules that own them.
typedef struct {
    char     mac_str[18];               // "aa:bb:cc:dd:ee:ff" — populated at init
    char     viewport_name[64];         // MAC-derived default; can be overridden via /config
    char     scrypted_url[256];         // empty before /config
    bool     configured;
    viewport_run_state_t state;

    uint8_t  brightness;                // 0–100, default 80
    uint32_t idle_timeout_ms;           // 0 = disabled, else >= 5000, default 60000
    viewport_orientation_t orientation; // default portrait

    uint64_t boot_us;                   // esp_timer_get_time() at boot
    int64_t  last_frame_us;             // -1 if no frame received yet

    uint64_t frames_received;
    uint64_t decode_errors;
    uint64_t state_post_failures;
} viewport_state_t;

void viewport_state_init(void);
viewport_state_t *viewport_state_get(void);
void viewport_state_lock(void);
void viewport_state_unlock(void);

// Returns the effective resolution string ("480x800" or "800x480") based
// on the current orientation. Caller-owned static storage; safe to read
// without the lock (atomic snapshot).
const char *viewport_state_resolution_str(void);

// Effective width/height for the current orientation: portrait = 480x800
// (rotated software-side from the panel's 800x480), landscape = 800x480.
// Acquires the state lock internally.
void viewport_state_effective_dims(uint16_t *w, uint16_t *h);
