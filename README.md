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

- [`TESTING.md`](TESTING.md) — **the verification reference.** New-unit bring-up playbook, regression `curl` recipes, outstanding tests, and the performance-review methodology. (All bring-up milestones are ✅ verified; the historical milestone log lives in git history.)
- [`scrypted/README.md`](scrypted/README.md) — Scrypted-side script install + per-viewport binding UI (camera picker, wake triggers, mDNS-discovered host dropdowns).
- [`scrypted/PLUGIN-CONVERSION.md`](scrypted/PLUGIN-CONVERSION.md) — TODO: plan for repackaging the script as a real installable Scrypted plugin (kills the Scripts-sandbox reload-leak machinery, makes deploys scriptable).

## Status

| Layer | Where we are | What's pending |
| --- | --- | --- |
| Firmware (`main/`) | Full path ✅ on hardware: Ethernet, panel, streaming, OTA. Painted = sent = 24 fps at the Unifi medium substream, sub-50 ms glass-to-glass over a raw TCP data socket (:81). Binary ~900 KB, ESP-IDF 5.4 / `esp32p4`. See [What's next](#whats-next) for the measured budget. | Backlog only (task-watchdog counters, multi-camera per viewport, production sealing). |
| Scrypted side (`scrypted/`) | v1.4 Script — DeviceProvider with per-viewport child devices, camera picker with trigger-scoped event subscriptions, live ffmpeg MJPEG streaming over the TCP data socket with prebuffer fast-start (~0.7 s wake-to-video), and built-in mDNS discovery (host dropdowns, auto-naming, auto-heal on DHCP renumber). Verified end-to-end. | v2: repackage the single-file script as an installable plugin — planned in [`scrypted/PLUGIN-CONVERSION.md`](scrypted/PLUGIN-CONVERSION.md). |
| Hardware | Ethernet pin map confirmed (Waveshare wiki + ESPHome). Hosyond panel architecture confirmed (Pi 7"-style, TC358762 bridge + ATTINY MCU at I²C `0x45`). All bring-up unknowns resolved — wiring + gotchas in [`TESTING.md`](TESTING.md#new-unit-bring-up). | Per-unit flash-size silkscreen check (16 vs 32 MB) when provisioning new boards. |

See [`TESTING.md`](TESTING.md) for the new-unit bring-up playbook, regression recipes, and the outstanding-test backlog.

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

- **Hostname / A record**: `viewport-<name>.local`. The name always has a value — a MAC-derived default (colons stripped, e.g. `viewport-e8f60ae09094.local`) until `/config` sets a friendlier one (`viewport-mudroom.local`). The `viewport-` prefix is a namespace that avoids collisions with other LAN devices.
- **Service advertisement**: `_scrypted-viewport._tcp.local` on port 80.
- **SRV record**: hostname + port.
- **TXT records**:
  - `version=1.4.0`
  - `resolution=<effective>` (e.g. `480x800` for portrait, `800x480` for landscape)
  - `orientation=<portrait|landscape>`
  - `name=<viewport name>` (MAC-derived default until `/config`)
  - `mac=<aa:bb:cc:dd:ee:ff>` — stable identity for discovery; names are editable, the MAC is not

Scrypted-side discovery (implemented in the script — see `mdnsBrowse` in [`scrypted/scrypted-viewport.ts`](scrypted/scrypted-viewport.ts)):

1. The Scripts sandbox has **no third-party mDNS libraries** (`bonjour-service`, `multicast-dns`, `mdns` all fail to resolve — verified), so the script browses with a plain `dgram` socket on an **ephemeral port**: per RFC 6762 §6.7 a query from a non-5353 source port is a *legacy unicast* query and responders reply unicast straight back to it. No `:5353` bind means no conflict with Scrypted's own HomeKit mDNS stack or a host `avahi-daemon`, and it works under Docker host-networking or a native install alike.
2. One PTR response packet carries PTR + SRV + TXT + A together (RFC 6763 §12.1), so parsing is per-packet with no follow-up queries. The **IP** from the A record is what's used for all subsequent calls.
3. Discovered viewports surface as dropdown choices on the host field (add-device form + each viewport's settings page), and a blank name on create inherits the discovered TXT `name`.
4. **Auto-heal instead of periodic re-browse**: when a `/config` registration fails (DHCP renumber), the script re-browses and matches by TXT `mac` first, then `name`; on a hit at a new address it rewrites the stored host and retries. This removes the need for a DHCP reservation.

Manual entry (IP or hostname) always remains available on the host field — discovery is best-effort and degrades to typing.

### Discover from the CLI

The custom service type is the discovery handle — no need to know names ahead of time. From macOS:

```sh
# List every viewport on the LAN
dns-sd -B _scrypted-viewport._tcp local.

# Resolve one to an IPv4 address
dns-sd -G v4 viewport-kitchen.local.

# Talk to it (mDNS resolves the hostname directly)
curl http://viewport-kitchen.local/state
```

On Linux: `avahi-browse -rt _scrypted-viewport._tcp` for the same effect with addresses inline. The instance name (`viewport-kitchen`) **is** the hostname prefix — they're the same string.

## API

Four endpoints. `GET /state` and `GET /config` are the read surface; `POST /config`, `POST /state`, and `POST /frame` are the write surface.

### GET /state

Returns the device's runtime status. Replaces the old `/health` endpoint.

Returns `200 OK` with JSON:

```json
{
  "name": "mudroom",
  "mac": "e8:f6:0a:e0:90:94",
  "version": "1.4.0",
  "ota_state": "valid",
  "configured": true,
  "state": "awake",
  "uptime_ms": 12345678,
  "last_frame_ms_ago": 1234,
  "frames_received": 4271,
  "decode_errors": 0,
  "state_post_failures": 2,
  "resolution": "480x800",
  "panel_width": 800,
  "panel_height": 480,
  "ip": "192.168.1.42",
  "free_heap": 123456,
  "free_psram": 12345678,
  "tear_guard_engaged": 111,
  "temp_c": 55.0,
  "stream": { "frames": 30, "window_us": 1250000, "...": "..." }
}
```

`state` is `awake` or `asleep` (it reports the screen's current state only). `configured` reports whether a `scrypted` URL is registered (the name always has a MAC-derived default, so it doesn't factor in). `last_frame_ms_ago` is `null` if no frame has been received since boot. `ota_state` is the running image's OTA slot state (`pending-verify` right after an OTA until the 30 s healthy timer marks it `valid` — the rollback tell, see [POST /firmware](#post-firmware)). `temp_c` is the on-die junction temperature (~10–20 °C above ambient; omitted if the sensor is unavailable). `tear_guard_engaged` counts frames the triple-buffer guard saved from tearing. `stream` is the most recent 30-painted-frame window of data-plane stats (recv/decode/paint/idle min/avg/max, wire rate, header-gap and pending-age decomposition, drop counters — see `stream_server.h` for field semantics); all zeros before the first window rolls.

### POST /state

```json
{ "state": "wake" }
```

- `state` is `wake` or `sleep`. Anything else: `400`.
- Idempotent. Already-in-that-state calls return `204` and do nothing.
- `wake`: backlight on, render loading screen (until the first frame paints), reset idle timer.
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

Before first `/config`: returns `200` with `scrypted` as `null`; `viewport` carries its MAC-derived default and the rest their first-boot defaults (`brightness: 80`, `orientation: "portrait"`, `idle_timeout_ms: 60000`).

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
- `viewport` must be non-empty and ≤ 54 chars (so the `viewport-<name>` mDNS hostname fits the 63-byte DNS label limit); `scrypted` must be `http://...` and is the Scrypted plugin's base URL. The device POSTs state changes to `<scrypted>/state`.
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

### POST /firmware

Push a new application image and reboot into it. The device's HTTP OTA
endpoint — replaces USB reflash once the device is on the LAN.

- `Content-Type: application/octet-stream`, body is the raw built app
  image (`build/scrypted-viewport.bin`, ~1.5 MB). `Content-Length`
  required.
- Single-shot: a second concurrent POST returns `409 Conflict`.
- Streams straight to the inactive OTA slot (no full-image RAM buffer),
  validates header + checksum on `esp_ota_end`, flips `otadata` to point
  at the new slot, replies `200` with
  `{"status":"ok","previous":"<git>","next":"<git>","slot":"ota_1","reboot_in_ms":500}`,
  then reboots ~500 ms later so the response can flush.
- On failure (`400` bad body / validate failed, `413` over partition size,
  `500` flash error) the OTA handle is aborted and the running image stays
  live. No partial-write half-state.
- **Rollback armed.** The new image boots `pending-verify`; firmware
  flips it to `valid` after 30 s of healthy uptime
  (`ota_arm_healthy_timer`). If the new image panics or the device is
  power-cycled before the timer fires, the bootloader reverts to the
  previous slot on next reset. `/state` reports the current state via
  `ota_state`.

The repo Makefile wraps the whole loop (recipes run in bash regardless
of your interactive shell; `idf.py` env is sourced per-recipe):

```sh
make ota                    # reconfigure (fresh git stamp) + build + push + verify
make ota VIEWPORT=<host>    # same, against a specific device (default 10.0.13.83)
make build                  # incremental firmware build
make cleanbuild             # wipe build/ + rebuild (fixes stale CMake generator cache)
make verify                 # post-push pending-verify -> valid check only
make check                  # type-check the Scrypted plugin (tsc --noEmit)
```

`make ota` (via `tools/ota.sh`) encodes the acceptance criterion from
the rollback bullet above: the fresh boot must report
`ota_state=pending-verify` before flipping to `valid` — a boot that
reads `valid` immediately means the bootloader silently reverted to the
old slot, and the script re-pushes once automatically (a known quirk of
first pushes). It also runs `idf.py reconfigure` first because the
embedded git hash is stamped at CMake configure time only; without it
the binary reports a stale SHA after new commits.

The raw mechanism underneath:

```sh
idf.py build
curl -v --data-binary @build/scrypted-viewport.bin \
    -H 'Content-Type: application/octet-stream' \
    http://<device-ip>/firmware
```

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

Frames never change state — neither the MJPEG stream on the data socket nor a `POST /frame`. Scrypted must `POST /state {"state":"wake"}` (or wait for a tap-driven `wake` POST from the device) before frames will paint. This makes the protocol race-free: once the device is asleep, frames arriving on the stream socket are discarded by the decode task and a `POST /frame` is rejected with `409` — neither silently re-wakes the panel. Scrypted learns the device slept from its `state=sleep` callback (and, as a backstop, its own per-stream safety timer).

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

HTTP 2xx is transport-level only. There is no application-level ack: the device does not retry, does not block subsequent state changes on the response, and does not treat a 5xx response as anything more than a counter increment. Idempotency + frames not painting while asleep + each side's independent idle timer recover every failure mode without an ack:

- Scrypted misses the device's `sleep`: its own per-stream safety timer eventually tears down the ffmpeg stream, and in the meantime any frames still on the wire are discarded by the (now-asleep) device, so nothing paints.
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
- A dropped or failed POST is recovered by the next user tap, the device's idle timer firing `sleep`, or Scrypted's per-stream safety timer ending the stream.
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

Idempotency does the rest — `POST /state` on either side is a no-op when the recipient is already in the requested state, and frames (streamed on the data socket, or a `POST /frame`) never paint while the device is asleep, so a stale stream can't re-wake it.

## Idle

After `idle_timeout_ms` (default 60s) with no painted frame (a streamed frame over the data socket, or a `/frame` POST), the device sleeps and POSTs `state=sleep`. Scrypted should use the same timeout so its per-stream cutoff matches, but they run independently — either side can end the session, whichever notices first.

## Idempotency

All endpoints are safe to retry. Every state-change path converges to the same final state regardless of how many times it's repeated or in what order:

| Endpoint | Idempotent? | Notes |
| --- | --- | --- |
| `GET /state` | yes | side-effect free |
| `GET /config` | yes | side-effect free |
| `POST /config` | yes | partial merge into persisted config, atomic |
| `POST /state` | yes | no-op if already in that state |
| `POST /frame` | yes (within state) | paints if awake; `409` if asleep — no partial state |
| Stream socket (`:81`) | yes (within state) | frames paint if awake, are discarded if asleep — no partial state; skip-oldest keeps only the freshest |

`POST /state` (in either direction) carries imperatives, not notifications. Both sides act and forget; neither expects an application-level ack. A dropped POST is recovered by the next user action or by Scrypted's own per-stream safety timer.

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

- **Info screen**: ~17 lines of `label  value` pairs (white on black, auto-scaled) covering the full `GET /config` + `GET /state` dump — name, mac, host, ip, state, configured, scrypted, orientation, brightness, idle, firmware, uptime, frames, errors, free heap, free PSRAM, chip temperature. Shown on first boot until `/config`, on NVS erase, and as a 15 s overlay on a touch long-press.
- **Loading screen**: shown from a wake — and re-shown on each new stream connection — until the first frame paints, so a stale prior frame never flashes during the connect→first-frame gap. Plain "Loading…" text. Rendered in the current orientation.

Both use a small embedded bitmap font — full lowercase a–z, digits, period, colon, dash, slash, plus uppercase `L` for "Loading...". No LVGL, no general text engine.

## Scrypted Integration

The Scrypted side is **code, not configuration** — Scrypted has no built-in concept of a network framebuffer. The code is small and lives inside Scrypted:

- **v1** (in this repo at [`scrypted/scrypted-viewport.ts`](scrypted/scrypted-viewport.ts), install instructions in [`scrypted/README.md`](scrypted/README.md)): a Scrypted Script (in the Scripts plugin) — subscribes to the bound camera's events (only the interfaces the selected wake triggers need) and, on wake, spawns one `ffmpeg` child (via `MediaManager`) that pulls the camera's substream, scales/rotates to panel-native 800×480, and streams MJPEG frames to the firmware over a **raw TCP data socket (port 81)** at ~24 fps. It also exposes a `POST /state` handler at the plugin's endpoint root (e.g. `http://scrypted.local:11080/endpoint/scrypted-viewport/state`) via the EndpointManager. Single-file TypeScript, no package install.
- **Fast start via prebuffer**: the script requests the camera's prebuffered substream so ffmpeg opens on an already-buffered keyframe, cutting wake-to-first-frame from ~5–6 s to ~0.7 s. Requires a rebroadcast prebuffer on the streamed substream — see [`scrypted/README.md`](scrypted/README.md#fast-wake--camera-prebuffer-required).
- **Built-in discovery**: the script browses `_scrypted-viewport._tcp` itself (dependency-free legacy-unicast `dgram` query — see [Discovery](#discovery)), offers discovered viewports as host-field choices, names new viewports from the advertised TXT `name`, and auto-heals a viewport's stored host when a registration fails after a DHCP renumber.
- **Next (v2)**: repackage the single-file script as a proper installable plugin — planned in [`scrypted/PLUGIN-CONVERSION.md`](scrypted/PLUGIN-CONVERSION.md). The streaming path itself (ffmpeg → framed MJPEG over the TCP data socket) is already in place; the firmware's `POST /frame` remains for one-shot snapshots and debug.

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
    brightness: 100
  })
});
```

To start a session (camera event like doorbell, motion, person): POST `wake`, then open the TCP data socket and pipe MJPEG frames straight to it — no per-frame HTTP.

```ts
await fetch(`${v.url}/state`, {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ state: "wake" })
});  // device shows the Loading screen until the first frame lands

// then stream: one ffmpeg child → framed MJPEG over a raw TCP socket to :81.
// Each frame is [ "VPRT" | jpeg_len | seq | event_us_low ] + JPEG body, at
// panel-native 800x480. See scrypted-viewport.ts (startStream) for the framer
// and the skip-oldest backpressure handling.
const sock = net.createConnection({ host: v.host, port: 81, noDelay: true });
sock.write(framed);  // header + JPEG, one write per frame
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

Scrypted should use the same `idle_timeout_ms` value it sent in `/config` as its own per-stream cutoff. The two timers run independently — either side can cut a session, whichever notices first. When the device idle-sleeps itself it POSTs `{state: "sleep"}` back, and Scrypted tears down the stream; if that callback is lost, the Scrypted-side safety timer ends the stream anyway.

## Ops

- Firmware updates: `POST /firmware` with the raw built `.bin` (see [POST /firmware](#post-firmware)). First flash of any new device still needs USB to install the bootloader + initial image; every update after that is over the LAN. Rollback is armed — a panicking new image reverts to the previous slot on next reset.
- Provisioning: flash the same firmware to every device. On first boot it advertises itself via mDNS and shows the info screen; in Scrypted, "+ Add Device" lists it in the host dropdown (name auto-fills from the advertisement) — no IP hunting needed.
- Viewport names must be unique across the LAN — mDNS hostnames are derived from `viewport` and two devices configured with the same name will collide.
- NVS wipe: plug USB and run `idf.py erase-flash` followed by `idf.py flash`. The device boots clean and shows the info screen until `/config` is POSTed.
- No DHCP lease: keep retrying; do not reboot. The info screen shows "ip no network" until a lease arrives.
- Ethernet disconnect: reconnect automatically. If Scrypted is unreachable, displays go stale — nothing the device can do about it.
- Watchdog: the ESP-IDF task watchdog reboots the device if a task hangs. Soft state is rebuilt from NVS on every boot.
- No usable on-board status LED on the Waveshare ESP32-P4-ETH. The info screen tells the boot story instead — short-tap to wake, long-press to overlay it.

## Build

First flash of a new board goes over USB:

```sh
source ~/Dev/code/git/esp32/env.sh
cd ~/Dev/code/git/esp32/projects/esp32-poe-scrypted-viewport
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

Every update after that is one command over the LAN — build, OTA push,
and rollback-checked verification (see [POST /firmware](#post-firmware)
for what it does and the underlying curl):

```sh
make ota [VIEWPORT=<host>]
```

## Firmware implementation notes

### Source map (`main/`)

| File | What it does |
| --- | --- |
| `app_main.c` | Boot sequence: NVS → state → Ethernet → mDNS → HTTP → state machine → state-client worker → display → JPEG decoder → touch. |
| `viewport_state.{h,c}` | Shared runtime state behind a FreeRTOS mutex; every module reads/writes through `viewport_state_lock`. |
| `nvs_config.{h,c}` | Persist viewport, scrypted URL, idle timeout, orientation, brightness to NVS. Load on boot; recomputes `configured` (true iff a scrypted URL is present). |
| `net_eth.{h,c}` | Internal EMAC + IP101GRI PHY init, DHCP wait, IP getter. Pin map verified against Waveshare wiki + ESPHome's working config. |
| `mdns_service.{h,c}` | `mdns_init()` + `_scrypted-viewport._tcp.local` advertisement. `mdns_service_refresh()` reapplies hostname + TXT after `/config` writes change them. |
| `http_api.{h,c}` | `esp_http_server` on :80. All six endpoints (`GET/POST /state`, `GET/POST /config`, `POST /frame`, `POST /firmware`) with partial-update + validation + status codes from the spec. |
| `display.{h,c}` | Pi 7"-style panel: I²C bring-up of the on-panel MCU at `0x45` (power-on register dance), ESP32-P4 MIPI-DSI in DPI video mode at canonical Pi 7" timings, gamma-corrected backlight PWM, orientation-aware blit (memcpy landscape / 90° CW rotate portrait). Owns the tear-free triple-buffer scheme (see *Display strategy*): scanning-fb tracking via `on_refresh_done`, free-fb selection for the decoder, `tear_guard_engaged` counter. |
| `jpeg_decoder.{h,c}` | ESP32-P4 hardware JPEG engine with a 1 MB PSRAM scratch buffer and a `try_lock(0)` for concurrent `/frame` → 503. |
| `state_machine.{h,c}` | Central wake/sleep transitions (mutex-protected, idempotent). Owns the `esp_timer` idle one-shot. `state_machine_set_local()` is the device-initiated variant — drives the transition *and* fires the outbound `/state` POST. |
| `state_client.{h,c}` | Worker task + `xQueueOverwrite()` depth-1 queue for outbound POSTs to `<scrypted>/state`. 1 s timeout, fire-and-forget, `state_post_failures` counter. |
| `touch.{h,c}` | FT5426 polling at 30 ms over the shared I²C bus. Short tap (down→up within 500 ms, 150 ms debounce) → toggle wake/sleep. ≥1.5 s hold → 15 s info-screen overlay. (No usable hardware button — GPIO35 is owned by EMAC.) |
| `local_screens.{h,c}` | 8×8 bitmap font (sparse 95-char table), auto-scaled text rendering into a PSRAM scratch FB, routed through `display_present_rgb565()` so orientation is automatic. Info screen + Loading screen + long-press overlay timer. |
| `stream_server.{h,c}` | Raw-TCP frame ingestion on :81 — dedicated recv-task + decode-task with a 3-buffer PSRAM ring, drop-oldest handoff, and the windowed data-plane stats behind `/state`'s `stream` object. |
| `ota.{h,c}` | 30 s healthy-uptime timer that marks a `pending-verify` image `valid` (cancels bootloader rollback), plus the `ota_state` string for `/state`. |
| `chip_temp.{h,c}` | On-die temperature sensor (TSENS); `NAN` when unavailable. Feeds `/state`'s `temp_c` and the info screen. |

### Memory strategy

The Waveshare board ships with 32 MB PSRAM. Everything large lives there: the stream body ring (3 × ~1 MB JPEG input buffers), the three `esp_lcd_dpi` BGR888 framebuffers (3 × 1.15 MB — the JPEG decoder writes directly into these, there is no separate decoder output buffer), and the local-screens scratch (~1.15 MB). Internal SRAM is reserved for FreeRTOS task stacks, EMAC DMA buffers, and small allocations — `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` keeps allocations ≤ 16 KB in SRAM by default.

### Display strategy

JPEG → BGR888 → DSI panel, **triple-buffered**, zero-copy and tear-free. No LVGL, no general text engine. The only locally-drawn UI is the IP screen, the Loading screen, and the info overlay (all via the 8×8 bitmap font in `local_screens.c`). Brightness PWM is gamma-corrected — `duty = (level/100)^2.2 * 255` — so 0–100 maps to perceptual brightness instead of linear duty cycle.

**Why three framebuffers.** The DPI driver owns the framebuffers (`num_fbs = 3`); the hardware JPEG decoder writes straight into one of them, so `esp_lcd_panel_draw_bitmap` takes the IDF fast path — a cache writeback plus an index swap (~40 µs), no memcpy anywhere. But that index swap is *deferred*: the DSI DMA only reloads the new index at the **end of the frame scan in progress** (~21 ms period at ~47 Hz). With two buffers, flipping and immediately decoding the next frame writes into the buffer the DMA is still scanning out — a torn frame. That regime is common once frames arrive back-to-back (measured: ~6% of painted frames at full stream rate).

Waiting for vsync before decoding would fix it at the cost of up to one refresh period of latency per frame. Instead, the three buffers hold three roles — **scanning** (DMA is reading it), **pending** (flipped, displays at the next boundary), **free** — and `display_back_buffer()` always hands the decoder the free one. The scanning buffer is tracked from the driver's `on_refresh_done` ISR (fires exactly when the DMA reloads its index). Three buffers minus at most two excluded roles = always a safe decode target: tear-free by construction, with zero waiting. `/state` reports `tear_guard_engaged` — picks made while the previous buffer was still mid-scan, i.e. frames that would have torn under double buffering. Cost: one extra 1.15 MB PSRAM framebuffer.

### Error handling

Every endpoint is idempotent; every failure leaves the device in a sane state.

- JPEG decode fails → `decode_errors++`, return 400 or 500, keep previous frame on screen, wake/sleep state unchanged.
- Outbound `/state` POST fails → `state_post_failures++`, continue. Local state change still happened. No retry queue; Scrypted catches up via its own per-stream safety timer or the next event.
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

M1 – M9 are all ✅ on hardware (see [`TESTING.md`](TESTING.md) for verification details). End-to-end Scrypted streaming via ffmpeg + the zero-copy `JPEG → BGR888 → DSI` hot path now sustains **painted = sent = 24 fps** at the Unifi medium substream rate, sub-50 ms glass-to-glass, no source-side backpressure.

### Measured per-frame budget

The firmware streams over a long-lived raw TCP socket on port 81 (replacing the per-frame HTTP `POST /frame` pattern from the early milestones). The stream server runs `recv` on its own FreeRTOS task and hands frames off to a separate decode/paint task through a 3-buffer PSRAM ping-pong ring. Steady-state on the bench (Waveshare ESP32-P4-ETH + Hosyond 5" panel, Unifi medium substream → ~80–130 KB JPEGs at ffmpeg `-q:v 1`):

| Phase | Time | Share | What it is |
|---|---|---|---|
| `recv` | 14–18 ms | ~70% | Pure wire time for the body, measured on the recv-task (does NOT include decoder-lock acquisition any more) |
| `dec` | ~5 ms | ~25% | Hardware JPEG decode → BGR888 |
| `paint` | 16–60 µs | < 0.2% | `esp_lcd_panel_draw_bitmap` — cache writeback + index swap thanks to `num_fbs = 3` and zero-copy decode into the free fb (see *Display strategy* for the tear-free triple-buffer model) |
| `decode_idle` | **27–40 ms** | n/a | Time decode-task spent waiting on the recv→decode signal. Means the decode/paint stage is *idle* most of the time at the source rate — the wire is the cap. |

Scrypted side: `sent=24.0fps painted=24.0fps backpressured=false`, `g2g=31–41 ms`. No `fw-skipped`, no `drops`, no `flushes`.

### The critical change that unlocked this

For a long stretch the device painted ~17 fps against a 24 fps source. The single-task `recv → decode → paint` loop in `stream_server.c` blocked the socket recv for ~6 ms every frame during decode+paint, which forced the sender into a tight stop-go cycle against the IDF-default 5760-byte TCP window. Raising the window to 65535 made it *worse* — the sender could pump 45+ segments before stopping but the lwIP RX path couldn't drain that into the single task, so the kernel buffer accumulated stale frames and `g2g` grew unbounded (one experiment hit 17 *seconds* before we reverted).

The fix wasn't a TCP knob, it was the task architecture. `d1c8d45` split `handle_client` into a **dedicated recv-task** (owns the socket, drains continuously) and a **decode-task** (waits on a binary semaphore), with a **3-buffer PSRAM ring**: recv-task fills one buffer, decode-task processes a second, the third is either free or holds a pending frame between them. The ring guarantees recv-task never blocks waiting on decode, and a 1-deep latest-frame slot lets the receiver skip-oldest if decode ever falls behind (mirror of the Scrypted-side skip-oldest in `e5acf93`). After the split, recv-task is busy ~30% of the time and decode-task is idle ~80% — the wire is now the only thing setting the rate, and at 24 fps source it is genuinely keeping up.

The full instrumentation that drove the diagnosis (`queued_at_body_start`, `recv_calls`, `recv_chunk_min/avg/max`, `recv_dropped_oldest`, `decode_idle_*`, `so_rcvbuf`) is still in `/state` and the windowed log.

### TCP window + EMAC tuning (the follow-up the task split unlocked)

With recv on its own task and a skip-oldest slot, the window became safe to raise — but "safe" had to be proven, so first the stream gained a decomposition that accounts for the whole frame interval: `interval ≈ hdr_gap (sender idle) + recv (wire) + pend_age (handoff wait) + dec + paint`, plus `wire_*_kbps` — the instantaneous throughput while a body drains, whose ceiling is `TCP_WND / RTT`, making it the definitive "is the window the limiter" metric.

Measured at the default `TCP_WND=5760` (~190 KB frames): wire pinned at 53 Mbps vs ~94 Mbps line rate on the 10/100 PHY, `recv_chunk_max` at exactly 5760, sender backpressured on half its frames — window-bound, three ways.

Raising to `TCP_WND=23040` alone **regressed** (fps 20 → 14, 200–450 ms recv stalls): the EMAC RX DMA pool (20 × 512 B = 10 KB) was smaller than the in-flight window, so a full-window burst overran the RX descriptors, the burst tail dropped with no dup-ACKs behind it, and the sender waited out ~200 ms min-RTO recoveries. **Invariant: the EMAC RX pool must exceed `TCP_WND`.** With `ETH_DMA_BUFFER_SIZE=1600` (one MSS frame per buffer/descriptor) × 24 = 38.4 KB, the stall tail vanished: wire 74 avg / 84 max Mbps, recv 21 ms, painted fps +19%, g2g 73 → ~40–60 ms.

Stopped there deliberately: the interval is now ~half `hdr_gap` (sender has nothing ready), so bigger windows buy little until the source rate rises. `pend_age_*` remains the tripwire — it must stay in microseconds; growth means the kernel-queue latency backlog that killed the pre-split 65535 experiment is back.

### Current backlog, in rough priority order

1. **(was) Network body shrink — done.** The receiver now drains near line rate without blocking decode: TCP window raised to 23040 with the EMAC RX pool sized above it (see *TCP window + EMAC tuning*), wire 74 Mbps avg. Source rate (Unifi substream `medium-resolution` ≈ 24 fps) is the cap.
2. **(was) OTA firmware updates — done.** `POST /firmware` ships in `175dd50`, ota_0/ota_1 alternation with `BOOTLOADER_APP_ROLLBACK_ENABLE` for first-boot revert. `curl --data-binary @build/scrypted-viewport.bin http://<viewport>/firmware` reboots into the new image.
3. **Task watchdog + crash counters** — *low effort*. Enable the ESP-IDF task watchdog, surface its bite count in `/state` alongside the existing `decode_errors` / `state_post_failures`. Good hygiene.
4. **Multi-camera per viewport** — *medium effort*. Let one viewport listen to events from N cameras, picking which one to stream based on which fired. Useful for "show whichever doorbell rang" or zone monitoring.
5. **Boot info-screen flash polish** — *low effort*. Keep the brief wake-on-boot (the FT5426 needs it to start reporting touches) but smoothen the ~600 ms flash so it doesn't visibly flicker on power-up.
6. **Production sealing** — *eventual*. Configurable LAN scope (cross-VLAN, mDNS-via-Unicast), Scrypted-side mutual auth, replay protection for `/state` callbacks.

## Philosophy

Scrypted Viewport is a thin network framebuffer appliance.

ESP:
- DHCP
- mDNS
- HTTP server (`/state`, `/config`, `/frame`, `/firmware`)
- MJPEG stream server (raw TCP socket on `:81`)
- HTTP client (outbound `/state` POSTs to Scrypted)
- JPEG decode
- framebuffer
- touch

Everything else belongs in Scrypted.

> If it changes **what** should be shown, it belongs in Scrypted.
> If it changes **how** pixels reach the screen, it belongs in the device.
