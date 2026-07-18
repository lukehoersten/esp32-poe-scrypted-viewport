# Testing & Verification

All bring-up milestones (M1–M9: Ethernet/DHCP, HTTP + mDNS, display, config
persistence, frame push, state + idle timer, touch + outbound POSTs, local
screens, live TCP stream) are ✅ **verified on hardware**. The
milestone-by-milestone log — acceptance runs, dates, and the fix notes from
each bring-up session — lived in this file through v1.4.0; see git history
(`git log --follow TESTING.md`, last full version at `8eebc0e`) if you need
it.

What remains here is the forward-looking material:

1. **New-unit bring-up** — provisioning another viewport from parts.
2. **Regression recipes** — quick `curl` checks for the API surface.
3. **Outstanding tests** — what has never been run.
4. **Performance review playbook** — the iterate-and-measure loop for
   data-plane changes.

> **Iterating on an already-flashed device:** `make ota [VIEWPORT=<host>]`
> builds with a fresh git stamp, pushes over the LAN, and verifies the
> `pending-verify -> valid` OTA sequence (auto-retrying the known
> first-push silent rollback). USB is for first flash only.

---

## New-unit bring-up

### Hardware prerequisites

- **Waveshare ESP32-P4-ETH** — check the silkscreen for flash size;
  `sdkconfig.defaults` declares 16 MB (a 32 MB SKU works but wastes half).
- **Hosyond 5" 800×480 Pi-architecture panel** — TC358762 bridge + ATTINY
  MCU at I²C `0x45`, FT5426 touch at `0x38`.
- **15→22-pin DSI adapter cable** — the panel's 15-pin Pi FPC into the
  board's 22-pin (Pi 5 / CM4 style) DSI connector. Everything goes through
  the FPC: DSI lanes, panel power, and the I²C bus (board side: SDA=GPIO 7,
  SCL=GPIO 8 — already what `display.c` uses). No jumper wires.
- Ethernet to a DHCP-serving switch; USB for the first flash; PoE optional.
- There is **no usable user button** (GPIO 35 is owned by EMAC TXD1);
  everything is touch — no code or wiring expectations around BOOT.

### Sequence

1. **Board only** (no panel). First flash over USB:

   ```sh
   source ~/Dev/code/git/esp32/env.sh
   idf.py set-target esp32p4
   idf.py build
   idf.py -p /dev/cu.usbmodem* flash monitor
   ```

   Expect `net_eth: got ip <addr>` within ~10 s of link-up. Then from
   another host: `curl http://<ip>/state | jq .` and
   `dns-sd -B _scrypted-viewport._tcp local.` (the TXT should carry
   `version=`, `resolution=`, `orientation=`, `name=<mac-derived>`,
   `mac=`). A bare board with no panel/Ethernet still boots — the flag
   summary logs degraded subsystems in lowercase instead of panicking.

2. **Attach the panel** (power off first). Pi FPCs install upside-down
   easily — contact side faces the connector lock, both ends. On boot
   expect `panel MCU id 0xc3 — Pi v1.1 architecture ack'd`,
   `TC358762 bridge configured`, `DSI up: 800x480 26 MHz, 1-lane 600 Mbps,
   non-burst`, and the info screen after a brief boot flash (the FT5426
   needs one lit scan cycle before it reports touches).

3. **Touch**: short tap toggles wake/sleep; ≥1.5 s hold shows the info
   overlay for 15 s.

4. **Scrypted**: in the Scrypted Viewport script, "+ Add Device" — the new
   unit appears in the host dropdown (name auto-fills from mDNS). Pick a
   camera and triggers, save, tap the panel or ring the doorbell → live
   video. See [`scrypted/README.md`](scrypted/README.md).

### Failure-mode signals (panel bring-up)

- `panel MCU @0x45 unreachable` → FPC seating/orientation (most common),
  an adapter cable that doesn't pass I²C through, or the rare panel
  variant with GT911 touch at `0x5D`/`0x14` instead of the Pi 7"
  architecture this driver targets (that variant has no ATTINY at all).
  Don't suspect DSI yet.
- I²C acks but no image → DSI cable orientation on the 22-pin side.
- Image but wrong colors → BGR/RGB element order (see `jpeg_decoder.c` /
  `display.c` notes — the pipeline wants `[B, G, R]` in memory).
- Image with sync artifacts → `RPI_*` timing constants in `display.c`
  (canonical Pi 7" values are the defaults).
- Touch acks at `0x38` but no taps → the panel hasn't had a lit scan
  cycle yet (handled by the boot wake-flash) or reset-line state.

---

## Regression recipes

Quick checks for the API surface after firmware changes. `V=http://<ip>`.

**Config validation** (each expects `400`):

```sh
curl -i -X POST -d '{"idle_timeout_ms":1000}'  -H 'Content-Type: application/json' $V/config
curl -i -X POST -d '{"orientation":"sideways"}' -H 'Content-Type: application/json' $V/config
curl -i -X POST -d '{"brightness":150}'         -H 'Content-Type: application/json' $V/config
curl -i -X POST -d '{"scrypted":"no-scheme"}'   -H 'Content-Type: application/json' $V/config
curl -i -X POST -d '{"viewport":"'$(python3 -c 'print("x"*60)')'"}' -H 'Content-Type: application/json' $V/config  # >54 chars — mDNS label limit
curl -i -X POST -d 'not json'                   -H 'Content-Type: application/json' $V/config
```

Valid partial updates merge atomically, persist across reboot (NVS), apply
brightness immediately when awake, and refresh the mDNS hostname + TXT on
name/orientation changes. `idle_timeout_ms: 0` is intentionally allowed
(never-sleep).

**Frame push** (test images: `convert -size 480x800 plasma: t.jpg`):

```sh
curl -i -X POST -H 'Content-Type: image/jpeg' --data-binary @t.jpg $V/frame  # 204 awake, 409 asleep
curl -i -X POST -H 'Content-Type: image/png'  --data-binary @t.jpg $V/frame  # 400
curl -i -X POST -H 'Content-Type: image/jpeg' -d 'not a jpeg'      $V/frame  # 400, decode_errors++
# wrong dimensions -> 400 "expected WxH, got WxH"; >1MB -> 413; concurrent second POST -> 503
```

**State machine**:

```sh
curl -i -X POST -d '{"state":"wake"}'  -H 'Content-Type: application/json' $V/state  # 204; repeat = 204 no-op
curl -i -X POST -d '{"state":"middle"}' -H 'Content-Type: application/json' $V/state # 400
# idle timer: set idle_timeout_ms=10000, wake, wait 12s -> /state reports "asleep"
# and the Scrypted script logs the device's sleep callback
```

**OTA**: `make ota` (full loop) or `make verify` after a manual push. The
acceptance criterion: a fresh boot must show `ota_state=pending-verify`
before flipping to `valid`; `valid` at low uptime = silent rollback to the
old slot.

**Device-initiated POSTs**: tap the panel — the script logs
`recv "<name>" -> wake (device-initiated)` and `state_post_failures` in
`/state` stays flat (each POST gets its 204 within the device's 1 s
timeout). Transitions Scrypted itself initiated produce no callback.

---

## Outstanding tests

Never run — the remaining verification backlog:

- **Mid-frame cable pull**: yank Ethernet during an active stream; expect
  idle-sleep after `idle_timeout_ms`, clean recovery on reconnect.
  (Idle-state link loss/recovery was verified during bring-up.)
- **24 h soak**: slow stream, verify `free_heap` / `free_psram` stable and
  `decode_errors` flat.
- **Frame storm**: 30 min at max sustainable fps; no watchdog bites, no
  resets.
- **Brown-out**: PoE at the edge of spec; device must reboot cleanly
  without NVS corruption.
- **Multi-viewport**: needs a second physical unit — per-name callback
  routing, discovery listing both, events on one camera not waking the
  other viewport.
- **Depth-1 outbound queue coalescing** under the real script (verified
  during bring-up against a test receiver only): rapid taps should
  collapse to the final state per in-flight window.

Race handling (concurrent tap vs Scrypted POST, mid-stream tap-to-sleep,
stale-timer-vs-fresh-wake) has been exercised continuously in real use
since bring-up; a DHCP-renumber recovery via the script's mDNS auto-heal
was explicitly verified 2026-07-18 (`host 10.0.13.99 -> 10.0.13.83
(mdns auto-heal)`).

---

## Performance review playbook

Repeatable methodology for "is the device still hitting its baseline" and
"where did the time go." Run after every commit that touches the data
plane. Baseline (Unifi medium substream, `-q:v 1`): **painted = sent =
24 fps**, wire ~74 Mbps avg, g2g ~40–100 ms, zero drops/flushes.

### Per-session capture

```sh
# Firmware serial — 30-frame window summaries
idf.py monitor | tee fw-$(date +%s).log &

# /state poll — windowed stats + glass-to-glass anchor
while true; do curl -s http://VIEWPORT_IP/state; echo; sleep 1; done \
  | tee state-$(date +%s).log &

# Scrypted plugin console — copy/paste from the web UI when done.
```

Trigger a wake (panel tap is the reliable manual trigger; camera events
otherwise). Let it run two minutes. First window after a stream starts is
transient — read steady state from the later windows.

### What the firmware serial says (every 30 painted frames)

One line per window with min/avg/max for every stage plus the TCP-window
decomposition — `wire` (throughput while a body drains; ceiling ≈
`TCP_WND/RTT`), `hdr_gap` (sender idle), `pend_age` (handoff wait), `recv`,
`dec`, `paint`, `idle`, `queued`, `recv_calls`, `recv_chunk`,
`drop-oldest`. The same numbers land in `/state`'s `stream` object
(semantics documented in `stream_server.h`).

### What the Scrypted console says

Cold-start stamps on each wake (`socket connect open +Xms`,
`first ffmpeg frame +Xms` — ~0.7 s prebuffered, ~5–6 s cold), then a 10 s
stream-health line **only when noteworthy** (drops, a buffer-cap flush,
fw-skipped ≥ 2 fps, painted < 20 fps): sent/painted fps, drop counters,
socket-write latency percentiles, backpressure duty cycle, `node_buf`
depth, firmware stage timings, chip temp, and `g2g`.

### Investigation thresholds (alarms)

| Symptom | Probable cause | First check |
|---|---|---|
| Painted fps sustained < 20 | Upstream starvation or wire regression | `hdr_gap` high = sender idle; `wire_avg` pinned = window-bound |
| `dec_avg > 8ms` | HW decoder pathology | `decode_errors` delta; PSRAM cache pressure |
| `paint_max > 200µs` | DPI or DMA stall | `DSI` warnings in serial; `tear_guard_engaged` growth rate |
| `pend_age` growing window-over-window | Kernel-queue latency backlog | The failure mode that killed the naive TCP-window raise — check EMAC RX pool ≥ `TCP_WND` |
| `g2g > 500ms` sustained | User-facing regression | Bisect script-side (`node_buf`, bp%) vs firmware-side (recv/dec) |
| Any `decode_errors` delta | JPEG corruption | TCP retransmit storm or memory corruption |

### Tools

`grep`, `awk`, `gnuplot`. Anything beyond that is over-engineering for a
single device's logs.
