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

// Pointer to the built-in input scratch buffer (PSRAM, DMA-aligned).
// Capacity = JPEG_DECODER_MAX_INPUT_BYTES. Owned by the decoder.
// http_api's snapshot path fills this and decodes from it. The stream
// server allocates its own buffers via jpeg_decoder_alloc_input_buffer.
void *jpeg_decoder_input_buffer(void);

// Allocate an additional DMA-aligned PSRAM input buffer suitable for
// jpeg_decoder_decode. Capacity reported via *out_cap. Returned pointer
// is owned by the caller and never freed — call once per buffer at
// init time. NULL on failure.
//
// Used by the stream server to maintain a ping-pong ring of body
// buffers between its recv task and its decode/paint task — recv
// fills one buffer while decode processes another, no memcpy on
// handoff.
void *jpeg_decoder_alloc_input_buffer(size_t *out_cap);

// Decode the JPEG sitting in in_buf into the caller-provided out_buf
// (must be at least out_cap bytes, ≥ 800*480*3 for a full panel-sized
// frame). Bytes land in BGR memory order so the DSI engine + TC358762
// + Pi panel render channels correctly. Reports image width/height
// in pixels.
//
// In the /frame hot path the caller passes display_back_buffer() so
// the hardware decoder writes pixels straight into the panel's back
// framebuffer with zero intermediate copies.
esp_err_t jpeg_decoder_decode(void     *in_buf,
                              size_t    jpeg_len,
                              void     *out_buf,
                              size_t    out_cap,
                              uint16_t *out_width,
                              uint16_t *out_height);
