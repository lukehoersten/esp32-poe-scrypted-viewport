# Scrypted Viewport v2 Claude Code Implementation Guide

Version: 2.0  
Target: Claude Code / ESP-IDF firmware implementation  
Hardware: Waveshare ESP32-P4-ETH-POE + 5" 800×480 IPS Capacitive Touch MIPI DSI display  
Project name: Scrypted Viewport

---

## 1. Project Summary

Scrypted Viewport is a thin Ethernet-powered network framebuffer appliance for Scrypted.

It runs on a Waveshare ESP32-P4-ETH-POE board connected to a 5" 800×480 MIPI DSI capacitive-touch display.

The device does not render UI, calendars, camera labels, overlays, fonts, or business logic. It receives fully rendered JPEG frames from Scrypted, decodes them, displays them, and reports touch gestures back to Scrypted.

The goal is a reliable appliance, not a general-purpose smart display.

---

## 2. Non-Goals

Do not implement:

- Matter
- HomeKit
- Wi-Fi
- BLE
- polling
- authentication
- cloud connectivity
- web UI
- local calendar/weather/camera logic
- HTML rendering
- template rendering
- local camera streaming clients
- configuration portal
- multi-user support
- database
- display abstraction for other screens

If tempted to add application logic to the ESP, move that logic to Scrypted.

---

## 3. Hardware Target

### Controller

Waveshare ESP32-P4-ETH-POE

Expected capabilities:

- ESP32-P4
- Ethernet
- PoE
- PSRAM
- MIPI DSI display interface
- touch interface
- USB programming/debugging

### Display

5" 800×480 IPS capacitive-touch MIPI DSI display, Raspberry Pi-compatible Hosyond/Amazon model.

Native panel resolution:

```text
800x480 (landscape)
```

Effective resolution depends on `orientation` set via `/config`:

```text
portrait  (default) -> 480x800
landscape           -> 800x480
```

Orientation is applied during framebuffer-to-panel push. JPEGs from Scrypted must match the effective resolution; the device never rotates or scales JPEG content.

Firmware should target this exact hardware. No runtime display detection is required.

---

## 4. High-Level Runtime Behavior

Boot flow:

```text
power on
  -> initialize logging
  -> initialize NVS
  -> load saved config (display name, callback, brightness)
  -> initialize Ethernet
  -> DHCP
  -> initialize mDNS
  -> initialize display/backlight/touch
  -> start HTTP server
  -> advertise _scrypted-viewport._tcp.local
  -> if unconfigured: render IP + viewport.local on screen
  -> wait for API calls
```

Wake/sleep model:

Wake and sleep couple the backlight with Scrypted's frame stream. The two move together, never independently. The device owns the state.

```text
awake = backlight on,  Scrypted streaming frames
asleep = backlight off, Scrypted not streaming
```

State changes are strictly limited to these triggers:

```text
tap while asleep                     -> awake  (callback: state=wake event=tap)
tap while awake                      -> asleep (callback: state=sleep event=tap)
idle timer expires                   -> asleep (callback: state=sleep event=timeout)
POST /state {"state":"wake"} asleep  -> awake  (no callback)
POST /state {"state":"sleep"} awake  -> asleep (no callback)
```

`/frame` never changes state. It paints if awake, returns `409` if asleep. This eliminates the race where a frame in flight could re-wake a device that just slept on a tap.

Scrypted-initiated state changes via `POST /state` do not echo a callback.

Frame flow (awake state only):

```text
Scrypted POST /frame
  -> Scrypted Viewport receives raw JPEG
  -> decode JPEG into framebuffer
  -> push to DSI display
  -> reset idle timer
  -> respond 204
```

Callback body has two fields:
- `state`: `wake` or `sleep` — the resulting state and the imperative for Scrypted.
- `event`: the cause. `tap` (any user tap) or `timeout` (idle expiry, sleep only). Forward-compat for future inputs like swipes.

Callbacks are idempotent imperatives — "start streaming" / "stop streaming" — not state notifications. Scrypted does not track per-viewport state. Scrypted may ignore `event` in v1.

Only `tap` is detected on the touchscreen. Long-press and swipes are out of scope for v1.

No wall-clock timestamps. The device has no RTC and no SNTP.

BOOT button:
- Short press: overlay IP screen for 15s, then return to prior state. Wakes backlight for the overlay but does not change wake/sleep state and does not POST a callback. Incoming `/frame` during the overlay is rejected `409`.
- Hold ≥5s: clear NVS and reboot. Device comes back unconfigured.

---

## 5. API Contract

The firmware exposes a minimal HTTP API.

Default port:

```text
80
```

mDNS service:

```text
_scrypted-viewport._tcp.local
```

Example hostname:

```text
viewport.local
viewport-mudroom.local
```

Four endpoints total: `GET /state`, `GET /config`, `POST /config`, `POST /state`, `POST /frame`.

### 5.1 GET /state

Return JSON status (replaces the old `/health`).

Example response:

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

Status code: `200 OK`.

### 5.2 GET /config

Return the persisted config:

```json
{
  "display": "mudroom",
  "callback": "http://scrypted.local:11080/api/viewport/touch",
  "idle_timeout_ms": 60000,
  "orientation": "portrait",
  "brightness": 80
}
```

Before first `/config`: `display` and `callback` are `null`; the rest carry their first-boot defaults (`idle_timeout_ms: 60000`, `orientation: "portrait"`, `brightness: 80`).

Status code: `200 OK`.

### 5.3 POST /config

Sets or updates the device config. Partial-update semantics — only fields present in the body are changed; omitted fields keep their current values. The merged config is persisted to NVS atomically.

Request body (full form):

```json
{
  "display": "mudroom",
  "callback": "http://scrypted.local:11080/api/viewport/touch",
  "idle_timeout_ms": 60000,
  "orientation": "portrait",
  "brightness": 80
}
```

To tweak only brightness:

```json
{ "brightness": 50 }
```

Behavior:

- For each present field: validate, then update the persisted record.
- Apply orientation and brightness changes immediately. Orientation update redraws the IP/Loading screens (if shown) and updates mDNS TXT records.
- Survive reboot.
- Idempotent: re-posting the same body yields the same state.

Validation:

- `display`: non-empty string.
- `callback`: must be `http://...`.
- `idle_timeout_ms`: `0` disables the idle timer; non-zero must be ≥ `5000`. Otherwise `400`.
- `orientation`: `portrait` or `landscape`. Otherwise `400`.
- `brightness`: integer `0`–`100`. Otherwise `400`.
- HTTPS is not required for v1/v2.

Response: `204 No Content`.

### 5.4 POST /state

Sets the wake/sleep state.

Request body:

```json
{ "state": "wake" }
```

or

```json
{ "state": "sleep" }
```

Behavior:

- `wake`: backlight on, render loading screen, reset idle timer. No-op if already awake.
- `sleep`: backlight off, framebuffer discarded. No-op if already asleep.
- Do not POST a callback (Scrypted initiated).
- Idempotent.

Validation: `state` must be `wake` or `sleep`. Otherwise `400`.

Response: `204 No Content`.

### 5.5 POST /frame

Paints a JPEG. Does not change wake/sleep state.

Headers: `Content-Type: image/jpeg`. Body: raw JPEG bytes. Expected image: baseline JPEG at the current effective resolution (480x800 portrait, 800x480 landscape). Max size: 1 MB.

Behavior:

- If asleep: return `409 Conflict`. Do not paint. Scrypted must `POST /state {"state":"wake"}` first.
- If awake: decode, push to display, reset idle timer, return `204`.

Failure responses:

```text
400 Bad Request        invalid content-type or malformed JPEG
409 Conflict           device is asleep; POST /state {"state":"wake"} first
413 Payload Too Large  JPEG exceeds configured maximum
500 Internal Error     decode/display failure (previous frame remains)
503 Service Unavailable concurrent frame in flight
```

Scrypted is responsible for scaling/cropping/composition. Scrypted Viewport does not scale.

---

## 6. Touch Callback Contract

Scrypted Viewport initiates HTTP POSTs to the configured callback URL.

Example request body:

```json
{
  "display": "mudroom",
  "state": "wake",
  "event": "tap"
}
```

Supported combinations:

```text
state=wake   event=tap        (touchscreen tap while asleep)
state=sleep  event=tap        (touchscreen tap while awake)
state=sleep  event=timeout    (idle timer expired)
```

No wall-clock timestamp — the device has no RTC and no SNTP. Scrypted timestamps on receipt.

`state` is the resulting wake/sleep state, also the imperative for Scrypted. `event` carries the cause and is forward-compatible with future inputs (swipes, long-press, hardware buttons). Scrypted may ignore `event` in v1.

Rules:

- The device owns wake/sleep state. Scrypted does not track it.
- Scrypted owns the viewport→camera binding and decides which stream goes to which viewport.
- Callbacks are idempotent imperatives: `state=wake` means "start streaming", `state=sleep` means "stop streaming". Scrypted acts on receipt and forgets.
- Scrypted enforces its own per-stream timeout independently of the device's idle timer. Either can end a session, whichever notices first.
- Scrypted-initiated `POST /state` and `POST /frame` do not echo a callback.

Callback delivery:

- Best effort.
- Timeout quickly, e.g. 1 second.
- Log failures.
- Do not block display operation for long.
- No retry queue required for v1/v2.

---

## 7. Firmware Architecture

Recommended ESP-IDF project layout:

```text
scrypted-viewport/
  CMakeLists.txt
  sdkconfig.defaults
  README.md

  main/
    CMakeLists.txt
    app_main.c

    viewport_config.h
    viewport_state.h
    viewport_state.c

    net_eth.h
    net_eth.c

    mdns_service.h
    mdns_service.c

    http_api.h
    http_api.c

    display.h
    display.c

    jpeg_decoder.h
    jpeg_decoder.c

    touch.h
    touch.c

    callback_client.h
    callback_client.c

    idle_timer.h
    idle_timer.c

    local_screens.h
    local_screens.c

    nvs_config.h
    nvs_config.c
```

Keep modules boring and explicit.

---

## 8. Module Responsibilities

### 8.1 `app_main.c`

Responsibilities:

- Initialize NVS.
- Load saved config.
- Initialize Ethernet.
- Wait for IP address.
- Initialize display.
- Initialize touch.
- Start HTTP server.
- Start mDNS.
- Start idle timer.
- Start touch task.

Pseudo-flow:

```c
void app_main(void) {
    nvs_config_init();
    viewport_state_init();

    net_eth_init();
    net_eth_wait_for_ip();

    display_init();
    display_set_brightness(saved_brightness);
    display_sleep();

    touch_init();
    touch_start_task();

    http_api_start();
    mdns_service_start();

    idle_timer_start();
}
```

### 8.2 `net_eth.c`

Responsibilities:

- Initialize Ethernet driver for Waveshare ESP32-P4-ETH-POE.
- Use DHCP.
- Publish current IP to global state.

No Wi-Fi support.

### 8.3 `mdns_service.c`

Responsibilities:

- Set hostname.
- Advertise `_scrypted-viewport._tcp.local` on port 80.
- Include TXT records:

```text
version=1.0.0
resolution=<effective>    (480x800 or 800x480)
orientation=<portrait|landscape>
name=<display name>
```

Update TXT records when `orientation` changes via `/config`.

### 8.4 `http_api.c`

Responsibilities:

- Register routes:
  - `GET /state`
  - `GET /config`
  - `POST /config`
  - `POST /state`
  - `POST /frame`
- Parse JSON for config and state bodies. Partial /config bodies are merged into the persisted record.
- Stream JPEG request body into buffer.
- Reject `/frame` with `409` when state is `asleep` (do not auto-wake).
- All endpoints idempotent.
- Return simple status codes.

### 8.5 `display.c`

Responsibilities:

- Initialize MIPI DSI display.
- Initialize framebuffer or draw buffer (always allocate 800x480x2 in RGB565; orientation determines mapping during push).
- Control backlight.
- Push decoded frames to the panel with the current orientation applied (portrait = 90° rotation).
- Sleep/wake backlight.

Do not draw application UI.

### 8.6 `jpeg_decoder.c`

Responsibilities:

- Decode JPEG into display-compatible pixel buffer.
- Prefer ESP-IDF JPEG/image decode support if available for ESP32-P4.
- Avoid repeated allocations where possible.
- Reuse a static PSRAM frame buffer.

Input:

```text
JPEG byte buffer
```

Output:

```text
800x480 framebuffer
```

### 8.7 `touch.c`

Responsibilities:

- Initialize capacitive touch controller.
- Detect `tap` only (touch-down + release within ~500ms, ignore movement). Long-press and swipes are out of scope.
- Debounce.
- On tap:
  - If asleep: transition to awake — backlight on, render loading screen, POST `{"state":"wake","event":"tap"}`.
  - If awake: transition to asleep — backlight off, POST `{"state":"sleep","event":"tap"}`.
- Also handle BOOT button:
  - Short press: ask `local_screens` to overlay the IP screen for 15s, then restore prior state. No callback.
  - Hold ≥5s: clear NVS via `nvs_config_reset()` and reboot.

### 8.8 `callback_client.c`

Responsibilities:

- POST touch JSON to configured callback URL.
- Fast timeout.
- Best effort.
- Do not retry indefinitely.

### 8.9 `idle_timer.c`

Responsibilities:

- Reset timer on `/frame`, `POST /state {state:wake}`, and tap-driven wake.
- On expiry: transition to asleep (backlight off, POST `{"state":"sleep","event":"timeout"}`).
- Idle timeout: read from NVS (`idle_timeout_ms`), default 60000 ms. `0` disables the timer; non-zero values must be ≥ 5000 ms (validated at `/config`).
- Scrypted is expected to use the same value as its own per-stream cutoff, but timers run independently — either side can end a session, whichever notices first.

### 8.10 `local_screens.c`

The only application UI the firmware draws. Two screens, both via a small embedded bitmap font (no LVGL, no general text engine).

Responsibilities:

- `local_screens_show_ip(ip)` — centered IP address and `viewport.local`. Shown:
  - on boot when no callback URL is in NVS (persistent until `/config` arrives), and
  - as a 15s overlay on BOOT short-press (then restore prior state).
- `local_screens_show_loading()` — centered "Loading…" text. Shown on wake until the next `/frame` lands.
- Render into the same RGB565 framebuffer the JPEG decoder targets, then push to the panel.

Keep it small. Hard-code the font glyphs for digits, dots, colons, and a-z. No string formatting library — write tiny inline blitters.

### 8.11 `nvs_config.c`

Responsibilities:

Persist:

- display name
- callback URL
- brightness
- idle timeout (ms)
- orientation

Do not persist frame data.

---

## 9. State Model

Global state should be small:

```c
typedef struct {
    char display_name[64];
    char callback_url[256];

    bool configured;
    bool display_awake;

    uint8_t brightness;      // 0-100
    uint32_t idle_timeout_ms;
    uint8_t orientation;     // 0 = portrait, 1 = landscape

    char ip_addr[64];

    uint64_t frames_received;
    uint64_t touch_events_sent;
    uint64_t decode_errors;
} viewport_state_t;
```

---

## 10. Memory Strategy

Use PSRAM for:

- JPEG request body buffer
- decode buffer
- framebuffer/draw buffer

Suggested constants:

```c
#define VIEWPORT_WIDTH 800
#define VIEWPORT_HEIGHT 480
#define VIEWPORT_MAX_JPEG_SIZE (1024 * 1024)
```

Target pixel format depends on display driver. Prefer RGB565 if supported:

```text
800 * 480 * 2 = 768,000 bytes
```

This fits comfortably in PSRAM.

---

## 11. Display Strategy

v1/v2 should use the simplest reliable display path.

Preferred:

```text
JPEG -> RGB565 framebuffer -> DSI display
```

Do not implement:

- double buffering unless needed
- animations
- transitions
- LVGL UI
- general-purpose text rendering (only the tiny bitmap font in `local_screens.c`)

Apply a gamma curve to the brightness PWM duty cycle. Linear 0–100 maps to non-linear perceptual brightness; without correction, `50` looks much brighter than half. A simple `duty = (level / 100) ^ 2.2` lookup table is enough.

If the Waveshare ESP32-P4 examples include a working DSI display demo, start from that display initialization code and strip it down.

Acceptance criterion:

- Boot shows a test color or test image.
- `/frame` updates the full screen.
- Backlight turns off/on reliably.

---

## 12. Scrypted Integration Assumptions

Scrypted owns a list of viewports, each bound to one camera device:

```ts
const viewports = [
  { url: "http://viewport-mudroom.local", display: "mudroom", camera: "frontDoor" },
  { url: "http://viewport-kitchen.local", display: "kitchen", camera: "driveway" },
];
```

The binding lives in Scrypted config. The device knows nothing about cameras.

Registration on startup:

```ts
for (const v of viewports) {
  await fetch(`${v.url}/config`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      display: v.display,
      callback: "http://scrypted.local:11080/api/viewport/touch",
      idle_timeout_ms: 60000,    // device uses this; Scrypted uses the same value
      orientation: "portrait",    // override per viewport if a screen is wall-mounted sideways
      brightness: 80,
    }),
  });
}
```

Stream a session of frames to a viewport — triggered by either a camera event (doorbell, person, motion) or a `state=wake` callback from the viewport:

```ts
await fetch(`${v.url}/frame`, {
  method: "POST",
  headers: { "Content-Type": "image/jpeg" },
  body: jpegBuffer,
});
```

Each viewport is bound to exactly one camera (1:1 in v1). Multi-camera cycling is out of scope.

Scrypted-initiated session (camera event):

```ts
await fetch(`${v.url}/state`, {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ state: "wake" }),
});  // device shows loading screen

// then push frames until the per-stream timeout elapses:
for (const jpeg of frames) {
  const r = await fetch(`${v.url}/frame`, {
    method: "POST",
    headers: { "Content-Type": "image/jpeg" },
    body: jpeg,
  });
  if (r.status === 409) break;  // device went to sleep; stop the loop
}
```

Callback handler at `/api/viewport/touch` (body: `{display, state, event}`):

- `state=wake` → look up viewport→camera binding, start streaming. No need to `POST /state` first; device is already awake.
- `state=sleep` → stop streaming to that viewport.

Both handlers are idempotent. Don't track viewport state. Apply a Scrypted-side per-stream timeout using the same `idle_timeout_ms` sent in `/config`, so streams end even if the sleep callback is dropped. Timers run independently — and if the device sleeps first (idle or tap), the next `/frame` returns `409` and Scrypted stops on that signal alone.

---

## 13. Development Milestones

### Milestone 1: Board Bring-Up

- Build ESP-IDF project.
- Flash ESP32-P4.
- Serial logging works.
- Ethernet DHCP works.
- Device prints IP.

Acceptance:

```text
Device gets DHCP lease over Ethernet.
```

### Milestone 2: HTTP + mDNS

- Start HTTP server.
- Implement `GET /state`.
- Advertise `_scrypted-viewport._tcp.local` with TXT records.

Acceptance:

```bash
curl http://viewport.local/state
```

returns JSON with `state: "unconfigured"` on a fresh device.

### Milestone 3: Display Bring-Up

- Initialize DSI display.
- Turn backlight on/off.
- Show color bars/test pattern.

Acceptance:

```text
Screen displays deterministic test pattern.
```

### Milestone 4: Config Persistence

Done before any state-bearing endpoints so they can read values from NVS instead of working around hardcoded defaults.

- Implement `GET /config` and `POST /config` (display, callback, idle_timeout_ms, orientation, brightness).
- Partial-update semantics on `POST /config`: only included fields are written.
- Validate per spec (non-empty display, http callback, idle_timeout = 0 or ≥ 5000, orientation in {portrait, landscape}, brightness 0–100).
- Persist all fields to NVS atomically.
- Apply orientation and brightness immediately (mDNS TXT and `/state` reflect them).
- Brightness defaults to 80 on first boot.

Acceptance:

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"display":"mudroom","callback":"http://host/cb","orientation":"landscape"}' \
  http://viewport.local/config

# Partial update — only brightness changes:
curl -X POST -H "Content-Type: application/json" \
  -d '{"brightness":50}' \
  http://viewport.local/config

curl http://viewport.local/config
```

After reboot, `GET /state` shows `configured=true` and name preserved; `GET /config` shows `orientation=landscape`, `brightness=50`, and the original callback.

### Milestone 5: JPEG Frame Push

- Implement `/frame`.
- Receive JPEG body, decode into framebuffer, push to panel with current orientation applied.
- Expected dimensions = effective resolution from M4.

Acceptance (default portrait, so test image is 480×800):

```bash
curl -X POST \
  -H "Content-Type: image/jpeg" \
  --data-binary @test-480x800.jpg \
  http://viewport.local/frame
```

updates screen. Re-running with `landscape` set via `/config` requires an 800×480 test image.

### Milestone 6: State + Idle Timer

- Implement `POST /state` (`wake` / `sleep`, idempotent).
- Make `/frame` reject with `409` when asleep — no auto-wake.
- Add idle timer using `idle_timeout_ms` from NVS; on expiry, transition to sleep and POST `state=sleep event=timeout` callback (callback target is a no-op until M7).

Acceptance:

```bash
curl -X POST -d '{"state":"sleep"}' http://viewport.local/state  # backlight off
curl -X POST -d '{"state":"wake"}'  http://viewport.local/state  # backlight on, loading
```

`/frame` after `state=sleep` returns 409. `/frame` after `state=wake` paints. Idle timer fires after `idle_timeout_ms` of no frames.

### Milestone 7: Touch Callback

- Initialize touch controller.
- Detect tap; toggle wake/sleep locally.
- POST callback JSON for `state=wake event=tap`, `state=sleep event=tap`, and `state=sleep event=timeout`.

Acceptance:

```text
Tap on asleep device: backlight on, callback {state:wake,event:tap}.
Tap on awake device: backlight off, callback {state:sleep,event:tap}.
After idle_timeout_ms with no /frame: callback {state:sleep,event:timeout}.
```

### Milestone 8: Local Screens + BOOT button

- Embed minimal bitmap font.
- Render IP screen on boot when NVS has no callback (persistent).
- Render loading screen on every wake (via tap or `POST /state`); replaced by next `/frame`.
- Wire BOOT short-press to 15s IP-screen overlay (no state change).
- Wire BOOT 5s-hold to NVS-clear + reboot.

Acceptance:

```text
Fresh device boots to IP screen.
Tap (or POST /state {state:wake}) shows "Loading…" until next /frame.
BOOT short-press overlays IP for 15s, then restores prior state.
BOOT 5s-hold returns the device to the IP screen.
```

### Milestone 9: Live Stream (`POST /stream`)

The first post-`/frame` enhancement. Once `/frame` works end-to-end, add a streaming endpoint to lift the per-frame HTTP overhead and reach live frame rates.

- Implement `POST /stream`, `Content-Type: multipart/x-mixed-replace; boundary=…`.
- Read chunked body, parse multipart boundaries, hand each part to the JPEG decoder.
- Reset the idle timer on each part (not just connection open).
- On `POST /state {state:sleep}`, tap-to-sleep, or idle timeout: close the stream connection (signals Scrypted to stop).
- Scrypted side moves from a Script polling `takePicture()` to a small plugin using `MediaManager` + FFmpeg to pipe MJPEG.

`/frame` stays — useful forever for snapshots, doorbell stills, and curl-driven debug.

Acceptance:

```text
Live camera stream renders on viewport at >= 10 fps.
Closing the stream connection from either side stops the session cleanly.
```

---

## 14. Test Images

Generate test images at the effective resolution (480×800 for the default portrait orientation, 800×480 for landscape):

- solid red
- solid green
- solid blue
- grayscale gradient
- camera-like JPEG
- text overlay rendered by host

Use these to validate color order, scaling, orientation, and full-screen update.

If colors are wrong, likely RGB/BGR or RGB565 byte-order issue.

---

## 15. Error Handling Philosophy

Keep failure behavior simple. Every endpoint is idempotent and every failure leaves the device in a sane state.

Frame decode fails:

- increment `decode_errors`
- return 400 or 500
- keep previous frame on screen
- do not change wake/sleep state

Callback POST fails:

- log
- increment `callback_failures`
- continue — local state change still happens
- do not retry; Scrypted catches up via its own timeout or the next event

Ethernet disconnects:

- keep display running
- reconnect automatically
- no reboot loop

Display init fails:

- log loudly
- continue serving `GET /state` with `state="unconfigured"` if possible

Watchdog:

- ESP-IDF task watchdog enabled on the main HTTP task and the touch task. If either hangs, the device reboots and rebuilds state from NVS.

Status LED (on-board):

- solid = configured & online
- slow blink = unconfigured, waiting for `/config`
- fast blink = no network
- Drives the LED on the Waveshare board; visible when the screen is asleep.

---

## 16. Security Model

No authentication.

Assumption:

```text
Scrypted Viewport runs only on trusted private LAN/VLAN.
```

Do not add auth, pairing, TLS, users, or passwords.

If future security is needed, solve it at the network layer.

---

## 17. Coding Standards

- Prefer C with ESP-IDF APIs.
- Keep modules small.
- Avoid dynamic allocation in hot paths after startup where practical.
- Use PSRAM explicitly for large buffers.
- Log meaningful errors.
- Avoid clever abstractions.
- Avoid generalized display framework unless necessary.
- Keep all constants obvious and centralized.

---

## 18. Acceptance Criteria for v2

The project is successful when:

1. Device powers from PoE.
2. Device gets DHCP over Ethernet.
3. Device advertises mDNS with TXT records (`version`, `resolution`, `orientation`, `name`).
4. `GET /state` returns runtime state, frame counters, and error counters.
5. `GET /config` returns the persisted config (with defaults filled in before first `/config`).
6. `POST /config` persists display, callback, idle timeout, orientation, and brightness across reboot, with partial-update semantics.
7. `POST /config` validates `idle_timeout_ms` (0 disables; non-zero ≥ 5000; else 400), `orientation` (`portrait` or `landscape`; else 400), and `brightness` (0–100; else 400). Defaults on first boot: `orientation=portrait`, `brightness=80`, `idle_timeout_ms=60000`.
8. `POST /state` transitions wake↔sleep idempotently and rejects unknown state values with 400.
9. `POST /frame` paints when awake, returns 409 when asleep, and never changes state.
10. Brightness PWM is gamma-corrected (perceptual 0–100).
11. Idle timer fires `state=sleep event=timeout` callback after `idle_timeout_ms` of no `/frame`.
12. Tap toggles wake/sleep locally and POSTs `state=wake event=tap` or `state=sleep event=tap`.
13. Unconfigured device shows its IP and `viewport.local` on screen.
14. Loading screen is shown on every wake until the next `/frame` arrives.
15. BOOT short-press overlays IP screen for 15s with no state change. BOOT 5s-hold factory-resets.
16. Watchdog reboots a hung device; state recovers from NVS.
17. Status LED indicates network and configuration state.
18. Firmware has no Matter, HomeKit, business logic, or config portal. The only locally-rendered UI is the IP screen and the loading screen.

---

## 19. Guiding Rule

When deciding where to implement a feature:

```text
If it changes what should be shown, it belongs in Scrypted.
If it changes how pixels reach the screen, it belongs in Scrypted Viewport.
```

Scrypted Viewport receives pixels, displays pixels, and reports touch.

Everything else belongs outside the ESP.
