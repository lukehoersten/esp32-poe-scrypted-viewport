#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define JPEG_DECODER_MAX_INPUT_BYTES   (1024 * 1024)        // 1 MB
#define JPEG_DECODER_MAX_OUTPUT_BYTES  (800 * 480 * 3)      // BGR888 panel native

// One-time setup of the ESP32-P4 hardware JPEG decoder. Allocates reusable
// DMA-aligned input + output buffers in PSRAM.
esp_err_t jpeg_decoder_init(void);

// The decoder owns shared scratch buffers. Caller must serialize access.
// http_api uses a try-lock so concurrent /frame POSTs get 503 instead of
// queueing.
bool jpeg_decoder_try_lock(uint32_t timeout_ms);
void jpeg_decoder_unlock(void);

// Pointer to the input scratch buffer (PSRAM, DMA-aligned). Capacity =
// JPEG_DECODER_MAX_INPUT_BYTES. Caller fills before calling decode.
void *jpeg_decoder_input_buffer(void);

// Decode the JPEG sitting in the input buffer into the caller-provided
// out_buf (must be at least out_cap bytes, ≥ 800*480*3 for a full
// panel-sized frame). Bytes land in BGR memory order so the DSI engine
// + TC358762 + Pi panel render channels correctly. Reports image
// width/height in pixels.
//
// In the /frame hot path the caller passes display_back_buffer() so
// the hardware decoder writes pixels straight into the panel's back
// framebuffer with zero intermediate copies.
esp_err_t jpeg_decoder_decode(size_t    jpeg_len,
                              void     *out_buf,
                              size_t    out_cap,
                              uint16_t *out_width,
                              uint16_t *out_height);
