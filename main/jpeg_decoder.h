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

// Decode the JPEG sitting in the input buffer. Fills out_bgr888 with a
// pointer to the decoded 24-bit image (3 bytes/pixel, BGR memory order so
// the DSI engine + TC358762 + Pi panel render channels correctly) and
// reports the image's width/height in pixels. The output buffer is owned
// by the decoder — valid only until the next jpeg_decoder_unlock().
esp_err_t jpeg_decoder_decode(size_t   jpeg_len,
                              void   **out_bgr888,
                              uint16_t *out_width,
                              uint16_t *out_height);
