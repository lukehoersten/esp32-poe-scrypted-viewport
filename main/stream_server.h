#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Raw-TCP frame ingestion server. Replaces the per-frame HTTP /frame
// POST loop with a single long-lived TCP connection that streams
// length+seq prefixed JPEGs back-to-back. Eliminates per-frame TCP
// setup, HTTP parsing, and the 200ms Nagle/delayed-ACK deadlocks
// that intermittently spiked the HTTP path to ~250ms wall.
//
// Wire protocol — one connection, repeating; all integers big-endian.
// Two formats are accepted on every connection:
//
//   v1 (16-byte header, current):
//     [u32 magic = 0x56505254 "VPRT"]
//     [u32 jpeg_len]
//     [u32 seq]
//     [u32 event_us_low]    low 32 bits of Scrypted-host monotonic µs
//                           at camera-event arrival; firmware passes
//                           this through verbatim to /state so the
//                           script can compute glass-to-glass.
//     [jpeg_len bytes: JPEG body]
//
//   v0 (8-byte header, legacy):
//     [u32 jpeg_len]
//     [u32 seq]
//     [jpeg_len bytes: JPEG body]
//
// We sniff the first 4 bytes: if they spell "VPRT" the connection is
// v1, otherwise we interpret bytes 0-3 as jpeg_len (v0) and continue.
// The v0 path will be removed once all Scrypted scripts in the field
// have rolled forward.
//
// One client at a time. New client = previous client's seq counter
// is reset (so each stream session starts fresh, and stale frames
// from a previous reconnect can't paint over current ones).
esp_err_t stream_server_start(uint16_t port);

// Snapshot of the most recent 30-frame window plus the latest
// painted frame's pass-through event timestamp. Locking is internal
// to stream_server — the caller just reads. All durations in
// microseconds. Fields are zero before the first window rolls.
typedef struct {
    uint64_t frames;          // count of painted frames in the window
    uint64_t bytes;           // total body bytes received in the window
    uint64_t window_us;       // wall-clock span of the window
    uint64_t window_end_us;   // esp_timer_get_time() at window roll
    uint32_t recv_min_us, recv_avg_us, recv_max_us;
    uint32_t dec_min_us,  dec_avg_us,  dec_max_us;
    uint32_t pnt_min_us,  pnt_avg_us,  pnt_max_us;
    uint32_t idle_min_us, idle_avg_us, idle_max_us;
    uint32_t last_paint_event_us_low;   // last v1 frame's event_us_low,
                                        // 0 if none seen yet on this
                                        // boot or last frame was v0
} stream_server_stats_t;

void stream_server_snapshot_stats(stream_server_stats_t *out);
