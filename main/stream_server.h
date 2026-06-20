#pragma once

#include <stdint.h>
#include "esp_err.h"

// Raw-TCP frame ingestion server. Replaces the per-frame HTTP /frame
// POST loop with a single long-lived TCP connection that streams
// length+seq prefixed JPEGs back-to-back. Eliminates per-frame TCP
// setup, HTTP parsing, and the 200ms Nagle/delayed-ACK deadlocks
// that intermittently spiked the HTTP path to ~250ms wall.
//
// Wire protocol (one connection, repeating; all integers big-endian):
//   [4 bytes: jpeg_len ][4 bytes: seq][jpeg_len bytes: JPEG body]
//
// One client at a time. New client = previous client's seq counter
// is reset (so each stream session starts fresh, and stale frames
// from a previous reconnect can't paint over current ones).
esp_err_t stream_server_start(uint16_t port);
