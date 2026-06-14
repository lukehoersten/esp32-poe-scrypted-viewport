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
  -> initialize Ethernet
  -> DHCP
  -> initialize mDNS
  -> initialize display/backlight/touch
  -> start HTTP server
  -> advertise _scrypted-viewport._tcp.local
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

Touch flow:

```text
user taps display
  -> firmware detects gesture
  -> POST JSON event to configured callback URL
  -> Scrypted decides what to do
  -> Scrypted may POST a new frame/sleep/brightness
```

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
  "callback": "http://scrypted.local:11080/api/viewport/touch"
}
```

Behavior:

- Store display name.
- Store callback URL.
- Persist to NVS so it survives reboot.
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
tap
long_press
swipe_left
swipe_right
```

Rules:

- Scrypted Viewport does not interpret events.
- Scrypted Viewport does not switch cameras.
- Scrypted Viewport does not decide whether to stream.
- Scrypted owns behavior.

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
- Detect basic gestures:
  - tap
  - long press
  - swipe left
  - swipe right
- Debounce.
- Send events to callback client.

Gesture accuracy does not need to be perfect. The primary event is tap.

### 8.8 `callback_client.c`

Responsibilities:

- POST touch JSON to configured callback URL.
- Fast timeout.
- Best effort.
- Do not retry indefinitely.

### 8.9 `idle_timer.c`

Responsibilities:

- Reset timer on `/frame`.
- Turn off backlight after timeout.
- Default idle timeout can be hardcoded initially, e.g. 30 seconds.
- Future `/config` may add timeout, but not required now.

### 8.10 `nvs_config.c`

Responsibilities:

Persist:

- display name
- callback URL
- brightness

Optional:

- idle timeout

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

Scrypted script owns a static display list:

```ts
const displays = [
  "http://viewport-mudroom.local",
  "http://viewport-kitchen.local",
];
```

Registration on startup:

```ts
await fetch(`${display}/config`, {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({
    display: "mudroom",
    callback: "http://scrypted.local:11080/api/viewport/touch"
  })
});
```

Frame push:

```ts
await fetch(`${display}/frame`, {
  method: "POST",
  headers: { "Content-Type": "image/jpeg" },
  body: jpegBuffer
});
```

Touch callback handler decides:

- latest snapshot
- short live preview
- cycle page/camera
- sleep

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
12. Firmware has no Matter, HomeKit, polling, local UI rendering, or config portal.

---

## 19. Guiding Rule

When deciding where to implement a feature:

```text
If it changes what should be shown, it belongs in Scrypted.
If it changes how pixels reach the screen, it belongs in Scrypted Viewport.
```

Scrypted Viewport receives pixels, displays pixels, and reports touch.

Everything else belongs outside the ESP.
