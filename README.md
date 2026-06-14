# Scrypted Viewport v1 Technical Design Specification

Version: 1.0

## Overview

Scrypted Viewport is an Ethernet-powered ambient display appliance optimized for Scrypted camera and doorbell events.

Design goals:
- No Matter
- No HomeKit
- No polling
- No configuration UI
- No authentication
- No rendering engine
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
-> If unconfigured: show IP + hostname on screen
-> Wait for `/config` and `/frame`

Unconfigured display shows the device IP and `viewport.local` so the operator can register it in Scrypted via `POST /config`. Once configured, the screen goes blank and waits for the first `/frame`.

## Resolution

800x480 native.

Scrypted always renders 800x480 JPEGs.

## Network

HTTP listen port: `80`.

mDNS service: `_scrypted-viewport._tcp.local` advertised on port 80.

mDNS TXT records:
- `version=1.0.0`
- `resolution=800x480`
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
  "uptime_ms": 12345678,
  "resolution": "800x480",
  "ip": "192.168.1.42",
  "free_heap": 123456,
  "free_psram": 12345678
}
```

### POST /config

```json
{
  "display": "mudroom",
  "callback": "http://scrypted.local:11080/api/viewport/touch",
  "idle_timeout_ms": 60000
}
```

- Persisted to NVS, survives reboot.
- Idempotent; subsequent calls atomically replace prior config.
- `display` must be non-empty; `callback` must be `http://...`.
- `idle_timeout_ms` optional, default `60000`. Sets the device-side idle timeout. Scrypted should use the same value for its own per-stream timeout so both ends agree (but they time independently — either can end the session).
- Response: `204 No Content`. Invalid body: `400`.

### POST /frame

- `Content-Type: image/jpeg`, body is raw JPEG bytes.
- Image must be 800x480 baseline JPEG. Device does not scale or letterbox.
- Max size: 1 MB.
- Wakes display (backlight on) and resets the 60s idle timer.
- Single in-flight frame; concurrent posts may be rejected with `503`.
- Returns `204` once decoded and pushed to the panel.
- `400` malformed JPEG, `413` over size, `500` decode/display failure. On failure the previous frame stays on screen.

### POST /sleep

- Backlight off; framebuffer preserved.
- Device stays online. Next `/frame` wakes the display. There is no `/wake`.
- Response: `204`.

### POST /brightness

```json
{ "brightness": 75 }
```

- Range `0`–`100`. Out-of-range: `400`.
- Persisted to NVS. Applied immediately if awake, otherwise on next wake.
- Response: `204`.

## Wake / Sleep

Wake and sleep couple the device backlight with the JPEG stream from Scrypted.

- **Wake** = backlight on, frames being received from Scrypted.
- **Sleep** = backlight off, no frames expected.

The device owns wake/sleep state. Scrypted does not — it just receives imperatives:

- `wake` → "start streaming to this display now"
- `sleep` → "stop streaming to this display now"

Both are idempotent on Scrypted's side. Scrypted does not track per-viewport state; it acts on the event and forgets.

Transitions:

| Trigger | Action | Callback to Scrypted |
| --- | --- | --- |
| Tap while asleep | Backlight on, show loading screen | `wake` |
| Tap while awake | Backlight off | `sleep` |
| Idle timer (`idle_timeout_ms` no `/frame`) | Backlight off | `sleep` |
| Scrypted `POST /sleep` | Backlight off | none |
| Scrypted `POST /frame` while asleep | Backlight on, paint frame | none |

Only `tap` is detected on the touchscreen — long-press and swipes are out of scope. `tap` is internal; the callback carries the resulting imperative.

Callback body:

```json
{
  "display": "mudroom",
  "event": "wake",
  "timestamp": 1730000000
}
```

`event` is `wake` or `sleep`. Delivery: best-effort, ~1s timeout, no retry. Events before `/config` registers a callback are dropped.

## Idle

After `idle_timeout_ms` (default 60s) with no `/frame`, the device sleeps and POSTs `sleep`. Scrypted should be configured with the same timeout so its per-stream cutoff matches, but they run independently — either side can end the session, whichever notices first.

## BOOT button

- **Short press**: overlay the IP screen for 15 seconds, then return to the prior state. Useful for identifying or re-registering a device that's already configured. Wakes the backlight temporarily; does not change the wake/sleep state or send a callback.
- **Hold ≥5s**: factory reset — clear NVS, reboot. The device comes back unconfigured, showing the IP screen until Scrypted POSTs `/config`.

## Local rendering

The device renders exactly two things itself; everything else is a JPEG from Scrypted:

- **IP screen**: IP address and `viewport.local` as plain centered text. Shown on first boot until `/config`, after factory reset, and as a 15s overlay when BOOT is short-pressed.
- **Loading screen**: shown between a wake and the next `/frame` arriving. Plain "Loading…" text.

Both use a small embedded bitmap font. No LVGL, no general text engine.

## Scrypted Integration

The Scrypted side is **code, not configuration** — Scrypted has no built-in concept of a network framebuffer. The code is small and lives inside Scrypted:

- **v1**: a Scrypted Script (in the Scripts plugin) — listens for camera events, calls `takePicture()`, POSTs the JPEG to `/frame`. Exposes the `/api/viewport/touch` endpoint via the EndpointManager. ~50 lines of TypeScript, no package install.
- **v2** (planned, not in this spec): a small custom plugin using FFmpeg via `MediaManager` to pipe MJPEG into a chunked `POST /stream`. Adds live-feel frame rates.

Either way, no Scrypted core changes and no external service.

Scrypted owns a static list of viewports, each **bound to one Scrypted camera device** in the script/plugin. The binding tells Scrypted which camera's events drive that viewport (doorbell press, person/motion detection, etc.) and which camera's frames to push to it.

On startup, register every viewport:

```ts
await fetch(`${viewport}/config`, {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({
    display: "mudroom",
    callback: "http://scrypted.local:11080/api/viewport/touch"
  })
});
```

Stream a session of frames to a viewport (in response to a camera event or a `wake` callback):

```ts
await fetch(`${viewport}/frame`, {
  method: "POST",
  headers: { "Content-Type": "image/jpeg" },
  body: jpegBuffer  // 800x480 baseline JPEG, <1 MB
});
```

Handle wake/sleep at `POST /api/viewport/touch`:

- `wake` → look up the camera bound to `display`, start streaming frames.
- `sleep` → stop streaming frames to `display`.

Both are idempotent. Don't track viewport state on the Scrypted side; act on the event and forget.

Scrypted should use the same timeout value it sent in `/config` as its own per-stream cutoff. The two timers run independently — either side can cut a session, whichever notices first. If the viewport's `sleep` callback is lost, the Scrypted-side timeout still ends the stream.

## Ops

- Firmware updates: reflash over USB. No OTA in v1.
- Provisioning: flash the same firmware to every device. On first boot the screen shows its IP; register it from Scrypted via `POST /config`.
- Factory reset: hold BOOT for 5s during normal operation to clear NVS (display name, callback, brightness) and reboot. The device returns to the IP screen.
- No DHCP lease: keep retrying; do not reboot. Screen shows "no network" if unconfigured.
- Ethernet disconnect: reconnect automatically. If Scrypted is unreachable, displays go stale — nothing the device can do about it.

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
