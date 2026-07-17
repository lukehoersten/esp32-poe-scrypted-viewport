#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

// Initialize the panel-side MCU over I2C, then bring up the DSI link in
// video (DPI) mode. Safe to call when the panel is not attached — returns
// an error and lets the rest of the firmware keep running.
esp_err_t display_init(void);

bool      display_is_up(void);

// The Hosyond's I2C bus carries both the panel-MCU (0x45) and the touch
// IC (0x38). Display owns the bus; touch shares it via this getter.
// Returns NULL before display_init() succeeds.
i2c_master_bus_handle_t display_i2c_bus(void);

// 0–100, gamma-corrected to 0–255 on the panel-MCU PWM register.
// brightness=0 is fully off (backlight enable line de-asserted).
esp_err_t display_set_brightness(uint8_t brightness_0_100);

// Sleep / wake hooks for the wake/sleep state machine (M6).
// Sleep cuts backlight; wake restores last brightness.
esp_err_t display_sleep(void);
esp_err_t display_wake(void);

// Blit an RGB565 source image to the panel, applying the current
// orientation. Source dimensions must match the effective resolution:
//   portrait  -> src is 480x800 (rotated 90° CW into the 800x480 panel)
//   landscape -> src is 800x480 (copied 1:1)
// CPU-rotation + format-conversion path used by local_screens for the
// info / loading screens (cold path). /frame uses the zero-copy
// BGR888 path below.
esp_err_t display_present_rgb565(const uint16_t *src,
                                 uint16_t        src_w,
                                 uint16_t        src_h);

// Framebuffer accessors for the zero-memcpy frame path. The DPI panel
// owns three BGR888 framebuffers (triple buffering): one scanning out,
// one pending display at the next frame boundary, one free.
// `display_back_buffer` hands back the free one — guaranteed not to be
// touched by the DSI DMA — so callers (the JPEG decoder) can fill it in
// place with no tear risk and no waiting. When the buffer is ready,
// `display_flip_back_buffer` swaps it in — a cache writeback + index
// swap inside the IDF driver (no memcpy).
//   `out_size` (if non-null) is set to the buffer's byte size, useful
//   for passing to jpeg_decoder_process as its output capacity.
void      *display_back_buffer(size_t *out_size);
esp_err_t  display_flip_back_buffer(void);

// Count of back-buffer picks made while the previously-flipped fb was
// still scanning out — i.e. frames that would have torn under the old
// double-buffer scheme. Monotonic since boot; exposed via /state.
uint32_t   display_tear_guard_engaged(void);
