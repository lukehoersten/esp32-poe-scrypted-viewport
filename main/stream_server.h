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
    // Recv-throughput diagnostics. Per-frame samples aggregated over the
    // window. queued_at_body_start = FIONREAD just before the body recv
    // loop runs; if it's close to jpeg_len the wire delivered the whole
    // frame while we were decoding the previous one (we're not the bottleneck).
    // recv_calls = number of recv() syscalls the body read needed (high →
    // many small chunks → window-throttled sender). recv_chunk = body
    // bytes returned per single recv() call within the window.
    uint32_t queued_min, queued_avg, queued_max;            // bytes
    uint32_t recv_calls_min, recv_calls_avg, recv_calls_max; // syscalls per frame body
    uint32_t recv_chunk_min, recv_chunk_avg, recv_chunk_max; // bytes per recv() return
    uint32_t so_rcvbuf;                                      // SO_RCVBUF observed at accept (0 = unknown)
    // Receiver-side skip-oldest. Counts frames where recv-task finished
    // a body but the previous frame was still sitting in the pending
    // slot (decode-task hadn't taken it yet). The older pending frame
    // gets overwritten by the just-received fresher one; the counter
    // captures how often the recv loop outran decode in this window.
    uint32_t recv_dropped_oldest;
    uint32_t decode_idle_min_us, decode_idle_avg_us, decode_idle_max_us;
    // Time decode-task spent waiting on the slot signal between frames.
    uint32_t last_paint_event_us_low;   // last v1 frame's event_us_low,
                                        // 0 if none seen yet on this
                                        // boot or last frame was v0
} stream_server_stats_t;

void stream_server_snapshot_stats(stream_server_stats_t *out);
