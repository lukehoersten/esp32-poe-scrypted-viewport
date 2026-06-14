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
  -> load saved config (viewport name, scrypted URL, brightness, idle timeout, orientation)
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
tap while asleep                     -> awake  (device POSTs <scrypted>/state {viewport,wake})
tap while awake                      -> asleep (device POSTs <scrypted>/state {viewport,sleep})
idle timer expires                   -> asleep (device POSTs <scrypted>/state {viewport,sleep})
POST /state {"state":"wake"} asleep  -> awake  (no outbound POST)
POST /state {"state":"sleep"} awake  -> asleep (no outbound POST)
```

`/frame` never changes state. It paints if awake, returns `409` if asleep. This eliminates the race where a frame in flight could re-wake a device that just slept on a tap.

Scrypted-initiated state changes via `POST /state` do not produce an outbound POST from the device.

Frame flow (awake state only):

```text
Scrypted POST /frame
  -> Scrypted Viewport receives raw JPEG
  -> decode JPEG into framebuffer
  -> push to DSI display
  -> reset idle timer
  -> respond 204
```

Callback body has one field:
- `state`: `wake` or `sleep` — the resulting state and the imperative for Scrypted.

Callbacks are idempotent imperatives — "start streaming" / "stop streaming" — not state notifications. Scrypted does not track per-viewport state. Source identification is via request source IP, not body.

Only `tap` is detected on the touchscreen. Long-press and swipes are out of scope for v1.

No wall-clock timestamps. The device has no RTC and no SNTP.

BOOT button:
- Short press: overlay IP screen for 15s, then return to prior state. Wakes backlight for the overlay but does not change wake/sleep state and does not POST to Scrypted. Incoming `/frame` during the overlay is rejected `409`.
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
  "state_post_failures": 2,
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
  "viewport": "mudroom",
  "scrypted": "http://scrypted.local:11080/endpoint/scrypted-viewport",
  "idle_timeout_ms": 60000,
  "orientation": "portrait",
  "brightness": 80
}
```

Before first `/config`: `viewport` and `scrypted` are `null`; the rest carry first-boot defaults (`idle_timeout_ms: 60000`, `orientation: "portrait"`, `brightness: 80`).

Status code: `200 OK`.

### 5.3 POST /config

Sets or updates the device config. Partial-update semantics — only fields present in the body are changed; omitted fields keep their current values. The merged config is persisted to NVS atomically.

Request body (full form):

```json
{
  "viewport": "mudroom",
  "scrypted": "http://scrypted.local:11080/endpoint/scrypted-viewport",
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

- `viewport`: non-empty string. Drives the mDNS hostname (`viewport-<name>.local`).
- `scrypted`: must be `http://...`. This is the Scrypted plugin's base URL. The device will POST state changes to `<scrypted>/state`.
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
- Do not POST to Scrypted (Scrypted initiated this).
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

## 6. Device → Scrypted `POST /state`

The device and Scrypted are HTTP peers; both expose `POST /state` with the same body shape. When the device changes state on its own (tap or idle timer), it POSTs to Scrypted's `/state` endpoint. There is no callback / ack semantic — just two REST servers calling each other.

The `scrypted` base URL is set via `/config`. Outbound URL is `<scrypted>/state`.

### No application-level ack

HTTP 2xx is transport-level only. The device does not retry, does not block subsequent state changes on the response, and does not treat 5xx as anything more than a counter increment. Idempotency + `/frame` returning `409` + each side's independent idle timer recover every failure mode. Do not write Scrypted-side logic that waits for the device to confirm a state change — there is no confirmation.

### Request

```
POST <scrypted>/state HTTP/1.1
Host: <derived from URL>
Content-Type: application/json
Content-Length: <body length>
User-Agent: ScryptedViewport/<version>
Connection: close
```

Body:

```json
{ "viewport": "mudroom", "state": "wake" }
```

or

```json
{ "viewport": "mudroom", "state": "sleep" }
```

- `viewport` (string): the value of the `viewport` field in the device's `/config`. Scrypted's routing key.
- `state` (string): `wake` or `sleep`. The resulting state and the imperative for Scrypted.

No other fields. No wall-clock timestamp (no RTC, no SNTP).

### Expected response

- Any 2xx is success.
- Body is ignored.
- Anything else (non-2xx, connection refused, DNS failure, request timeout) increments `state_post_failures` and is otherwise ignored.

### Timeouts

- Connect timeout: 1 second.
- Total request timeout: 1 second.
- After timeout the device aborts the connection and moves on.

### Concurrency and ordering

- At most one outbound `/state` POST in flight at a time. Use a single dedicated worker task so HTTP I/O does not block the display path.
- POSTs are delivered in the order state changes occur on the device.
- If a state change happens while a POST is in flight, the new POST goes into a depth-1 queue. If the queue already holds a POST, the queued entry is **replaced** by the newer one. The in-flight POST is never cancelled.
- Replacement is safe because POSTs are imperatives — only the latest desired state matters to Scrypted. Intermediate flips collapse to the final state.

### Failure semantics

- The local state change always happens regardless of POST outcome. The POST is a hint, not a confirmation.
- A dropped or failed POST is recovered by one of: the next user tap, the device's idle timer firing `sleep`, or the next `/frame` from Scrypted returning `409`.
- No retry queue. No backoff. No persistence across reboots.

### When the device does NOT POST

- Before `/config` registers a `scrypted` URL (boot state, factory reset) — silently dropped.
- For state changes initiated via the API (`POST /state`, `POST /frame` while asleep). Scrypted already knows; echoing would loop.
- BOOT short-press IP overlay (not a state change).

### Rules summary

- The device owns wake/sleep state. Scrypted does not track it.
- Scrypted owns the viewport→camera binding and decides which stream goes to which viewport.
- `POST /state` (in either direction) is an idempotent imperative. The recipient acts and forgets.
- Scrypted enforces its own per-stream timeout independently of the device's idle timer. Either can end a session, whichever notices first.
- Plain HTTP, no TLS, no auth — same trust model as inbound. LAN-only.

### Race handling

Wake/sleep changes can race: a user taps the device while Scrypted is mid-flight with a `POST /state` from a stale camera-event timeout, or the idle timer fires at the same instant Scrypted POSTs a fresh `wake`. The protocol does NOT carry epochs, session IDs, or priorities. Race resolution is purely about each side serializing its own writes and trusting idempotency to converge.

1. **Device-side serialization.** Guard the state-mutation function with a mutex. Tap, idle timer, and `POST /state` all funnel through it. Whichever lands second wins.
2. **Scrypted-side serialization.** Scrypted handles inbound `POST /state` requests for a given `viewport` one at a time (in-process queue per viewport). Whichever lands second wins.
3. **Last write wins.** No priorities. A stale `sleep` landing after a fresh `wake` sleeps the device; the user taps again and we're back. One extra tap is cheap.
4. **Scrypted must cancel its own pending operations on each inbound POST.** On `wake`, cancel any pending per-viewport sleep timer before starting a fresh stream. Same in reverse. This makes "stale Scrypted timer fires after the user tapped to wake" impossible without protocol-level epochs.

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

    state_client.h
    state_client.c

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

The device's mDNS responder. The ESP-IDF `mdns` component serves all `.local` records for this host directly — no external DNS server is involved. Scrypted discovers viewports by browsing the service; it does NOT depend on OS-level `.local` hostname resolution.

Responsibilities:

- `mdns_init()` once at boot.
- `mdns_hostname_set("viewport")` pre-config, `mdns_hostname_set("viewport-<name>")` after `/config` sets `viewport`. The component answers A-record queries for `<hostname>.local`.
- `mdns_service_add(NULL, "_scrypted-viewport", "_tcp", 80, NULL, 0)` to advertise the service on port 80. The component answers the PTR + SRV queries for the browse.
- `mdns_service_txt_set("_scrypted-viewport", "_tcp", txt_items, n)` with:

```text
version=1.0.0
resolution=<effective>    (480x800 or 800x480)
orientation=<portrait|landscape>
name=<viewport name>
```

- Update the hostname and TXT records when `viewport` or `orientation` changes via `/config`.

Scrypted-side discovery uses a Node mDNS-SD library (`bonjour-service`, `mdns`, etc.). Browse results carry the device's current IP from the SRV/A records — Scrypted uses that IP directly for all subsequent `/config`, `/state`, `/frame` calls and does not perform a separate hostname lookup.

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
  - If asleep: transition to awake — backlight on, render loading screen, POST `{"viewport":"<name>","state":"wake"}`.
  - If awake: transition to asleep — backlight off, POST `{"viewport":"<name>","state":"sleep"}`.
- Also handle BOOT button:
  - Short press: ask `local_screens` to overlay the IP screen for 15s, then restore prior state. No outbound POST.
  - Hold ≥5s: clear NVS via `nvs_config_reset()` and reboot.

### 8.8 `state_client.c`

The HTTP client that POSTs the device's state changes to Scrypted's `/state`. Pair to `http_api.c` (which serves the device's own `/state`).

Responsibilities:

- Single dedicated worker task. At most one HTTP POST in flight.
- Depth-1 queue for the next POST. If the slot is occupied when a new state change happens, replace the queued entry with the newer one (do not cancel the in-flight POST).
- POST JSON body `{"viewport": "<name>", "state": "wake"|"sleep"}` to `<scrypted>/state` (where `<scrypted>` is the configured base URL) with `Content-Type: application/json`, `User-Agent: ScryptedViewport/<version>`, `Connection: close`.
- Connect timeout 1s, total request timeout 1s. Abort on timeout.
- Treat any 2xx as success. Increment `state_post_failures` on anything else (non-2xx, connection refused, DNS failure, timeout) and continue.
- Never block the display, frame, or touch paths.
- No retries. No backoff. No persistence across reboot.

Callers fire-and-forget to the worker. Local state changes proceed immediately regardless of POST outcome.

### 8.9 `idle_timer.c`

Responsibilities:

- Reset timer on `/frame`, `POST /state {state:wake}`, and tap-driven wake.
- On expiry: transition to asleep (backlight off, POST `{"viewport":"<name>","state":"sleep"}`).
- Idle timeout: read from NVS (`idle_timeout_ms`), default 60000 ms. `0` disables the timer; non-zero values must be ≥ 5000 ms (validated at `/config`).
- Scrypted is expected to use the same value as its own per-stream cutoff, but timers run independently — either side can end a session, whichever notices first.

### 8.10 `local_screens.c`

The only application UI the firmware draws. Two screens, both via a small embedded bitmap font (no LVGL, no general text engine).

Responsibilities:

- `local_screens_show_ip(ip)` — centered IP address and `viewport.local`. Shown:
  - on boot when no `scrypted` URL is in NVS (persistent until `/config` arrives), and
  - as a 15s overlay on BOOT short-press (then restore prior state).
- `local_screens_show_loading()` — centered "Loading…" text. Shown on wake until the next `/frame` lands.
- Render into the same RGB565 framebuffer the JPEG decoder targets, then push to the panel.

Keep it small. Hard-code the font glyphs for digits, dots, colons, and a-z. No string formatting library — write tiny inline blitters.

### 8.11 `nvs_config.c`

Responsibilities:

Persist:

- viewport name
- scrypted base URL
- brightness
- idle timeout (ms)
- orientation

Do not persist frame data.

---

## 9. State Model

Global state should be small:

```c
typedef struct {
    char viewport_name[64];
    char scrypted_url[256];

    bool configured;
    bool awake;

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

Scrypted owns a list of viewport-to-camera bindings:

```ts
const bindings = [
  { name: "mudroom", camera: "frontDoor" },
  { name: "kitchen", camera: "driveway" },
];
```

The binding lives in Scrypted config. The device knows nothing about cameras.

### Discovery

Scrypted finds viewports via mDNS service discovery — not by hardcoded hostname. Use a Node mDNS-SD library that hits the multicast layer directly, so OS-level `.local` resolution is not required:

```ts
import { Bonjour } from "bonjour-service";

const bonjour = new Bonjour();
const viewports = new Map<string, { ip: string; port: number }>();  // name -> address

bonjour.find({ type: "scrypted-viewport" }, (svc) => {
  const name = svc.txt?.name;
  const ip = svc.addresses?.find(a => !a.includes(":"));  // prefer IPv4
  if (name && ip) viewports.set(name, { ip, port: svc.port });
});

// Re-browse periodically to catch DHCP renumbering. The library typically
// also emits 'down' events when a device disappears; honor those too.
setInterval(() => bonjour.find({ type: "scrypted-viewport" }), 5 * 60 * 1000);
```

Resolve an IP for each binding when calling the device:

```ts
function urlFor(name: string): string {
  const v = viewports.get(name);
  if (!v) throw new Error(`viewport ${name} not discovered yet`);
  return `http://${v.ip}:${v.port}`;
}
```

If mDNS-SD is not available in the deployment (some Docker setups, VLAN edge cases), allow the operator to set an explicit `http://<ip>:<port>` per viewport in plugin config as a fallback — same shape as the discovered entry.

### Registration

Registration on startup. The plugin's HTTP root is the `scrypted` base URL the device will POST to (with `/state` appended):

```ts
const SCRYPTED_BASE = "http://scrypted.local:11080/endpoint/scrypted-viewport";

for (const b of bindings) {
  await fetch(`${urlFor(b.name)}/config`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      viewport: b.name,             // e.g. "mudroom" — routing key in device's outbound /state POSTs
      scrypted: SCRYPTED_BASE,      // device will POST <scrypted>/state
      idle_timeout_ms: 60000,        // both sides use this independently
      orientation: "portrait",       // override per viewport if wall-mounted sideways
      brightness: 80,
    }),
  });
}
```

Each viewport is bound to exactly one camera (1:1 in v1). Multi-camera cycling is out of scope.

Scrypted-initiated session (camera event):

```ts
await fetch(`${urlFor(b.name)}/state`, {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ state: "wake" }),
});  // device shows loading screen

// then push frames until the per-stream timeout elapses:
for (const jpeg of frames) {
  const r = await fetch(`${urlFor(b.name)}/frame`, {
    method: "POST",
    headers: { "Content-Type": "image/jpeg" },
    body: jpeg,
  });
  if (r.status === 409) break;  // device went to sleep; stop the loop
}
```

Handler at `POST <SCRYPTED_BASE>/state` (body: `{viewport, state}`):

```ts
const { viewport, state } = req.body;
const b = bindings.find(x => x.name === viewport);
if (!b) return res.status(404).end();

cancelPendingSleep(b);     // race rule: cancel stale timers on every inbound POST

if (state === "wake")  startStream(b);   // idempotent
if (state === "sleep") stopStream(b);    // idempotent

res.status(204).end();
```

Apply a Scrypted-side per-stream timeout using the same `idle_timeout_ms` sent in `/config`, so streams end even if the device's outbound sleep POST is dropped. Timers run independently — and if the device sleeps first (idle or tap), the next `/frame` returns `409` and Scrypted stops on that signal alone.

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
- Initialize mDNS responder. Advertise `_scrypted-viewport._tcp.local` on port 80 with TXT records (`version`, `resolution`, `orientation`, `name`).

Acceptance:

```bash
# OS-level mDNS resolution (works on macOS by default; requires nss-mdns on Linux):
curl http://viewport.local/state

# Or service-discovery browse, which does NOT require OS-level resolution:
dns-sd -B _scrypted-viewport._tcp local.   # macOS
avahi-browse -r _scrypted-viewport._tcp    # Linux
```

`GET /state` returns JSON with `state: "unconfigured"` on a fresh device. The service browse shows the device with its current IP. Scrypted-side discovery should use the browse, not the hostname.

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

- Implement `GET /config` and `POST /config` (viewport, scrypted, idle_timeout_ms, orientation, brightness).
- Partial-update semantics on `POST /config`: only included fields are written.
- Validate per spec (non-empty viewport, http scrypted URL, idle_timeout = 0 or ≥ 5000, orientation in {portrait, landscape}, brightness 0–100).
- Persist all fields to NVS atomically.
- Apply orientation and brightness immediately (mDNS TXT and `/state` reflect them).
- Brightness defaults to 80 on first boot.

Acceptance:

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"viewport":"mudroom","scrypted":"http://host/endpoint/scrypted-viewport","orientation":"landscape"}' \
  http://viewport.local/config

# Partial update — only brightness changes:
curl -X POST -H "Content-Type: application/json" \
  -d '{"brightness":50}' \
  http://viewport.local/config

curl http://viewport.local/config
```

After reboot, `GET /state` shows `configured=true` and name preserved; `GET /config` shows `orientation=landscape`, `brightness=50`, and the original `scrypted` URL.

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
- Add idle timer using `idle_timeout_ms` from NVS; on expiry, transition to sleep and POST `{"viewport":"<name>","state":"sleep"}` to `<scrypted>/state` (target is a no-op until M7).

Acceptance:

```bash
curl -X POST -d '{"state":"sleep"}' http://viewport.local/state  # backlight off
curl -X POST -d '{"state":"wake"}'  http://viewport.local/state  # backlight on, loading
```

`/frame` after `state=sleep` returns 409. `/frame` after `state=wake` paints. Idle timer fires after `idle_timeout_ms` of no frames.

### Milestone 7: Touch Callback

- Initialize touch controller.
- Detect tap; toggle wake/sleep locally.
- POST `{"viewport":"<name>","state":"wake"|"sleep"}` to `<scrypted>/state` via the `state_client` worker (depth-1 queue, 1s timeout).

Acceptance:

```text
Tap on asleep device: backlight on; outbound POST {viewport,state:wake} to <scrypted>/state.
Tap on awake device: backlight off; outbound POST {viewport,state:sleep} to <scrypted>/state.
After idle_timeout_ms with no /frame: outbound POST {viewport,state:sleep}.
```

### Milestone 8: Local Screens + BOOT button

- Embed minimal bitmap font.
- Render IP screen on boot when NVS has no `scrypted` URL (persistent).
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
- increment `state_post_failures`
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
6. `POST /config` persists viewport, scrypted URL, idle timeout, orientation, and brightness across reboot, with partial-update semantics.
7. `POST /config` validates `idle_timeout_ms` (0 disables; non-zero ≥ 5000; else 400), `orientation` (`portrait` or `landscape`; else 400), and `brightness` (0–100; else 400). Defaults on first boot: `orientation=portrait`, `brightness=80`, `idle_timeout_ms=60000`.
8. `POST /state` transitions wake↔sleep idempotently and rejects unknown state values with 400.
9. `POST /frame` paints when awake, returns 409 when asleep, and never changes state.
10. Brightness PWM is gamma-corrected (perceptual 0–100).
11. Idle timer POSTs `{viewport, state:sleep}` to `<scrypted>/state` after `idle_timeout_ms` of no `/frame`.
12. Tap toggles wake/sleep locally and POSTs `{state:wake}` or `{state:sleep}` to `<scrypted>/state`.
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
