# Testing & Verification

This file tracks which milestones have been verified on hardware and how. Per-milestone entries below each list:

- **Acceptance** — what "done" means (mirrors the impl guide).
- **How to verify** — exact commands or actions.
- **Status** — ⬜ not verified / 🟡 partial / ✅ verified, with a one-line note (date, board rev, anything weird).

Hardware-required milestones can't be verified by a clean build alone — a checked-in build success counts as 🟡 (compiles, not yet run).

---

## M1 — Board Bring-Up

**Acceptance**: device gets a DHCP lease over Ethernet and logs its IP.

**How to verify**

```bash
source ~/Dev/code/git/esp32/env.sh
idf.py -p /dev/cu.usbmodem* flash monitor
```

Expected log within ~5–10s of boot:

```
I (xxx) viewport: Scrypted Viewport boot
I (xxx) net_eth: ethernet driver started, waiting for link + DHCP
I (xxx) net_eth: link up, mac xx:xx:xx:xx:xx:xx
I (xxx) net_eth: got ip 192.168.x.x  gw 192.168.x.1  netmask 255.255.255.0
I (xxx) viewport: online at 192.168.x.x
```

Cross-check from another host:

```bash
ping 192.168.x.x
```

**Status**: 🟡 builds clean against ESP-IDF 5.4 (commit e2ac22e). Not yet flashed to hardware — awaiting first board run.

> Combined hardware verification of M1+M2 is fine — they both run from the same flash session.

---

## M2 — HTTP + mDNS

**Acceptance**: `GET /state` returns JSON; mDNS service is discoverable.

**How to verify**

After flash, from a host on the same LAN:

```bash
# Direct call by IP (always works):
curl http://<device-ip>/state | jq .

# mDNS hostname (requires OS-level .local resolution: macOS Bonjour, Linux nss-mdns):
curl http://viewport.local/state | jq .

# Service discovery browse (preferred — what Scrypted will do, no OS .local resolver needed):
dns-sd -B _scrypted-viewport._tcp local.        # macOS
avahi-browse -r _scrypted-viewport._tcp         # Linux
```

Expected `/state` body on a fresh device (unconfigured):

```json
{
  "name": null,
  "version": "0.1.0",
  "configured": false,
  "state": "unconfigured",
  "uptime_ms": 12345,
  "last_frame_ms_ago": null,
  "frames_received": 0,
  "decode_errors": 0,
  "state_post_failures": 0,
  "resolution": "480x800",
  "ip": "192.168.x.x",
  "free_heap": 200000,
  "free_psram": 30000000
}
```

Expected mDNS browse output should show a `_scrypted-viewport._tcp` instance with TXT records `version=`, `resolution=`, `orientation=`, `name=` (empty for `name`).

**Status**: 🟡 builds clean against ESP-IDF 5.4 with espressif/mdns managed component. Awaiting first board run.

---

## M3 — Display Bring-Up

**Acceptance**: panel powers on, MCU at I2C 0x45 responds, color-bar test pattern renders, brightness control works.

**Wiring** (Hosyond 5" 800x480 panel ↔ Waveshare ESP32-P4-ETH):

The DSI FPC adapter (15-pin Pi side → 22-pin Waveshare side) carries the DSI data lanes and power. I2C and panel power are jumpered separately from the ESP32-P4 board to the Hosyond's auxiliary header (the same header Pi users normally connect to the Pi's 40-pin GPIO):

| Hosyond panel pin | Wire to ESP32-P4 board |
| --- | --- |
| 5V | board 5V rail |
| GND | board GND |
| SDA | GPIO 7 |
| SCL | GPIO 8 |

GPIO 7/8 match Waveshare's BSP convention for touch I2C on their bundled-panel kit. If different pins are more convenient, change `PIN_I2C_SDA` / `PIN_I2C_SCL` in `display.c`.

**Bring-up sequence**

1. Power the ESP32-P4 board over USB (don't plug DSI yet).
2. Connect 5V + GND jumpers to the panel. Panel LED (if present) should light.
3. Connect SDA + SCL jumpers.
4. Plug the DSI FPC cable.
5. Flash:

```bash
idf.py -p /dev/cu.usbmodem* flash monitor
```

**Expected log**

```
I (xxx) display: panel MCU id 0xC3 — Pi 7" architecture ack'd
I (xxx) display: panel powered on
I (xxx) display: DSI up: 800x480 30 MHz, 2-lane 480 Mbps
I (xxx) viewport: display up — test pattern on screen
```

**Visual check**: 8 vertical color bars (white, yellow, cyan, green, magenta, red, blue, black) across the panel. Brightness should look perceptually mid-range (default 80/100 with gamma).

**Failure-mode signals**

- `panel MCU @0x45 unreachable` → jumper or pull-up problem on I2C, no need to suspect DSI yet.
- I2C ack but no image → DSI cable / FPC adapter orientation. Pi FPCs are easy to install upside-down.
- Image but wrong colors → check RGB565 byte order; flip `LCD_COLOR_PIXEL_FORMAT_RGB565` to a variant with swap.
- Image but vertical/horizontal sync issues → adjust the `PANEL_*SYNC_*` timings; Pi 7" canonical values used as defaults.

**Status**: 🟡 builds clean against ESP-IDF 5.4. Driver ported from Linux `panel-raspberrypi-touchscreen.c`. Awaiting hardware bring-up with confirmed jumper wiring.

---

## M4 — Config Persistence

**Acceptance**: `POST /config` persists across reboot; partial-update semantics work.

**How to verify**

```bash
# Full config:
curl -X POST -H "Content-Type: application/json" \
  -d '{"viewport":"mudroom","scrypted":"http://host/endpoint/scrypted-viewport","orientation":"landscape"}' \
  http://<device-ip>/config

# Read back:
curl http://<device-ip>/config | jq .

# Partial update (only brightness):
curl -X POST -H "Content-Type: application/json" \
  -d '{"brightness":50}' \
  http://<device-ip>/config

# Reboot, then re-read — values should survive:
curl http://<device-ip>/config | jq .
```

Also verify validation:

```bash
# idle_timeout_ms below 5000 (and non-zero) — expect 400:
curl -i -X POST -H "Content-Type: application/json" \
  -d '{"idle_timeout_ms":1000}' http://<device-ip>/config

# bogus orientation — expect 400:
curl -i -X POST -H "Content-Type: application/json" \
  -d '{"orientation":"sideways"}' http://<device-ip>/config

# brightness out of range — expect 400:
curl -i -X POST -H "Content-Type: application/json" \
  -d '{"brightness":150}' http://<device-ip>/config

# scrypted without http:// — expect 400:
curl -i -X POST -H "Content-Type: application/json" \
  -d '{"scrypted":"scrypted.local"}' http://<device-ip>/config

# Garbage JSON — expect 400:
curl -i -X POST -H "Content-Type: application/json" \
  -d 'not json' http://<device-ip>/config
```

Idle-timer disable (`idle_timeout_ms: 0`) is intentionally allowed.

Side-effects to confirm:
- After `POST /config` with `brightness`: panel brightness changes immediately (if display is up).
- After `POST /config` with `viewport` or `orientation`: mDNS TXT records update; `viewport-<name>.local` resolves; browse shows new TXT.
- After `POST /config` with both `viewport` and `scrypted` (any order, on any subsequent call): `GET /state` shows `configured: true`, `state: "asleep"`.

**Status**: 🟡 builds clean against ESP-IDF 5.4. Logic exercised in code but unverified on hardware.

---

## M5 — JPEG Frame Push

**Acceptance**: `POST /frame` paints a 480x800 (portrait, default) or 800x480 (landscape) JPEG. Orientation determines the expected dimensions and the panel-side rotation.

**How to verify**

```bash
# Default orientation (portrait) — send 480x800:
curl -X POST -H "Content-Type: image/jpeg" \
  --data-binary @test-480x800.jpg \
  http://<device-ip>/frame

# Then switch to landscape and re-test with an 800x480 JPEG:
curl -X POST -H "Content-Type: application/json" \
  -d '{"orientation":"landscape"}' http://<device-ip>/config

curl -X POST -H "Content-Type: image/jpeg" \
  --data-binary @test-800x480.jpg \
  http://<device-ip>/frame
```

Visual: image fills the panel correctly oriented. After paint, `GET /state` shows `frames_received` incremented and `last_frame_ms_ago` populated.

Generate test JPEGs with ImageMagick:

```bash
# 8 vertical color bars at 480x800 portrait:
convert -size 60x800 gradient:white-black -duplicate 7 +append test-480x800.jpg
# or just any 480x800 JPEG:
convert -size 480x800 plasma: test-480x800.jpg
convert -size 800x480 plasma: test-800x480.jpg
```

Negative tests:

```bash
# Wrong Content-Type — expect 400:
curl -i -X POST -H "Content-Type: image/png" --data-binary @anything \
  http://<device-ip>/frame

# Oversize — expect 413:
dd if=/dev/urandom of=big.bin bs=1M count=2
curl -i -X POST -H "Content-Type: image/jpeg" --data-binary @big.bin \
  http://<device-ip>/frame

# Wrong dimensions — expect 400 with "expected WxH, got WxH":
convert -size 640x480 plasma: test-640x480.jpg
curl -i -X POST -H "Content-Type: image/jpeg" --data-binary @test-640x480.jpg \
  http://<device-ip>/frame

# Concurrent posts — second gets 503:
curl -X POST -H "Content-Type: image/jpeg" --data-binary @test-480x800.jpg \
  http://<device-ip>/frame &
curl -i -X POST -H "Content-Type: image/jpeg" --data-binary @test-480x800.jpg \
  http://<device-ip>/frame   # expect 503 if first is still in flight

# Garbage bytes claiming to be JPEG — expect 400:
curl -i -X POST -H "Content-Type: image/jpeg" -d 'not a jpeg' \
  http://<device-ip>/frame
```

After every error: `decode_errors` in `GET /state` should increment.

**Known gap (M6 closes this)**: M5 paints regardless of wake/sleep state. The `/frame` → `409` when asleep rule is added with `POST /state` in M6. Until then, a configured device boots ASLEEP per spec but `/frame` still paints because the state guard isn't wired yet.

**Status**: 🟡 builds clean against ESP-IDF 5.4. Awaiting hardware verification (the test pattern from M3 confirms paint works; M5 adds the JPEG path).

---

## M6 — State + Idle Timer

**Acceptance**: `POST /state` toggles wake/sleep; `/frame` is 409 when asleep; idle timer fires after `idle_timeout_ms`.

**How to verify**

First make sure the device is configured (M4 must be done):

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"viewport":"mudroom","scrypted":"http://host/cb","idle_timeout_ms":10000}' \
  http://<device-ip>/config
```

Wake / sleep:

```bash
# Backlight off:
curl -i -X POST -H "Content-Type: application/json" \
  -d '{"state":"sleep"}' http://<device-ip>/state
# expect: 204

# Backlight on (loading-screen placeholder until M8):
curl -i -X POST -H "Content-Type: application/json" \
  -d '{"state":"wake"}' http://<device-ip>/state
# expect: 204

# Idempotency: repeating either is also 204 with no side effects:
curl -X POST -H "Content-Type: application/json" \
  -d '{"state":"wake"}' http://<device-ip>/state
curl -X POST -H "Content-Type: application/json" \
  -d '{"state":"wake"}' http://<device-ip>/state
# Both: 204
```

`/frame` while asleep returns 409:

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"state":"sleep"}' http://<device-ip>/state
curl -i -X POST -H "Content-Type: image/jpeg" \
  --data-binary @test-480x800.jpg \
  http://<device-ip>/frame
# expect: HTTP/1.1 409 Conflict, body "device asleep — POST /state ..."
```

Idle timer:

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"state":"wake"}' http://<device-ip>/state
sleep 12   # idle_timeout_ms was set to 10000 above
curl http://<device-ip>/state | jq .state
# expect: "asleep"
```

When the timer fires, the serial log should print:

```
I (xxx) state: idle timer expired — sleeping
I (xxx) state: ASLEEP
```

Disabling the idle timer:

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"idle_timeout_ms":0}' http://<device-ip>/config
curl -X POST -H "Content-Type: application/json" \
  -d '{"state":"wake"}' http://<device-ip>/state
sleep 70
curl http://<device-ip>/state | jq .state
# expect: still "awake"
```

Resetting on `/frame`: every successful paint restarts the idle timer.

```bash
# Set short idle, then keep painting to keep awake:
curl -X POST -d '{"idle_timeout_ms":10000}' \
  -H "Content-Type: application/json" http://<device-ip>/config
curl -X POST -d '{"state":"wake"}' \
  -H "Content-Type: application/json" http://<device-ip>/state
for i in 1 2 3 4 5; do
  sleep 8
  curl -X POST -H "Content-Type: image/jpeg" \
    --data-binary @test-480x800.jpg http://<device-ip>/frame
done
curl http://<device-ip>/state | jq .state   # expect "awake"
```

Unconfigured device rejects `POST /state` with 409:

```bash
# (After factory reset / fresh boot, no /config yet)
curl -i -X POST -H "Content-Type: application/json" \
  -d '{"state":"wake"}' http://<device-ip>/state
# expect: 409 Conflict "device unconfigured"
```

Bad input:

```bash
curl -i -X POST -d '{"state":"middle"}' \
  -H "Content-Type: application/json" http://<device-ip>/state    # 400
curl -i -X POST -d 'not json' \
  -H "Content-Type: application/json" http://<device-ip>/state    # 400
```

**Known gap (M7 closes this)**: when the idle timer fires the device transitions to ASLEEP locally but does NOT yet POST `{viewport,state:sleep}` to `<scrypted>/state`. That outbound POST lands with `state_client` in M7.

**Status**: 🟡 builds clean against ESP-IDF 5.4. Awaiting hardware.

---

## M7 — Touch + Outbound `/state` POST

**Acceptance**: tap toggles wake/sleep locally; idle timer firing posts sleep; device POSTs `{viewport,state}` to `<scrypted>/state` for every local transition.

**Test receiver** (run on the host you'll point `scrypted` at):

```bash
# Single-file flask receiver that echoes every POST:
pip install flask
cat > /tmp/recv.py <<'EOF'
from flask import Flask, request
app = Flask(__name__)
@app.post("/state")
def state():
    print("RX", request.get_json())
    return "", 204
app.run(host="0.0.0.0", port=11080)
EOF
python3 /tmp/recv.py
```

Configure the device:

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"viewport":"mudroom","scrypted":"http://<host>:11080","idle_timeout_ms":10000}' \
  http://<device-ip>/config
```

**Tap dispatch**

- Tap while asleep → device wakes; receiver prints `RX {'viewport': 'mudroom', 'state': 'wake'}`.
- Tap while awake → device sleeps; receiver prints same with `state: 'sleep'`.
- Idle timer expires (10s here) → receiver prints `state: 'sleep'`.

Each successful POST also logs on serial:

```
I (xxx) state_client: POST http://<host>:11080/state {state:wake} -> 204
```

**Failure path**

- Stop the receiver, then tap. Serial:

  ```
  W (xxx) state_client: POST http://<host>:11080/state failed: err=ESP_OK status=-1
  ```

  Then `GET /state` shows `state_post_failures` incremented.

- Local state change still happens (backlight toggles). The POST is best-effort, not a precondition.

**Depth-1 queue with replace-on-full**

Tap rapidly (faster than the receiver can ack) and confirm the receiver only sees the most recent state transition between in-flight POSTs. Intermediate flips are coalesced. With a 1s receiver delay, only the final state of each ~1s window should hit the receiver.

**No POST when Scrypted-initiated**

- `curl -X POST -d '{"state":"sleep"}' /state` — receiver should print **nothing** (Scrypted already knows it initiated).
- Same for `/state {wake}` and `/frame` (asleep → 409, no callback either).

**No POST when unconfigured**

- After factory reset, tapping the screen does nothing (no Scrypted URL to call). No queue entries dropped to disk.

**Status**: 🟡 builds clean against ESP-IDF 5.4. Hardware verification needs the FT5426 touch jumpered + reachable on I2C 0x38.

---

## M8 — Local Screens + BOOT button

**Acceptance**: IP screen on first boot; loading screen on every wake; BOOT button works.

**How to verify**

- Fresh flash → screen shows `viewport.local` and the device IP centered.
- `POST /config` → IP screen clears; backlight off (device enters sleep).
- Tap while asleep → "Loading…" screen until next `/frame`.
- BOOT short-press (any state) → IP screen overlays for 15s, then prior state restored.
- BOOT 5s-hold → NVS clears, device reboots, IP screen reappears.

**Status**: ⬜ pending.

---

## M9 — Live Stream (`POST /stream`)

**Acceptance**: ≥ 10 fps multipart MJPEG over a single chunked POST.

**How to verify**: ffmpeg-driven test stream from a host, measure fps from the device's frame counter.

**Status**: ⬜ pending.

---

## Integration & system tests (post-M9)

Run these once all milestones are implemented and individually verified. The point is to exercise races, edge cases, and longevity that single-milestone tests miss.

### A. End-to-end with Scrypted

- Real Scrypted-side Script (M7's scope): doorbell-event → `/state {wake}` → frame stream → idle timer fires → `/state {sleep}` callback → Scrypted stops.
- Bound camera switching: change the binding in Scrypted, confirm next wake routes to the new camera.

### B. Race conditions

- Concurrent tap-while-Scrypted-POSTs-`/state`: spam both, confirm the device converges (mutex serializes; last-write-wins). Use the wake/sleep counters in `/state` to track transitions.
- Mid-stream tap-to-sleep: while Scrypted is streaming frames, tap to sleep. Inflight `/frame` should return 409. Confirm no half-painted frames, no re-wake.
- Idle-timeout coincides with a fresh tap-wake: the second event wins. Verify by repeating with timing jitter.
- Stale Scrypted sleep timer races a fresh wake callback: Scrypted-side `cancelPendingSleep` must work — if a stale sleep lands after a wake, viewport sleeps; user taps again; recovery in one extra tap.

### C. Failure modes

- Cable pull mid-frame: device idle-sleeps after `idle_timeout_ms`. On reconnect, mDNS re-advertises; Scrypted re-finds and continues.
- Scrypted unreachable on tap: device still toggles backlight, `state_post_failures` increments. Recovery: Scrypted comes back, next tap syncs.
- DHCP lease change: device gets new IP, re-advertises via mDNS. Scrypted's periodic browse picks up the new address.
- `/state_post_failures` count should be observable via `GET /state`.

### D. Longevity

- 24h soak with a slow camera stream (~1 frame every 10s): verify no memory leaks (`free_heap` / `free_psram` stable in `/state`), no decode_errors growing.
- Frame storm: 30 minutes at max sustainable fps. Verify watchdog never fires, no resets.

### E. Power & boot

- PoE injector cycle: device boots clean, gets DHCP, re-registers via mDNS, no manual intervention needed.
- Brown-out (PoE on edge of spec): device should reboot cleanly, not corrupt NVS.

### F. Negative protocol

- Garbage JSON to `/config` → 400.
- `/frame` with `Content-Type: image/png` → 400.
- `/state` with `{"state":"middle"}` → 400.
- `/frame` with body > 1 MB → 413.
- Two concurrent `/frame` posts → one wins, the other 503.

### G. Multi-viewport

Once at least two physical units are configured: confirm each routes its callback to Scrypted with its own `viewport` name; confirm Scrypted's discovery map has both IPs; confirm a camera event on the wrong-named viewport is correctly ignored on the right one.
