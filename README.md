# Scrypted Viewport v1 Technical Design Specification

Version: 1.0

## Overview

Scrypted Viewport is an Ethernet-powered ambient display appliance optimized for Scrypted camera and doorbell
events. It's meant to be plug and play onto a trusted POE VLAN connection with Scrypted access so there's no
configuration done on the esp32 itself and instead is discoverd and configured within Scrypted.

Design goals:
- No Matter
- No HomeKit
- No device-side polling (the device never asks; Scrypted pushes)
- No configuration UI
- No authentication
- No general-purpose UI framework — the only locally-drawn screens are the IP screen and the Loading screen
- No business logic on the device

Scrypted owns rendering, overlays, camera selection and interaction logic.

Scrypted Viewport owns Ethernet, JPEG decode, display, touch input and outbound state-change POSTs.

## Related docs

- [`TESTING.md`](TESTING.md) — **the self-contained verification reference.** Status snapshot for every milestone, hardware prerequisites + open unknowns, bench-order playbook, per-milestone `curl` recipes, integration test suite. Start here before flashing anything.
- [`scrypted/README.md`](scrypted/README.md) — Scrypted-side script install + per-viewport binding UI (Path B with camera picker and mDNS auto-resolve).

## Status

| Layer | Where we are | What's pending |
| --- | --- | --- |
| Firmware (`main/`) | M1–M8 implemented; binary ~870 KB; builds clean against ESP-IDF 5.4 for `esp32p4`. **M1 + M2 ✅ verified on hardware** (2026-06-14, Ethernet + `GET /state` + mDNS browse). | M9 (`POST /stream`) not started. M3–M8 still 🟡 (panel not yet attached). |
| Scrypted side (`scrypted/`) | v1 Script — DeviceProvider with per-viewport child devices, camera picker, mDNS auto-resolve. | v2 plugin (packaged + FFmpeg streaming to `/stream`) not started. Script unverified end-to-end. |
| Hardware | Ethernet pin map confirmed (Waveshare wiki + ESPHome). Hosyond panel architecture confirmed (Pi 7"-style, TC358762 bridge + ATTINY MCU at I²C `0x45`). Jumper wiring documented. | Schematic confirmation needed for DSI FPC pin count, I²C GPIO mapping, BOOT button GPIO, flash size. All gated by [`TESTING.md`'s Hardware prerequisites](TESTING.md#hardware-prerequisites). |

See [`TESTING.md`](TESTING.md) for the full milestone-by-milestone status and the bench-order playbook.

## Hardware

### Controller
[Waveshare ESP32-P4-ETH](https://www.amazon.com/ESP32-P4-Ethernet-Development-MIPI-CSI-Microphone/dp/B0FN7JQ2V8/) — ESP32-P4 with IP101GRI PHY, 2-lane MIPI-DSI out, 32 MB PSRAM, 16/32 MB flash. PoE is an optional add-on module on the same SKU.

### Display
[Hosyond 5" 800x480 IPS Capacitive Touch MIPI DSI display](https://www.amazon.com/dp/B0CXTFN8K9) — Pi-compatible panel (TC358762 DSI-to-DPI bridge + ATTINY-class init MCU at I²C `0x45`, FT5426 touch at `0x38`). 15-pin Pi FPC for the DSI lanes + power; I²C runs as jumpers off the panel's auxiliary header (see [`TESTING.md`](TESTING.md) M3 for the wiring table).

## Boot

Power
-> DHCP
-> mDNS (_scrypted-viewport._tcp.local)
-> If no `scrypted` URL set: backlight on, show info screen (persistent until `/config`)
-> If configured: enter sleep state (backlight off, wait for `POST /state` or a tap)

## Resolution

Native panel: 800x480 landscape.

Effective resolution depends on `orientation` (set via `/config`):

- `portrait` (default): 480x800
- `landscape`: 800x480

Scrypted must render JPEGs at the **effective** resolution. The device does not scale or rotate JPEG content — orientation is applied during framebuffer-to-panel push.

## Network

HTTP listen port: `80`.

Trust model: LAN-only, no auth, no TLS. Deploy on a trusted VLAN.

## Discovery

The ESP32 publishes itself via **mDNS-SD (service discovery)**. Scrypted discovers viewports by browsing the service; it does not need OS-level `.local` hostname resolution.

The device runs an mDNS responder (ESP-IDF `mdns` component) that serves all of the following from itself — no external DNS server is involved:

- **Hostname / A record**: `viewport.local` before configuration, `viewport-<name>.local` after (e.g. `viewport-mudroom.local`). The `viewport-` prefix is a namespace that avoids collisions with other LAN devices.
- **Service advertisement**: `_scrypted-viewport._tcp.local` on port 80.
- **SRV record**: hostname + port.
- **TXT records**:
  - `version=1.0.0`
  - `resolution=<effective>` (e.g. `480x800` for portrait, `800x480` for landscape)
  - `orientation=<portrait|landscape>`
  - `name=<viewport name>` (empty until `/config`)

Scrypted-side discovery flow:

1. Browse `_scrypted-viewport._tcp.local` via a Node mDNS-SD library (`bonjour-service`, `mdns`, etc.). These libraries talk to the multicast layer directly — they do **not** need OS-level `.local` resolution.
2. Each browse result contains `{name, host, port, addresses, txt}`. Use the **IP** from `addresses`, not the hostname, for all subsequent calls.
3. Match TXT `name=` against the operator's `viewport → camera` config bindings.
4. Re-browse periodically (every few minutes) to catch DHCP renumbering.

If mDNS-SD is unavailable in the deployment (some Docker setups, certain VLAN configurations), allow the operator to set an explicit `http://<ip>:<port>` per viewport in Scrypted-side config as a fallback.

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
  "state_post_failures": 2,
  "resolution": "480x800",
  "ip": "192.168.1.42",
  "free_heap": 123456,
  "free_psram": 12345678
}
```

`state` is `awake` or `asleep` (it reports the screen's current state only). `configured` is the separate flag that reports whether `viewport` and `scrypted` are both set. `last_frame_ms_ago` is `null` if no frame has been received since boot.

### POST /state

```json
{ "state": "wake" }
```

- `state` is `wake` or `sleep`. Anything else: `400`.
- Idempotent. Already-in-that-state calls return `204` and do nothing.
- `wake`: backlight on, render loading screen (until the next `/frame`), reset idle timer.
- `sleep`: backlight off; framebuffer discarded.
- The device does NOT POST back to Scrypted (Scrypted initiated).
- Response: `204`.

### GET /config

Returns the persisted config:

```json
{
  "viewport": "mudroom",
  "scrypted": "http://scrypted.local:11080/endpoint/scrypted-viewport",
  "idle_timeout_ms": 60000,
  "orientation": "portrait",
  "brightness": 80
}
```

Before first `/config`: returns `200` with `viewport` and `scrypted` as `null`; the rest carry their first-boot defaults (`brightness: 80`, `orientation: "portrait"`, `idle_timeout_ms: 60000`).

### POST /config

```json
{
  "viewport": "mudroom",
  "scrypted": "http://scrypted.local:11080/endpoint/scrypted-viewport",
  "idle_timeout_ms": 60000,
  "orientation": "portrait",
  "brightness": 80
}
```

- **Partial update**: only fields present in the body are changed; omitted fields keep their current values. The persisted config is replaced atomically with the merged result.
- Persisted to NVS, survives reboot.
- Idempotent; reposting the same body yields the same state.
- `viewport` must be non-empty; `scrypted` must be `http://...` and is the Scrypted plugin's base URL. The device POSTs state changes to `<scrypted>/state`.
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
- **Requires awake state.** While asleep, returns `409 Conflict` and does not paint. Scrypted must `POST /state {"state":"wake"}` first (or wait for a tap-driven `wake` POST from the device).
- Resets the idle timer on success.
- Single in-flight frame; concurrent posts may be rejected with `503`.
- Returns `204` once decoded and pushed to the panel.
- `400` malformed JPEG, `409` device asleep, `413` over size, `500` decode/display failure. On failure the previous frame stays on screen.

## Wake / Sleep

Wake and sleep couple the device backlight with Scrypted's frame stream. The device owns the state.

Both the device and Scrypted expose the same endpoint, `POST /state`, with the same body shape `{viewport, state}` — they're peers. Either side can push to the other to set state. Repeats are safe; both sides are idempotent.

- `{"viewport": "<name>", "state": "wake"}` → "start streaming to this viewport now"
- `{"viewport": "<name>", "state": "sleep"}` → "stop streaming to this viewport now"

Each request carries the device's `viewport` name as the routing key. Scrypted does not track per-viewport state across requests; it acts on each and forgets.

Transitions:

| Trigger | Resulting state | Device → Scrypted `POST /state`? |
| --- | --- | --- |
| Tap while asleep | Awake (loading screen) | `state=wake` |
| Tap while awake | Asleep | `state=sleep` |
| Idle timer expires | Asleep | `state=sleep` |
| `POST /state {"state":"wake"}` | Awake (loading screen) | none |
| `POST /state {"state":"sleep"}` | Asleep | none |
| `POST /frame` | (no state change) | — |

`/frame` never changes state. Scrypted must `POST /state {"state":"wake"}` (or wait for a tap-driven `wake` POST from the device) before sending frames. This makes the protocol race-free: a `/frame` arriving after a tap-to-sleep is rejected with `409`, not silently re-woken.

Only `tap` is detected on the touchscreen — long-press and swipes are out of scope for v1. `tap` itself is internal; what Scrypted sees is the resulting `state`.

### Device → Scrypted `POST /state`

When the device changes state on its own (a tap, the idle timer firing), it POSTs to Scrypted's `/state` endpoint. Same shape as Scrypted POSTing to the device's `/state`; no `event`, no `type`, no callback semantics. The two endpoints are peers, not request/response.

**Request**

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

Same body as Scrypted → Device `POST /state`, plus the routing key.

- `viewport` (string): the value of the `viewport` field in the device's `/config`. Scrypted's routing key.
- `state` (string): `wake` or `sleep`. The resulting state and the imperative for Scrypted.

No other fields. No timestamp (no RTC, no SNTP); Scrypted timestamps on receipt.

**No application-level ack**

HTTP 2xx is transport-level only. There is no application-level ack: the device does not retry, does not block subsequent state changes on the response, and does not treat a 5xx response as anything more than a counter increment. Idempotency + `/frame` returning `409` + each side's independent idle timer recover every failure mode without an ack:

- Scrypted misses the device's `sleep`: its own per-stream timer eventually stops the stream, or the next `/frame` it sends returns `409`.
- Device misses Scrypted's `wake`/`sleep`: same — the next state change on either side syncs them.

Don't design Scrypted-side logic that waits for the device to confirm a state change. There is no confirmation.

**Expected response (counter only)**

- Any 2xx is success.
- Body is ignored.
- Anything else (non-2xx, connection refused, DNS failure, request timeout) increments `state_post_failures` and is otherwise ignored.

**Timeouts**

- Connect timeout: 1s.
- Total request timeout: 1s.
- After timeout the device aborts the connection and moves on.

**Concurrency and ordering**

- At most one outbound `/state` POST in flight at a time.
- POSTs are delivered in the order state changes occur on the device.
- If a state change happens while a POST is in flight, it goes into a depth-1 queue. If the queue already holds a POST, the queued entry is **replaced** by the newer one. The in-flight POST is never cancelled.
- Replacement is safe because POSTs are imperatives: only the latest desired state matters to Scrypted. Intermediate flips between wake and sleep within the queue window collapse to the final state.

**Failure semantics**

- The local state change always happens regardless of POST outcome.
- A dropped or failed POST is recovered by the next user tap, the device's idle timer firing `sleep`, or the next `/frame` from Scrypted returning `409`.
- No retry queue. No backoff. No persistence across reboots.

**When the device does NOT POST**

- Before `/config` has registered a `scrypted` URL (boot state, NVS-erased). Silently dropped.
- For state changes Scrypted initiated (`POST /state`, `POST /frame` while asleep). Scrypted already knows; echoing would loop.
- Long-press info overlay (not a state change).

**Trust model**

Plain HTTP, no TLS, no auth — same as the inbound API. LAN-only.

### Race handling

Wake/sleep changes can race: a user taps the device while Scrypted is mid-flight with a `POST /state` from a stale camera-event timeout, or the idle timer fires at the same instant Scrypted POSTs a fresh `wake`. The protocol does **not** carry epochs, session IDs, or priorities. Race resolution is purely about each side serializing its own writes and trusting idempotency to converge.

Rules:

1. **Device-side serialization.** The device guards its state-mutation function with a mutex. Tap, idle timer, and `POST /state` all funnel through it. Whichever lands second wins.
2. **Scrypted-side serialization.** Scrypted handles inbound `POST /state` requests for a given `viewport` one at a time (in-process queue per viewport). Whichever lands second wins.
3. **Last write wins.** No priorities. No "the device is where the user is so it always trumps Scrypted." If a stale `sleep` lands after a fresh `wake`, the device sleeps; the user taps again and we're back. One extra tap is cheap.
4. **Scrypted must cancel its own pending operations on each inbound POST.** When a `wake` arrives, Scrypted cancels any pending per-viewport sleep timer before starting a fresh stream. Same in reverse. This makes "stale Scrypted timer fires after the user tapped to wake" impossible without needing protocol-level epochs.

Idempotency does the rest — `POST /state` on either side and `POST /frame` (relative to its `409`-vs-`204` behavior) are all no-ops when the recipient is already in the requested state.

## Idle

After `idle_timeout_ms` (default 60s) with no `/frame`, the device sleeps and POSTs `state=sleep`. Scrypted should use the same timeout so its per-stream cutoff matches, but they run independently — either side can end the session, whichever notices first.

## Idempotency

All endpoints are safe to retry. Every state-change path converges to the same final state regardless of how many times it's repeated or in what order:

| Endpoint | Idempotent? | Notes |
| --- | --- | --- |
| `GET /state` | yes | side-effect free |
| `GET /config` | yes | side-effect free |
| `POST /config` | yes | partial merge into persisted config, atomic |
| `POST /state` | yes | no-op if already in that state |
| `POST /frame` | yes (within state) | paints if awake; `409` if asleep — no partial state |

`POST /state` (in either direction) carries imperatives, not notifications. Both sides act and forget; neither expects an application-level ack. A dropped POST is recovered by the next user action, by Scrypted's own timeout, or by a `/frame` returning `409`.

Failure modes do not corrupt state:

- Failed outbound `/state` POST: local state still changes; `state_post_failures` increments.
- Failed JPEG decode: previous frame stays; state unchanged.
- Network drop mid-stream: device idle-sleeps when the timer expires.
- Concurrent `/frame` posts: one wins, the other gets `503`; no half-painted frames.

## Touch gestures

The board has no usable user button (GPIO 35 is owned by EMAC TXD1 at runtime), so both behaviours live on the touch panel:

- **Short tap** (<500 ms): toggle wake / sleep. POSTs `/state` to Scrypted when configured.
- **Long-press** (≥1.5 s): overlay the info screen for 15 seconds, then return to the prior state. Useful for identifying or re-registering a device that's already configured. Wakes the backlight temporarily; does not change the wake/sleep state and does not POST to Scrypted. An incoming `/frame` while the overlay is showing is rejected with `409` (state is still "sleep" underneath).

There is no factory-reset gesture. To wipe NVS, plug USB and run `idf.py erase-flash` followed by a normal reflash.

## Local rendering

The device renders exactly two things itself; everything else is a JPEG from Scrypted:

- **Info screen**: ~15 lines of `label  value` pairs (white on black, auto-scaled) covering the full `GET /config` + `GET /state` dump — name, host, ip, state, configured, scrypted, orientation, brightness, idle, firmware, uptime, frames, errors, free heap, free PSRAM. Shown on first boot until `/config`, on NVS erase, and as a 15 s overlay on a touch long-press.
- **Loading screen**: shown between a wake and the next `/frame` arriving. Plain "Loading…" text. Rendered in the current orientation.

Both use a small embedded bitmap font — full lowercase a–z, digits, period, colon, dash, slash, plus uppercase `L` for "Loading...". No LVGL, no general text engine.

## Scrypted Integration

The Scrypted side is **code, not configuration** — Scrypted has no built-in concept of a network framebuffer. The code is small and lives inside Scrypted:

- **v1** (in this repo at [`scrypted/scrypted-viewport.ts`](scrypted/scrypted-viewport.ts), install instructions in [`scrypted/README.md`](scrypted/README.md)): a Scrypted Script (in the Scripts plugin) — listens for camera events, calls `takePicture()`, POSTs the JPEG to `/frame`. Exposes a `POST /state` handler at the plugin's endpoint root (e.g. `http://scrypted.local:11080/endpoint/scrypted-viewport/state`) via the EndpointManager. ~250 lines of TypeScript, no package install.
- **Next milestone after v1 (`/stream`)**: add `POST /stream` with `multipart/x-mixed-replace` chunked body for live frame rates. Scrypted side becomes a small custom plugin using FFmpeg via `MediaManager` to pipe MJPEG. `/frame` stays for snapshots and debug.

Either way, no Scrypted core changes and no external service.

Scrypted owns a static list of viewports, each **bound to one Scrypted camera device** in the script/plugin. The binding tells Scrypted which camera's events drive that viewport (doorbell press, person/motion detection, etc.) and which camera's frames to push to it.

On startup, register every viewport:

```ts
const SCRYPTED_BASE = "http://scrypted.local:11080/endpoint/scrypted-viewport";

await fetch(`${v.url}/config`, {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({
    viewport: v.name,           // e.g. "mudroom"
    scrypted: SCRYPTED_BASE,
    idle_timeout_ms: 60000,
    orientation: "portrait",
    brightness: 80
  })
});
```

To start a session (camera event like doorbell, motion, person):

```ts
await fetch(`${v.url}/state`, {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ state: "wake" })
});  // device shows loading

// then stream frames:
await fetch(`${v.url}/frame`, {
  method: "POST",
  headers: { "Content-Type": "image/jpeg" },
  body: jpegBuffer  // baseline JPEG at the viewport's effective resolution, <1 MB
});
```

Expose `POST /state` at `<SCRYPTED_BASE>/state` (peer of the device's `/state`). Body is `{viewport, state}`:

```ts
const { viewport, state } = req.body;
const v = viewports.find(x => x.name === viewport);
if (!v) return res.status(404).end();

cancelPendingSleep(v);     // race rule: cancel stale timers on every incoming POST

if (state === "wake")  startStream(v);   // idempotent
if (state === "sleep") stopStream(v);    // idempotent

res.status(204).end();
```

- `state=wake` → start streaming frames to the viewport's bound camera. You do not need to `POST /state` to the device first; the device is already awake when it sends this.
- `state=sleep` → stop streaming frames to that viewport.

Both are idempotent. Don't track viewport state across requests; act on each and forget.

Scrypted should use the same `idle_timeout_ms` value it sent in `/config` as its own per-stream cutoff. The two timers run independently — either side can cut a session, whichever notices first. If the device's outbound `sleep` POST is lost, the Scrypted-side timeout still ends the stream; the next `/frame` posted after the device idle-slept simply returns `409` and Scrypted stops.

## Ops

- Firmware updates: reflash over USB. No OTA in v1 (planned post-v1: HTTP OTA from Scrypted).
- Provisioning: flash the same firmware to every device. On first boot the screen shows its IP; register it from Scrypted via `POST /config`.
- Viewport names must be unique across the LAN — mDNS hostnames are derived from `viewport` and two devices configured with the same name will collide.
- NVS wipe: plug USB and run `idf.py erase-flash` followed by `idf.py flash`. The device boots clean and shows the info screen until `/config` is POSTed.
- No DHCP lease: keep retrying; do not reboot. The info screen shows "ip no network" until a lease arrives.
- Ethernet disconnect: reconnect automatically. If Scrypted is unreachable, displays go stale — nothing the device can do about it.
- Watchdog: the ESP-IDF task watchdog reboots the device if a task hangs. Soft state is rebuilt from NVS on every boot.
- No usable on-board status LED on the Waveshare ESP32-P4-ETH. The info screen tells the boot story instead — short-tap to wake, long-press to overlay it.

## Build

```sh
source ~/Dev/code/git/esp32/env.sh
cd ~/Dev/code/git/esp32/projects/esp32-poe-scrypted-viewport
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

## Firmware implementation notes

### Source map (`main/`)

| File | What it does |
| --- | --- |
| `app_main.c` | Boot sequence: NVS → state → Ethernet → mDNS → HTTP → state machine → state-client worker → display → JPEG decoder → touch. |
| `viewport_state.{h,c}` | Shared runtime state behind a FreeRTOS mutex; every module reads/writes through `viewport_state_lock`. |
| `nvs_config.{h,c}` | Persist viewport, scrypted URL, idle timeout, orientation, brightness to NVS. Load on boot; flip state UNCONFIGURED → ASLEEP when both name + URL are present. |
| `net_eth.{h,c}` | Internal EMAC + IP101GRI PHY init, DHCP wait, IP getter. Pin map verified against Waveshare wiki + ESPHome's working config. |
| `mdns_service.{h,c}` | `mdns_init()` + `_scrypted-viewport._tcp.local` advertisement. `mdns_service_refresh()` reapplies hostname + TXT after `/config` writes change them. |
| `http_api.{h,c}` | `esp_http_server` on :80. All five endpoints (`GET /state`, `GET /config`, `POST /config`, `POST /state`, `POST /frame`) with partial-update + validation + status codes from the spec. |
| `display.{h,c}` | Pi 7"-style panel: I²C bring-up of the on-panel MCU at `0x45` (power-on register dance), ESP32-P4 MIPI-DSI in DPI video mode at canonical Pi 7" timings, gamma-corrected backlight PWM, orientation-aware blit (memcpy landscape / 90° CW rotate portrait). |
| `jpeg_decoder.{h,c}` | ESP32-P4 hardware JPEG engine with a 1 MB PSRAM scratch buffer and a `try_lock(0)` for concurrent `/frame` → 503. |
| `state_machine.{h,c}` | Central wake/sleep transitions (mutex-protected, idempotent). Owns the `esp_timer` idle one-shot. `state_machine_set_local()` is the device-initiated variant — drives the transition *and* fires the outbound `/state` POST. |
| `state_client.{h,c}` | Worker task + `xQueueOverwrite()` depth-1 queue for outbound POSTs to `<scrypted>/state`. 1 s timeout, fire-and-forget, `state_post_failures` counter. |
| `touch.{h,c}` | FT5426 polling at 30 ms over the shared I²C bus. Tap = down→up within 500 ms, 150 ms debounce. |
| `local_screens.{h,c}` | 8×8 bitmap font (sparse 95-char table populated for the IP and Loading strings), centered text rendering at scale 3×/4× into a PSRAM scratch FB, routed through `display_present_rgb565()` so orientation is automatic. |
| `touch.{h,c}` | FT5426 polling. Short tap → toggle wake/sleep. ≥1.5 s hold → 15 s info-screen overlay. (Replaced `button.{h,c}` — no usable hardware button.) |

### Memory strategy

The Waveshare board ships with 32 MB PSRAM. Everything large lives there: JPEG input scratch (1 MB), decoder output (768 KB RGB565 native), `esp_lcd_dpi` framebuffer (also PSRAM-backed), local-screens scratch (768 KB). Internal SRAM is reserved for FreeRTOS task stacks and small allocations — `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` keeps allocations ≤ 16 KB in SRAM by default.

### Display strategy

JPEG → RGB565 framebuffer → DSI panel, single buffer. No double buffering, no LVGL, no general text engine. The only locally-drawn UI is the IP screen and the Loading screen (both via the 8×8 bitmap font in `local_screens.c`). Brightness PWM is gamma-corrected — `duty = (level/100)^2.2 * 255` — so 0–100 maps to perceptual brightness instead of linear duty cycle.

### Error handling

Every endpoint is idempotent; every failure leaves the device in a sane state.

- JPEG decode fails → `decode_errors++`, return 400 or 500, keep previous frame on screen, wake/sleep state unchanged.
- Outbound `/state` POST fails → `state_post_failures++`, continue. Local state change still happened. No retry queue; Scrypted catches up via its own timeout, the next event, or the next `/frame` returning 409.
- Ethernet disconnects → driver reconnects automatically. No reboot loop. `GET /state` keeps serving over loopback for diagnostics.
- Display init fails → log loudly, keep serving the rest of the API. The protocol is still usable for re-registration.
- Task hangs → ESP-IDF watchdog reboots. NVS rebuilds soft state on the next boot.

### Coding standards

- C with ESP-IDF APIs. No C++.
- No dynamic allocation in hot paths after startup. Allocate at init, reuse.
- PSRAM is opt-in: `heap_caps_malloc(MALLOC_CAP_SPIRAM)` for buffers > 16 KB.
- Constants centralized at the top of each module (pin map, timeouts, panel timings, register addresses).
- No clever abstractions. No display framework beyond `esp_lcd`.
- Per-module mutex on shared state; one in-flight worker for outbound HTTP and JPEG decode.

## What's next

M1 – M8 are all ✅ on hardware (see [`TESTING.md`](TESTING.md) for verification details). End-to-end Scrypted streaming via ffmpeg + the zero-copy `JPEG → BGR888 → DSI` hot path sustains **~22 fps at full quality** with the double-buffered panel.

### Measured per-frame budget

The firmware logs a `frame N: lock=… ttfb=… body=… dec=… paint=… post=… total=…ms` line every 10 frames. Steady-state on the bench (Waveshare ESP32-P4-ETH + Hosyond 5" panel, ~215 KB JPEGs at `-q:v 2`):

| Phase | Time | Share | What it is |
|---|---|---|---|
| `lock` | 6–10 µs | < 0.1% | `jpeg_decoder_try_lock` mutex acquire |
| `ttfb` | 300–650 µs | < 1% | First byte landing after lock — TCP handshake noise |
| `body` | 37–44 ms | **~85%** | Wire time for the JPEG body (~215 KB at ~45 Mbit/s effective on 100 Mbit Ethernet) |
| `dec` | ~6 ms | ~13% | Hardware JPEG decode → BGR888 (way faster than the textbook 50–80 ms guess) |
| `paint` | 35–55 µs | < 0.1% | `esp_lcd_panel_draw_bitmap` — cache writeback + index swap thanks to `num_fbs = 2` and zero-copy decode into the back fb |
| `post` | 20–30 µs | < 0.1% | State counter bookkeeping + unlock |
| **total** | **43–49 ms** | | **Ceiling ~22 fps** |

Network body is now ~85% of every frame; everything else is at or near hardware floor.

### Current backlog, in rough priority order

1. **Network body shrink** — *the only remaining big lever*. The firmware is reading ~215 KB JPEGs at ~45 Mbit/s effective on a 100 Mbit Ethernet link — that's about 45% of theoretical. Two routes:
   - **TCP tuning in `sdkconfig`**: bump `CONFIG_LWIP_TCP_WND_DEFAULT` and `CONFIG_LWIP_TCP_RCVMBOX_SIZE`, plus enable `CONFIG_LWIP_TCP_RTO_MIN` adjustments. With BDP at LAN-1 ms × 100 Mbit ≈ 12.5 KB, the default 5.7 KB window is half what we need; doubling it should add 30–50% throughput.
   - **Per-viewport JPEG quality** (`-q:v` on the ffmpeg side): -q:v 5 halves file size at modest visual cost. `body` halves with it.
   Combined, total → ~22 ms → **~45 fps ceiling**.
2. **OTA firmware updates** — *low effort, gateway to fleet ops*. Partition table already has `ota_0` / `ota_1`. Add `esp_https_ota` (or a one-shot `POST /firmware` using `esp_ota_*`) so reflashing doesn't require USB. Critical once you have more than one viewport in production.
3. **Task watchdog + crash counters** — *low effort*. Enable the ESP-IDF task watchdog, surface its bite count in `/state` alongside the existing `decode_errors` / `state_post_failures`. Good hygiene.
4. **Multi-camera per viewport** — *medium effort*. Let one viewport listen to events from N cameras, picking which one to stream based on which fired. Useful for "show whichever doorbell rang" or zone monitoring.
5. **MJPEG-over-WebSocket** instead of `POST /frame` per JPEG — *medium effort, marginal win*. Skip unless we find a use case the per-frame path can't cover.
6. **Boot info-screen flash polish** — *low effort*. Keep the brief wake-on-boot (the FT5426 needs it to start reporting touches) but smoothen the ~600 ms flash so it doesn't visibly flicker on power-up.
7. **Production sealing** — *eventual*. Configurable LAN scope (cross-VLAN, mDNS-via-Unicast), Scrypted-side mutual auth, replay protection for `/state` callbacks.

## Philosophy

Scrypted Viewport is a thin network framebuffer appliance.

ESP:
- DHCP
- mDNS
- HTTP server (`/state`, `/config`, `/frame`)
- HTTP client (outbound `/state` POSTs to Scrypted)
- JPEG decode
- framebuffer
- touch

Everything else belongs in Scrypted.

> If it changes **what** should be shown, it belongs in Scrypted.
> If it changes **how** pixels reach the screen, it belongs in the device.
