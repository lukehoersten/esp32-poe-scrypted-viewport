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
-> If configured: enter sleep state (backlight off, wait for `POST /state` or a tap)

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

Four endpoints. `GET /state` and `GET /config` are the read surface; `POST /config`, `POST /state`, and `POST /frame` are the write surface.

### GET /state

Returns the device's runtime status. Replaces the old `/health` endpoint.

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
  "resolution": "480x800",
  "ip": "192.168.1.42",
  "free_heap": 123456,
  "free_psram": 12345678
}
```

`state` is `awake`, `asleep`, or `unconfigured`. `last_frame_ms_ago` is `null` if no frame has been received since boot.

### POST /state

```json
{ "state": "wake" }
```

- `state` is `wake` or `sleep`. Anything else: `400`.
- Idempotent. Already-in-that-state calls return `204` and do nothing.
- `wake`: backlight on, render loading screen (until the next `/frame`), reset idle timer.
- `sleep`: backlight off; framebuffer discarded.
- No callback echo (Scrypted initiated).
- Response: `204`.

### GET /config

Returns the persisted config:

```json
{
  "display": "mudroom",
  "callback": "http://scrypted.local:11080/api/viewport/touch",
  "idle_timeout_ms": 60000,
  "orientation": "portrait",
  "brightness": 80
}
```

Before first `/config`: returns `200` with an object whose fields are all `null` except defaults (`brightness: 80`, `orientation: "portrait"`, `idle_timeout_ms: 60000`).

### POST /config

```json
{
  "display": "mudroom",
  "callback": "http://scrypted.local:11080/api/viewport/touch",
  "idle_timeout_ms": 60000,
  "orientation": "portrait",
  "brightness": 80
}
```

- **Partial update**: only fields present in the body are changed; omitted fields keep their current values. The persisted config is replaced atomically with the merged result.
- Persisted to NVS, survives reboot.
- Idempotent; reposting the same body yields the same state.
- `display` must be non-empty; `callback` must be `http://...`.
- `idle_timeout_ms`: `0` disables the idle timer; non-zero values must be ≥ `5000`. Otherwise `400`. Scrypted should use the same value for its own per-stream timeout so both ends agree, but they time independently — either can end the session.
- `orientation`: `portrait` (480x800) or `landscape` (800x480). Default `portrait` on first boot. Changing orientation takes effect immediately, including for the IP and Loading screens. Scrypted must send JPEGs at the new effective resolution after a change.
- `brightness`: integer `0`–`100`. Default `80` on first boot. Applied immediately if awake; takes effect on next wake if asleep. PWM is gamma-corrected so the scale is perceptual.
- Response: `204 No Content`. Invalid body: `400`.

To tweak only brightness:

```json
{ "brightness": 50 }
```

### POST /frame

Paints a frame. Does **not** change wake/sleep state.

- `Content-Type: image/jpeg`, body is raw JPEG bytes.
- Image must match the effective resolution (480x800 portrait, 800x480 landscape) as a baseline JPEG. Device does not scale, rotate, or letterbox JPEG content.
- Max size: 1 MB.
- **Requires awake state.** While asleep, returns `409 Conflict` and does not paint. Scrypted must `POST /state {"state":"wake"}` first (or wait for a `wake` callback from a tap).
- Resets the idle timer on success.
- Single in-flight frame; concurrent posts may be rejected with `503`.
- Returns `204` once decoded and pushed to the panel.
- `400` malformed JPEG, `409` device asleep, `413` over size, `500` decode/display failure. On failure the previous frame stays on screen.

## Wake / Sleep

Wake and sleep couple the device backlight with Scrypted's frame stream. The device owns the state; Scrypted just receives idempotent imperatives in callbacks:

- `state=wake` → "start streaming to this display now"
- `state=sleep` → "stop streaming to this display now"

Scrypted does not track per-viewport state; it acts on the callback and forgets. Repeats are safe.

Transitions:

| Trigger | Resulting state | Callback to Scrypted |
| --- | --- | --- |
| Tap while asleep | Awake (loading screen) | `state=wake event=tap` |
| Tap while awake | Asleep | `state=sleep event=tap` |
| Idle timer expires | Asleep | `state=sleep event=timeout` |
| `POST /state {"state":"wake"}` | Awake (loading screen) | none |
| `POST /state {"state":"sleep"}` | Asleep | none |
| `POST /frame` | (no state change) | — |

`/frame` never changes state. Scrypted must `POST /state {"state":"wake"}` (or wait for a tap-driven callback) before sending frames. This makes the protocol race-free: a `/frame` arriving after a tap-to-sleep is rejected with `409`, not silently re-woken.

Only `tap` is detected on the touchscreen — long-press and swipes are out of scope for v1. `tap` itself is internal; what Scrypted sees is the resulting `state`.

Callback body:

```json
{
  "display": "mudroom",
  "state": "wake",
  "event": "tap"
}
```

- `state`: `wake` or `sleep` — the resulting state, also the imperative for Scrypted.
- `event`: the cause. `tap` (any user tap) or `timeout` (idle expiry, sleep only). Future events (e.g. `swipe_left`) can be added without schema changes. Scrypted may ignore `event` in v1.

Delivery: best-effort, ~1s timeout, no retry. Callbacks before `/config` registers a URL are dropped. If the callback POST fails, the local state change still happens — Scrypted catches up via its own timeout or the next callback.

No wall-clock timestamp is included. The device has no RTC and no SNTP. Scrypted timestamps callbacks on receipt.

## Idle

After `idle_timeout_ms` (default 60s) with no `/frame`, the device sleeps and POSTs `state=sleep` with `event=timeout`. Scrypted should use the same timeout so its per-stream cutoff matches, but they run independently — either side can end the session, whichever notices first.

## Idempotency

All endpoints are safe to retry. Every state-change path converges to the same final state regardless of how many times it's repeated or in what order:

| Endpoint | Idempotent? | Notes |
| --- | --- | --- |
| `GET /state` | yes | side-effect free |
| `GET /config` | yes | side-effect free |
| `POST /config` | yes | partial merge into persisted config, atomic |
| `POST /state` | yes | no-op if already in that state |
| `POST /frame` | yes (within state) | paints if awake; `409` if asleep — no partial state |

Callbacks (`state=wake`, `state=sleep`) are imperatives, not notifications. Scrypted acts and forgets; the device never expects an ack. A dropped callback is recovered by the next user action, by Scrypted's own timeout, or by a `/frame` returning `409`.

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
    orientation: "portrait",
    brightness: 80
  })
});
```

To start a session (camera event like doorbell, motion, person):

```ts
await fetch(`${viewport}/state`, {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ state: "wake" })
});  // device shows loading

// then stream frames:
await fetch(`${viewport}/frame`, {
  method: "POST",
  headers: { "Content-Type": "image/jpeg" },
  body: jpegBuffer  // baseline JPEG at the viewport's effective resolution, <1 MB
});
```

Handle callbacks at `POST /api/viewport/touch` (body: `{display, state, event}`):

- `state=wake` → look up the camera bound to `display`, start streaming frames. (You do not need to `POST /state` first — the device is already awake when it sends this.)
- `state=sleep` → stop streaming frames to `display`.

Both are idempotent on Scrypted's side. Don't track viewport state; act on the callback and forget. Ignore the `event` field in v1.

Scrypted should use the same `idle_timeout_ms` value it sent in `/config` as its own per-stream cutoff. The two timers run independently — either side can cut a session, whichever notices first. If the viewport's sleep callback is lost, the Scrypted-side timeout still ends the stream; the next `/frame` posted after the device idle-slept simply returns `409` and Scrypted stops.

## Ops

- Firmware updates: reflash over USB. No OTA in v1 (planned post-v1: HTTP OTA from Scrypted).
- Provisioning: flash the same firmware to every device. On first boot the screen shows its IP; register it from Scrypted via `POST /config`.
- Display names must be unique across the LAN — mDNS hostnames are derived from `display` and two viewports configured with the same name will collide.
- Factory reset: hold BOOT for 5s during normal operation to clear NVS (display name, callback, brightness, idle timeout, orientation) and reboot. The device returns to the IP screen.
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
