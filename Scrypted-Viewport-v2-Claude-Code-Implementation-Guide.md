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

Native resolution:

```text
800x480
```

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

Frame flow:

```text
Scrypted renders 800x480 JPEG
  -> HTTP POST /frame
  -> Scrypted Viewport receives raw JPEG
  -> decode JPEG into framebuffer/display buffer
  -> push to DSI display
  -> turn on backlight
  -> reset idle timer
```

Wake/sleep model:

Wake and sleep couple the backlight with Scrypted's frame stream. The two move together, never independently.

```text
wake  = backlight on  + Scrypted streaming frames
sleep = backlight off + Scrypted not streaming
```

Touch flow:

```text
tap while asleep:
  backlight on
  render loading screen
  POST {"event":"wake"} -> Scrypted starts streaming
  next /frame replaces loading screen

tap while awake:
  backlight off
  POST {"event":"sleep"} -> Scrypted stops streaming

idle timer fires (60s with no /frame):
  backlight off
  POST {"event":"sleep"} -> Scrypted stops streaming
```

The callback events are `wake` and `sleep`; `tap` itself is internal. The events are idempotent imperatives — "start streaming" / "stop streaming" — not state notifications. Scrypted does not track per-viewport state; it acts on the event and forgets.

Scrypted-initiated `/sleep` and `/frame` do not echo a callback.

Only `tap` is detected on the touchscreen. Long-press and swipes are out of scope for v1.

BOOT button:
- Short press: overlay IP screen for 15s, then return to prior state. Wakes backlight temporarily but does not change wake/sleep state and does not POST a callback.
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

### 5.1 GET /health

Return JSON status.

Example response:

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

Status code:

```text
200 OK
```

### 5.2 POST /config

Registers the controlling Scrypted callback.

Request body:

```json
{
  "display": "mudroom",
  "callback": "http://scrypted.local:11080/api/viewport/touch",
  "idle_timeout_ms": 60000
}
```

Behavior:

- Store display name.
- Store callback URL.
- Store idle timeout (optional in body, default 60000).
- Persist all to NVS so it survives reboot.
- May be called repeatedly.
- Replaces old config atomically.

Response:

```text
204 No Content
```

Validation:

- `display` must be non-empty.
- `callback` must be HTTP URL.
- HTTPS is not required for v1/v2.

### 5.3 POST /frame

Displays a full-frame JPEG.

Headers:

```text
Content-Type: image/jpeg
```

Body:

```text
raw JPEG bytes
```

Expected image:

```text
800x480 JPEG
```

Behavior:

- Accept raw JPEG body.
- Decode.
- Display.
- Turn on backlight.
- Reset idle timer.
- Return once frame is accepted/displayed.

Response:

```text
204 No Content
```

Failure responses:

```text
400 Bad Request        invalid content-type or malformed JPEG
413 Payload Too Large  JPEG exceeds configured maximum
500 Internal Error     decode/display failure
```

Recommended max JPEG size:

```text
1 MB
```

Implementation note:

Scrypted is responsible for scaling/cropping/composition. Scrypted Viewport should not scale images in v1/v2.

### 5.4 POST /sleep

Turns off the backlight.

Behavior:

- Backlight off.
- Preserve framebuffer.
- Device remains online and ready for `/frame`.

Response:

```text
204 No Content
```

### 5.5 POST /brightness

Sets backlight brightness.

Request:

```json
{
  "brightness": 75
}
```

Range:

```text
0-100
```

Behavior:

- Clamp or reject out-of-range values. Prefer rejecting invalid values with 400.
- Persist latest brightness to NVS.
- If display is awake, apply immediately.
- If asleep, store value for next wake.

Response:

```text
204 No Content
```

---

## 6. Touch Callback Contract

Scrypted Viewport initiates HTTP POSTs to the configured callback URL.

Example request body:

```json
{
  "display": "mudroom",
  "event": "tap",
  "timestamp": 1730000000
}
```

Supported event names:

```text
wake
sleep
```

`tap` is the input the touchscreen detects; the callback event is the resulting state (`wake` or `sleep`). Idle-timer-driven sleep also sends `sleep`.

Rules:

- The device owns wake/sleep state. Scrypted does not track it.
- Scrypted owns the viewport→camera binding and decides which stream goes to which viewport.
- `wake`/`sleep` callbacks are idempotent imperatives: "start streaming" / "stop streaming". Scrypted acts on receipt and forgets.
- Scrypted enforces its own per-stream timeout independently of the device's idle timer. Either can end a session, whichever notices first.
- Scrypted-initiated `/sleep` and `/frame` do not echo a callback.

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
- Include TXT records if easy:

```text
version=1.0.0
resolution=800x480
name=mudroom
```

### 8.4 `http_api.c`

Responsibilities:

- Register routes:
  - `GET /health`
  - `POST /config`
  - `POST /frame`
  - `POST /sleep`
  - `POST /brightness`
- Parse JSON for config/brightness.
- Stream JPEG request body into buffer.
- Call display/JPEG functions.
- Return simple status codes.

### 8.5 `display.c`

Responsibilities:

- Initialize MIPI DSI display.
- Initialize framebuffer or draw buffer.
- Control backlight.
- Push decoded frames to display.
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
  - If asleep: transition to wake — backlight on, render loading screen, POST `{"event":"wake"}`.
  - If awake: transition to sleep — backlight off, POST `{"event":"sleep"}`.
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

- Reset timer on `/frame` and on tap-driven wake.
- On expiry: transition to sleep (backlight off, POST `{"event":"sleep"}`).
- Idle timeout: read from NVS (`idle_timeout_ms`), default 60000 ms. Set via `/config`. Scrypted is expected to use the same value as its own per-stream cutoff, but timers run independently.

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
- text rendering

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
      idle_timeout_ms: 60000,
    }),
  });
}
```

Stream a session of frames to a viewport — triggered by either a camera event (doorbell, person, motion) or a `wake` callback from the viewport:

```ts
await fetch(`${v.url}/frame`, {
  method: "POST",
  headers: { "Content-Type": "image/jpeg" },
  body: jpegBuffer,
});
```

Callback handler at `/api/viewport/touch`:

- `wake` → look up viewport→camera binding, start streaming.
- `sleep` → stop streaming to that viewport.

Both are idempotent. Don't track viewport state. Apply a Scrypted-side per-stream timeout using the same `idle_timeout_ms` value sent in `/config`, so streams end even if the `sleep` callback is dropped. Timers run independently on each side.

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
- Implement `/health`.
- Advertise `_scrypted-viewport._tcp.local`.

Acceptance:

```bash
curl http://viewport.local/health
```

returns JSON.

### Milestone 3: Display Bring-Up

- Initialize DSI display.
- Turn backlight on/off.
- Show color bars/test pattern.

Acceptance:

```text
Screen displays deterministic test pattern.
```

### Milestone 4: JPEG Frame Push

- Implement `/frame`.
- Receive JPEG body.
- Decode into framebuffer.
- Display full frame.

Acceptance:

```bash
curl -X POST \
  -H "Content-Type: image/jpeg" \
  --data-binary @test-800x480.jpg \
  http://viewport.local/frame
```

updates screen.

### Milestone 5: Sleep/Brightness

- Implement `/sleep`.
- Implement `/brightness`.
- Add idle timer.

Acceptance:

```bash
curl -X POST http://viewport.local/sleep
```

turns off backlight.

### Milestone 6: Config Persistence

- Implement `/config`.
- Persist callback/name in NVS.
- Confirm survives reboot.

Acceptance:

```text
After reboot, /health shows configured=true and name preserved.
```

### Milestone 7: Touch Callback

- Initialize touch controller.
- Detect tap.
- POST callback JSON.

Acceptance:

```text
Tap generates callback to test HTTP endpoint.
```

### Milestone 8: Local Screens

- Embed minimal bitmap font.
- Render unconfigured screen (IP + viewport.local) on boot when NVS has no callback.
- Render loading screen on tap-wake; replaced by next `/frame`.

Acceptance:

```text
Fresh device boots to an IP-display screen.
Tap on a sleeping configured device shows "Loading…" until next frame.
```

---

## 14. Test Images

Generate test images at exactly 800×480:

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

Keep failure behavior simple.

If frame decode fails:

- increment decode error counter
- return 400 or 500
- keep previous frame on screen

If callback fails:

- log
- increment counter
- continue

If Ethernet disconnects:

- keep display running
- reconnect automatically
- no reboot loop

If display init fails:

- log loudly
- continue serving `/health` if possible with display error status

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
3. Device advertises mDNS.
4. `/health` works.
5. `/config` persists display name and callback URL.
6. `/frame` accepts an 800×480 JPEG and updates screen.
7. `/frame` implicitly wakes display.
8. Idle timeout turns backlight off.
9. `/sleep` works.
10. `/brightness` works.
11. Tap sends callback JSON to Scrypted/test server.
12. Unconfigured device shows its IP on screen so it can be registered from Scrypted.
13. Tap toggles the device between wake (backlight on + Scrypted streaming) and sleep (backlight off + Scrypted not streaming), via `wake`/`sleep` callback events.
14. Idle timer fires `sleep` after `idle_timeout_ms` with no `/frame` (default 60s, set by Scrypted via `/config`).
15. BOOT short-press overlays the IP screen for 15s; BOOT hold ≥5s factory-resets.
16. Firmware has no Matter, HomeKit, polling, business logic, or config portal. The only locally-rendered UI is the IP screen and the loading screen.

---

## 19. Guiding Rule

When deciding where to implement a feature:

```text
If it changes what should be shown, it belongs in Scrypted.
If it changes how pixels reach the screen, it belongs in Scrypted Viewport.
```

Scrypted Viewport receives pixels, displays pixels, and reports touch.

Everything else belongs outside the ESP.
