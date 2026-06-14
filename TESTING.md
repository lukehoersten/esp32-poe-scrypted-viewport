# Testing & Verification

This file is the self-contained reference for verifying the firmware on real hardware. It has three layers:

1. **Status snapshot** — at-a-glance state of every milestone.
2. **Hardware prerequisites** — what to confirm or order before flashing anything.
3. **Recommended bench order** — the sequence to flip 🟡 → ✅, stage by stage.
4. **Per-milestone entries** (M1–M9) — acceptance criteria + exact commands/actions + status.
5. **Integration & system tests** — to run after every per-milestone test passes.

Status legend:

- ⬜ not verified — code not yet exercised at all.
- 🟡 partial — compiles cleanly, builds against ESP-IDF 5.4 for `esp32p4`, but not yet run on a board.
- ✅ verified — confirmed on hardware. Annotate with the date and board rev.

---

## Status snapshot

| # | Milestone | Code | HW |
| --- | --- | --- | --- |
| M1 | Board Bring-Up — Ethernet + DHCP | ✅ | ✅ |
| M2 | HTTP + mDNS (`GET /state`) | ✅ | ✅ |
| M3 | Display Bring-Up (Hosyond panel) | ✅ | ✅ |
| M4 | Config Persistence (NVS, partial updates) | ✅ | ✅ |
| M5 | JPEG Frame Push (`POST /frame`) | ✅ | ✅ |
| M6 | State + Idle Timer (`POST /state`, 409 guard) | ✅ | ✅ |
| M7 | Touch + Outbound `/state` POST | ✅ | ✅ |
| M8 | Local Screens + touch long-press | ✅ | ✅ |
| M9 | Live Stream (`POST /stream`) | ⬜ | ⬜ |

---

## Hardware prerequisites

Before flashing anything, confirm or acquire:

| Item | Status as of this writing | How to resolve |
| --- | --- | --- |
| Waveshare ESP32-P4-ETH board | shipped | check silkscreen for flash size (16 vs 32 MB) — adjust `sdkconfig.defaults` if 32 |
| Hosyond 5" 800×480 panel | shipped | n/a |
| 15-pin Pi-FPC → Waveshare-side DSI adapter cable | **unknown — order before M3** | check Waveshare board DSI connector pin count, buy matching adapter |
| 4× jumper wires (5V / GND / SDA / SCL) | trivial | bench supply |
| Ethernet cable + DHCP-serving switch | trivial | LAN already in place |
| (optional) PoE injector | nice-to-have | for M1/M2 a USB-only power path is fine |

Hardware unknowns — resolved:

1. **DSI FPC pin count on the Waveshare board.** ✅ 22-pin DSI on board; bridge to the panel's 15-pin Pi FPC via a 15→22 adapter cable.
2. **I²C jumper destinations.** ✅ `PIN_I2C_SDA=7`, `PIN_I2C_SCL=8` confirmed by ATTINY ack at 0x45 + FT5426 ack at 0x38.
3. **BOOT button GPIO.** ✅ No usable GPIO — ESP32-P4 GPIO 35 strap pin is owned by EMAC TXD1 at runtime; no separate user button is wired on the Waveshare ESP32-P4-ETH. Info-overlay behaviour moved onto touch long-press (≥1.5 s). No factory-reset gesture — that path goes through USB reflash + `idf.py erase-flash`.
4. **Flash size silkscreen.** `sdkconfig.defaults` declares 16 MB. If your SKU shipped with 32 MB the build still works but you waste half the flash.

---

## Recommended bench order

Work through these stages in order. Each stage builds on the previous one's verified state.

### Stage 1 — board comes alive (M1 + M2)

No display, no panel, no jumpers. Just board on Ethernet over USB.

```bash
source ~/Dev/code/git/esp32/env.sh
idf.py -p /dev/cu.usbmodem* flash monitor
```

Check serial for `net_eth: got ip` then `curl http://<ip>/state | jq .` and `dns-sd -B _scrypted-viewport._tcp local.`. If both pass, flip M1 + M2 to ✅. Stop here if you hit any issue — debug Ethernet before adding more variables.

### Stage 2 — display panel (M3) ✅

Look for `panel MCU id 0xc3 — Pi v1.1 architecture ack'd`, `TC358762 bridge configured`, and `DSI up: 800x480 26 MHz, 1-lane 600 Mbps, non-burst` in the serial log. Info screen visible on tap-wake.

### Stage 3 — protocol (M4 + M5 + M6)

No new wiring. POST `/config`, send a JPEG, exercise wake/sleep, watch the idle timer fire. Run through the per-milestone curls below.

### Stage 4 — touch (M7)

Tap the screen. Confirm the FT5426 acks on I²C in the boot log. Stand up the flask receiver, configure the device to point at it, and watch outbound `/state` POSTs arrive.

### Stage 5 — local screens + BOOT (M8)

Visual: fresh boot shows IP screen; wake shows loading screen; tap-press of the BOOT button overlays IP. If BOOT GPIO is wrong, the screens still work — only the button feature stays cold.

### Stage 6 — end-to-end with Scrypted

Paste `scrypted/scrypted-viewport.ts` into Scrypted's Scripts plugin, add a viewport device with the binding (camera picker dropdown), trigger the bound camera, watch the panel light up.

### Stage 7 — integration

Run the suite at the bottom of this file (races, failures, longevity, etc.).

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

**Status**: ✅ verified 2026-06-14 on Waveshare ESP32-P4-ETH (Amazon B0FN7JQ2V8, 32 MB PSRAM, 32 MB flash silkscreen, IDF v5.4.1). DHCP lease landed at 10.0.13.83. Three fixes were needed during bring-up — see commit 220ee4c:
> 1. **RXD0 / RXD1 GPIO swap**: ESP32-P4 EMAC iomux fixes RXD0=29, RXD1=30. The original pin map had them transposed.
> 2. **mDNS hostname order**: `mdns_service_add()` returns INVALID_ARG if hostname isn't set yet. Now init → hostname → service_add → TXT.
> 3. **`app_main` best-effort everywhere**: any one missing subsystem (no LAN, no panel) used to abort the boot. Now each is independent; final log line summarizes which letters of `EMHDJTB` came up.

**Link-loss & recovery also verified 2026-06-14** — important for PoE-only deployments where a switch reboot or briefly down VLAN won't kill power but will yank the link:

```
W (748316) net_eth: link down                                # cable pulled
I (916316) net_eth: link up, mac e8:f6:0a:e0:90:94          # cable plugged back in (3 min later)
I (924816) net_eth: got ip 10.0.13.83 gw 10.0.13.1 netmask 255.255.255.0   # DHCP renewed ~8.5s after link-up
```

Same boot throughout (uptime kept climbing past the disconnect, no reset). mDNS responder task and HTTP server stayed alive through the outage and resumed serving the moment DHCP came back. `free_heap` / `free_psram` identical to pre-disconnect — no leak from the cycle.

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

Expected `/state` body on a fresh device (no `/config` posted yet):

```json
{
  "name": null,
  "version": "0.1.0",
  "configured": false,
  "state": "asleep",
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

**Status**: ✅ verified 2026-06-14 alongside M1.

- `GET http://10.0.13.83/state` returned the full spec JSON: `name=null, configured=false, state=asleep, resolution=480x800, ip=10.0.13.83, free_psram=31730048, ...`.
- `dns-sd -B _scrypted-viewport._tcp local.` on macOS surfaced one instance named `viewport`.

Bare-board behavior (no panel, no ethernet) was also verified — the device boots cleanly and prints a summary like `boot complete — subsystems [EMHdJ-B]  ip=(no link)` (lowercase `d` = display down because no panel attached; `-` for touch because it shares the panel I²C bus).

---

## M3 — Display Bring-Up

**Acceptance**: panel powers on, MCU at I2C 0x45 responds, color-bar test pattern renders, brightness control works.

**Wiring** (Hosyond 5" 800x480 panel ↔ Waveshare ESP32-P4-ETH)

The Hosyond panel exposes only the 15-pin Pi DSI FPC — no auxiliary GPIO header. **Everything goes through the FPC**: DSI data lanes, panel power (3.3V / 5V / GND), and the I²C bus (SDA / SCL) shared by the on-panel ATTINY MCU at `0x45` and the FT5426 touch at `0x38`. No jumper wires required.

Verified board-side mapping (Waveshare ESP32-P4-ETH wiki + ESPHome device page):

| Signal | DSI FPC | ESP32-P4 GPIO |
| --- | --- | --- |
| DSI data lanes / clock | dedicated DSI PHY | (fixed silicon pins) |
| I²C SDA | FPC pin | **GPIO 7** |
| I²C SCL | FPC pin | **GPIO 8** |
| 3.3V / 5V / GND | FPC pins | board rails (no GPIO) |
| Backlight | I²C register `0x96` on the panel MCU | (no GPIO) |
| Panel reset / power-on | I²C `REG_POWERON` (`0x85`) on the panel MCU | (no GPIO) |

`PIN_I2C_SDA=7` / `PIN_I2C_SCL=8` in `display.c` already match this. **No code changes needed before flashing.**

DSI connector: 22-pin (Pi 5 / CM4 style). The 15-pin Pi DSI FPC on the panel plugs into a **15→22 pin adapter cable** that goes into the board.

**Bring-up sequence**

1. Power the ESP32-P4 board (USB is fine for bench; PoE/12V in production).
2. Plug the **15-pin** side of the DSI cable into the Hosyond panel.
3. Plug the **22-pin** side into the Waveshare DSI connector. (Pi FPCs are easy to install upside-down — make sure the contact side faces the connector lock.)
4. Reset the board (or `idf.py flash monitor` if you've changed firmware):

```bash
idf.py -p /dev/cu.usbmodem* monitor
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

- `panel MCU @0x45 unreachable` → either the FPC seating / orientation is wrong (most common — Pi FPCs are easy to install upside-down on either end), the adapter cable doesn't pass I²C straight through, or the panel is one of the rare Waveshare-bundled units with **GT911 touch at 0x5D/0x14** instead of the FT5426 Pi 7" architecture this driver targets (in which case the *panel MCU* check still fails because that variant has no ATTINY at all). No need to suspect DSI yet.
- I²C ack but no image → DSI cable orientation on the 22-pin side, or `LCD_COLOR_PIXEL_FORMAT_RGB565` byte order if there's something on the panel but garbled.
- Image but wrong colors → RGB565 byte order; flip to BGR variant.
- Image but vertical/horizontal sync issues → adjust the `PANEL_*SYNC_*` timings in `display.c`; Pi 7" canonical values are the defaults.
- Touch ack at `0x38` but no taps → polling task not running, or panel reset state wrong.

**Status**: ✅ verified 2026-06-14 on Waveshare ESP32-P4-ETH + Hosyond 5" panel. ATTINY v1.1 firmware (ID 0xC3) acks; TC358762 bridge configured via 16 DSI Generic Long Writes; DSI 1-lane @ 600 Mbps non-burst; DPI 26 MHz RGB888; info screen renders correctly. Bring-up notes captured in commit 6c1a26b.

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

**Status**: ✅ verified 2026-06-14. Full POST + partial POST + 5 validation failure modes all returned correctly. Config survives reboot (NVS persistence). `configured: true` was set automatically once both `viewport` and `scrypted` were supplied. Required raising httpd stack from 4 KiB to 8 KiB — the handler's 2 KiB body buffer plus locals were overflowing into the protect page.

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

**Status**: ✅ verified 2026-06-14. Hardware JPEG decode + paint works end-to-end. Fixing it surfaced two stacked channel-swap bugs (commit 11ad249): the IDF JPEG decoder's `_RGB` element-order emits the RGB565 word in big-endian byte order (we now use `_BGR` for LE), and the ESP32-P4 DSI + TC358762 + Pi panel pipeline wants `[B, G, R]` in memory (we now pack the RGB888 framebuffer accordingly). Symmetric pixels like the white-on-black info screen masked both bugs; saturated solid colours exposed them.

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

`POST /state` always works regardless of whether `/config` has been posted — `state` is just the screen's awake/asleep, decoupled from the `configured` flag. Outbound POST-to-Scrypted is the part gated on a scrypted URL being present.

Bad input:

```bash
curl -i -X POST -d '{"state":"middle"}' \
  -H "Content-Type: application/json" http://<device-ip>/state    # 400
curl -i -X POST -d 'not json' \
  -H "Content-Type: application/json" http://<device-ip>/state    # 400
```

**Known gap (M7 closes this)**: when the idle timer fires the device transitions to ASLEEP locally but does NOT yet POST `{viewport,state:sleep}` to `<scrypted>/state`. That outbound POST lands with `state_client` in M7.

**Status**: ✅ verified 2026-06-14. Wake/sleep toggle 204, idempotent repeats 204, `/frame` while asleep returns 409 with expected body, bad inputs 400, idle timer fires after `idle_timeout_ms`, `idle_timeout_ms:0` correctly disables the timer.

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

**No POST when no scrypted URL is set**

- Before `/config` provides a Scrypted URL, tapping the screen does nothing outbound (no Scrypted URL to call). No queue entries dropped to disk.

**Status**: ✅ verified 2026-06-14. FT5426 acks at 0x38; short tap toggles awake/sleep, ≥1.5s hold opens the info overlay. Outbound `/state` POST gated on configured flag (untested without Scrypted).

---

## M8 — Local Screens + touch long-press

**Acceptance**: info screen on first boot; loading screen on every wake; long-press shows the info overlay.

**Visual checks (panel-attached)**

- Fresh flash → screen shows the info screen (~15 lines of `label  value` pairs, white on black, auto-scaled). With no `/config` posted it reports `name unset`, `config no`, `state asleep`, `scrypt none`.
- `POST /config` with viewport + scrypted → device transitions to ASLEEP, backlight off.
- `POST /state {state:wake}` (or tap) → backlight on, `Loading...` centered until the first `/frame` lands.
- `POST /frame` while AWAKE → loading screen replaced by the JPEG.
- **Long-press (≥1.5s)** at any state → info overlay for 15 s with the full current config + state dump (name, host, ip, state, config, scrypt, orient, bright, idle, fw, up, frames, errs, heap, psram).
- `POST /state {state:sleep}` (or idle timeout, or tap-while-awake) → backlight off.

**Touch gestures** (no hardware button — see Hardware note)

- Short tap (<500 ms): toggle wake / sleep.
- Long-press (≥1.5 s): info overlay for 15 s.
- No factory-reset gesture. Use USB + `idf.py erase-flash` to clear NVS.

**Hardware note**: ESP32-P4's strap pin GPIO35 is owned by EMAC TXD1 at runtime, and the Waveshare ESP32-P4-ETH doesn't expose any separate user button on a free GPIO. Both BOOT-button behaviours moved onto the touch panel (see M7).

**Negative / edge**

- Info overlay during AWAKE: the overlay paints over the live frame. Scrypted's next `/frame` (within ~1s in a normal stream) overwrites it. Acceptable: the operator sees the info for ≤ 1 frame interval then the live view resumes.
- Info overlay during ASLEEP: backlight comes on for the overlay, then expires back to sleep when the 15 s timer fires.
- Font fallback: any character outside the supported set (digits, period, colon, dash, slash, `L`, lowercase a–z, space) renders as blank. Info-screen labels and IPv4 strings are all covered.

**Status**: ✅ verified 2026-06-14. Info screen renders the full config + state dump; loading screen shown on wake when configured; info overlay returns to prior state when its 15 s timer expires.

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

- Cable pull mid-frame: device idle-sleeps after `idle_timeout_ms`. On reconnect, mDNS re-advertises; Scrypted re-finds and continues. (Link-loss/recovery part verified 2026-06-14 with the device idle — see M1 section. Mid-frame variant still pending.)
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
