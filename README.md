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
-> Wait

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
  "callback": "http://scrypted.local:11080/api/viewport/touch"
}
```

- Persisted to NVS, survives reboot.
- Idempotent; subsequent calls atomically replace prior config.
- `display` must be non-empty; `callback` must be `http://...`.
- Response: `204 No Content`. Invalid body: `400`.

### POST /frame

- `Content-Type: image/jpeg`, body is raw JPEG bytes.
- Image must be 800x480 baseline JPEG. Device does not scale or letterbox.
- Max size: 1 MB.
- Wakes display (backlight on) and resets the idle timer.
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

## Touch Callback

Device POSTs to the `callback` URL registered via `/config`:

```json
{
  "display": "mudroom",
  "event": "tap",
  "timestamp": 1730000000
}
```

Events: `tap`, `long_press`, `swipe_left`, `swipe_right`. No coordinates — the device emits gestures, not points.

Delivery: best-effort, ~1 second timeout, no retry queue. Events before `/config` registers a callback are dropped. The device does not interpret events; Scrypted decides what to show next.

## Scrypted Integration

Scrypted owns a static list of viewports and pushes frames to each. On startup, register every viewport:

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

Push a frame:

```ts
await fetch(`${viewport}/frame`, {
  method: "POST",
  headers: { "Content-Type": "image/jpeg" },
  body: jpegBuffer  // 800x480 baseline JPEG, <1 MB
});
```

Handle touch at `POST /api/viewport/touch`. The handler is the entire interaction layer — camera selection, page cycling, snapshot vs live, sleep — all in Scrypted, keyed off `display` and `event`.

Idle behavior is device-local: the viewport sleeps the backlight after ~30s with no new frame. Scrypted does not need to send `/sleep` unless it wants to dim early.

## Ops

- Firmware updates: reflash over USB. No OTA in v1.
- Factory reset: hold BOOT during power-on to clear NVS (display name, callback, brightness).
- Boot screen: black until first `/frame`. `/health` is available as soon as DHCP completes.
- No DHCP lease: keep retrying; do not reboot.
- Ethernet disconnect: reconnect automatically; keep last frame on screen.

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
