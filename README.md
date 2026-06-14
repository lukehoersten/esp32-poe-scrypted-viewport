# Scrypted Viewport v1 Technical Design Specification

Version: 1.0

## Overview

Scrypted Viewport is an Ethernet-powered ambient display appliance optimized for Scrypted camera and doorbell events.

Design goals:
- No Matter
- No HomeKit
- No device-side polling (the device never asks; Scrypted pushes)
- No configuration UI
- No authentication
- No general-purpose UI framework — the only locally-drawn screens are the IP screen and the Loading screen
- No business logic on the device

Scrypted owns rendering, overlays, camera selection and interaction logic.

Scrypted Viewport owns Ethernet, JPEG decode, display, touch input and callback delivery.

## Hardware

### Controller
Waveshare ESP32-P4-ETH-POE

### Display
5" 800x480 IPS Capacitive Touch MIPI DSI display

## Boot

Power
-> DHCP
-> mDNS (_scrypted-viewport._tcp.local)
-> If unconfigured: backlight on, show IP + hostname (persistent until `/config`)
-> If configured: enter sleep state (backlight off, wait for `/wake` or a tap)

## Resolution

Native panel: 800x480 landscape.

Effective resolution depends on `orientation` (set via `/config`):

- `portrait` (default): 480x800
- `landscape`: 800x480

Scrypted must render JPEGs at the **effective** resolution. The device does not scale or rotate JPEG content — orientation is applied during framebuffer-to-panel push.

## Network

HTTP listen port: `80`.

mDNS service: `_scrypted-viewport._tcp.local` advertised on port 80.

mDNS TXT records:
- `version=1.0.0`
- `resolution=<effective>` (e.g. `480x800` for portrait, `800x480` for landscape)
- `orientation=<portrait|landscape>`
- `name=<display name>` (empty until `/config`)

Hostname: `viewport.local` before configuration, `viewport-<display>.local` after.

Trust model: LAN-only, no auth, no TLS. Deploy on a trusted VLAN.

## API

### GET /health

Returns `200 OK` with JSON:

```json
{
  "name": "mudroom",
  "version": "1.0.0",
  "configured": true,
  "state": "awake",
  "uptime_ms": 12345678,
  "last_frame_ms_ago": 1234,
  "frames_received": 4271,
  "decode_errors": 0,
  "callback_failures": 2,
  "idle_timeout_ms": 60000,
  "brightness": 80,
  "orientation": "portrait",
  "resolution": "480x800",
  "ip": "192.168.1.42",
  "free_heap": 123456,
  "free_psram": 12345678
}
```

`state` is `awake`, `asleep`, or `unconfigured`. `last_frame_ms_ago` is `null` if no frame has been received since boot.

### POST /config

```json
{
  "display": "mudroom",
  "callback": "http://scrypted.local:11080/api/viewport/touch",
  "idle_timeout_ms": 60000,
  "orientation": "portrait"
}
```

- Persisted to NVS, survives reboot.
- Idempotent; subsequent calls atomically replace prior config.
- `display` must be non-empty; `callback` must be `http://...`.
- `idle_timeout_ms` optional, default `60000`. `0` disables the idle timer entirely. Non-zero values must be ≥ `5000`; otherwise `400`. Scrypted should use the same value for its own per-stream timeout so both ends agree, but they time independently — either can end the session.
- `orientation` optional, default `portrait`. Values: `portrait` (480x800) or `landscape` (800x480). Changing orientation takes effect immediately, including for the IP and Loading screens. Scrypted must send JPEGs at the new effective resolution after a change.
- Response: `204 No Content`. Invalid body: `400`.

### POST /wake

Transitions the device to the wake state (backlight on, loading screen) without painting a frame.

- Idempotent. Already-awake calls return `204` and do nothing.
- Resets the idle timer.
- No callback to Scrypted (Scrypted already knows — it initiated).
- Response: `204`.

### POST /sleep

Transitions the device to the sleep state.

- Idempotent. Already-asleep calls return `204` and do nothing.
- Backlight off; framebuffer is discarded (not preserved across sleep).
- No callback to Scrypted.
- Response: `204`.

### POST /frame

Paints a frame. Does **not** change wake/sleep state.

- `Content-Type: image/jpeg`, body is raw JPEG bytes.
- Image must match the effective resolution (480x800 portrait, 800x480 landscape) as a baseline JPEG. Device does not scale, rotate, or letterbox JPEG content.
- Max size: 1 MB.
- **Requires awake state.** While asleep, returns `409 Conflict` and does not paint. Scrypted must `POST /wake` first (or wait for a `wake` callback from a tap).
- Resets the idle timer on success.
- Single in-flight frame; concurrent posts may be rejected with `503`.
- Returns `204` once decoded and pushed to the panel.
- `400` malformed JPEG, `409` device asleep, `413` over size, `500` decode/display failure. On failure the previous frame stays on screen.

### POST /brightness

```json
{ "brightness": 75 }
```

- Range `0`–`100`. Out-of-range: `400`.
- Default on first boot: `80`.
- Persisted to NVS. Applied immediately if awake, otherwise on next wake.
- Idempotent. Response: `204`.

## Wake / Sleep

Wake and sleep couple the device backlight with Scrypted's frame stream. The device owns the state; Scrypted just receives idempotent imperatives:

- `wake` → "start streaming to this display now"
- `sleep` → "stop streaming to this display now"

Scrypted does not track per-viewport state; it acts on the event and forgets. Repeat events are safe.

Transitions:

| Trigger | Resulting state | Callback to Scrypted |
| --- | --- | --- |
| Tap while asleep | Awake (loading screen) | `wake` `type=tap` |
| Tap while awake | Asleep | `sleep` `type=tap` |
| Idle timer expires | Asleep | `sleep` `type=timeout` |
| `POST /wake` | Awake (loading screen) | none |
| `POST /sleep` | Asleep | none |
| `POST /frame` | (no state change) | — |

`/frame` never changes state. Scrypted must `POST /wake` (or wait for a tap-driven `wake` callback) before sending frames. This makes the protocol race-free: a `/frame` arriving after a tap-to-sleep is rejected with `409`, not silently re-woken.

Only `tap` is detected on the touchscreen — long-press and swipes are out of scope for v1. `tap` itself is internal; what Scrypted sees is the resulting `wake` or `sleep`.

Callback body:

```json
{
  "display": "mudroom",
  "event": "wake",
  "type": "tap"
}
```

- `event` is `wake` or `sleep`.
- `type` carries the cause: `tap` (any user tap), `timeout` (idle expiry — `sleep` only). Future types (e.g. `swipe_left`) can be added without changing the schema. Scrypted may ignore `type` in v1.

Delivery: best-effort, ~1s timeout, no retry. Events before `/config` registers a callback are dropped. If the callback POST fails, the local state change still happens — Scrypted catches up via its own timeout or the next event.

No wall-clock timestamp is included. The device has no RTC and no SNTP. Scrypted timestamps events on receipt.

## Idle

After `idle_timeout_ms` (default 60s) with no `/frame`, the device sleeps and POSTs `sleep` with `type=timeout`. Scrypted should use the same timeout so its per-stream cutoff matches, but they run independently — either side can end the session, whichever notices first.

## Idempotency

All endpoints are safe to retry. Every state-change path converges to the same final state regardless of how many times it's repeated or in what order:

| Endpoint | Idempotent? | Notes |
| --- | --- | --- |
| `GET /health` | yes | side-effect free |
| `POST /config` | yes | atomically replaces prior config |
| `POST /wake` | yes | no-op if already awake |
| `POST /sleep` | yes | no-op if already asleep |
| `POST /frame` | yes (within state) | paints if awake; `409` if asleep — no partial state |
| `POST /brightness` | yes | value is overwritten |

Callbacks (`wake`, `sleep`) are imperatives, not notifications. Scrypted acts and forgets; the device never expects an ack. A dropped callback is recovered by the next user action, by Scrypted's own timeout, or by a `/frame` returning `409`.

Failure modes do not corrupt state:

- Failed callback POST: local state still changes; counter increments.
- Failed JPEG decode: previous frame stays; state unchanged.
- Network drop mid-stream: device idle-sleeps when the timer expires.
- Concurrent `/frame` posts: one wins, the other gets `503`; no half-painted frames.

## BOOT button

- **Short press**: overlay the IP screen for 15 seconds, then return to the prior state. Useful for identifying or re-registering a device that's already configured. Wakes the backlight temporarily; does not change the wake/sleep state or send a callback. An incoming `/frame` while the overlay is showing is rejected with `409` (state is still "sleep" underneath).
- **Hold ≥5s**: factory reset — clear NVS, reboot. The device comes back unconfigured, showing the IP screen until Scrypted POSTs `/config`.

## Local rendering

The device renders exactly two things itself; everything else is a JPEG from Scrypted:

- **IP screen**: IP address and `viewport.local` as plain centered text. Shown on first boot until `/config`, after factory reset, and as a 15s overlay when BOOT is short-pressed. Rendered in the current orientation (portrait by default).
- **Loading screen**: shown between a wake and the next `/frame` arriving. Plain "Loading…" text. Rendered in the current orientation.

Both use a small embedded bitmap font. No LVGL, no general text engine.

## Scrypted Integration

The Scrypted side is **code, not configuration** — Scrypted has no built-in concept of a network framebuffer. The code is small and lives inside Scrypted:

- **v1**: a Scrypted Script (in the Scripts plugin) — listens for camera events, calls `takePicture()`, POSTs the JPEG to `/frame`. Exposes the `/api/viewport/touch` endpoint via the EndpointManager. ~50 lines of TypeScript, no package install.
- **Next milestone after v1 (`/stream`)**: add `POST /stream` with `multipart/x-mixed-replace` chunked body for live frame rates. Scrypted side becomes a small custom plugin using FFmpeg via `MediaManager` to pipe MJPEG. `/frame` stays for snapshots and debug.

Either way, no Scrypted core changes and no external service.

Scrypted owns a static list of viewports, each **bound to one Scrypted camera device** in the script/plugin. The binding tells Scrypted which camera's events drive that viewport (doorbell press, person/motion detection, etc.) and which camera's frames to push to it.

On startup, register every viewport:

```ts
await fetch(`${viewport}/config`, {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({
    display: "mudroom",
    callback: "http://scrypted.local:11080/api/viewport/touch",
    idle_timeout_ms: 60000,
    orientation: "portrait"
  })
});
```

To start a session (camera event like doorbell, motion, person):

```ts
await fetch(`${viewport}/wake`, { method: "POST" });   // device shows loading
// then stream frames:
await fetch(`${viewport}/frame`, {
  method: "POST",
  headers: { "Content-Type": "image/jpeg" },
  body: jpegBuffer  // baseline JPEG at the viewport's effective resolution, <1 MB
});
```

Handle wake/sleep callbacks at `POST /api/viewport/touch`:

- `wake` → look up the camera bound to `display`, start streaming frames. (You do not need to POST `/wake` first — the device is already awake when it sends this.)
- `sleep` → stop streaming frames to `display`.

Both are idempotent on Scrypted's side. Don't track viewport state; act on the event and forget. Ignore the `type` field in v1.

Scrypted should use the same `idle_timeout_ms` value it sent in `/config` as its own per-stream cutoff. The two timers run independently — either side can cut a session, whichever notices first. If the viewport's `sleep` callback is lost, the Scrypted-side timeout still ends the stream; the next `/frame` posted after the device idle-slept simply returns `409` and Scrypted stops.

## Ops

- Firmware updates: reflash over USB. No OTA in v1 (planned post-v1: HTTP OTA from Scrypted).
- Provisioning: flash the same firmware to every device. On first boot the screen shows its IP; register it from Scrypted via `POST /config`.
- Display names must be unique across the LAN — mDNS hostnames are derived from `display` and two viewports configured with the same name will collide.
- Factory reset: hold BOOT for 5s during normal operation to clear NVS (display name, callback, brightness, idle timeout) and reboot. The device returns to the IP screen.
- No DHCP lease: keep retrying; do not reboot. Screen shows "no network" if unconfigured.
- Ethernet disconnect: reconnect automatically. If Scrypted is unreachable, displays go stale — nothing the device can do about it.
- Watchdog: the ESP-IDF task watchdog reboots the device if a task hangs. Soft state is rebuilt from NVS on every boot.
- Status LED (on-board): solid = configured & online, slow blink = unconfigured (waiting for `/config`), fast blink = no network. The screen tells the same story but the LED is visible when the screen is asleep.

## Build

```sh
source ~/Dev/code/git/esp32/env.sh
cd ~/Dev/code/git/esp32/projects/esp32-poe-scrypted-viewport
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

## Philosophy

Scrypted Viewport is a thin network framebuffer appliance.

ESP:
- DHCP
- mDNS
- HTTP server
- JPEG decode
- framebuffer
- touch
- callback

Everything else belongs in Scrypted.
