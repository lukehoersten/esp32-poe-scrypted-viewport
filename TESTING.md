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

**Acceptance**: panel shows a test pattern; backlight toggles via API.

**How to verify**: TBD (write after M3 implementation).

**Status**: ⬜ pending.

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

Also verify validation: `idle_timeout_ms: 1000` (below 5000) returns 400; `orientation: "sideways"` returns 400; etc.

**Status**: ⬜ pending.

---

## M5 — JPEG Frame Push

**Acceptance**: `POST /frame` paints an 800x480 (or 480x800 portrait) JPEG.

**How to verify**

```bash
# After /config sets state=wake (M6 dep — for now test post-/wake or hardcode awake):
curl -X POST -H "Content-Type: image/jpeg" \
  --data-binary @test-480x800.jpg \
  http://<device-ip>/frame
```

Visual: matching pattern appears on the panel. Re-test in landscape orientation with `test-800x480.jpg`.

Negative tests:
- Non-JPEG body → 400.
- 1.1 MB body → 413.
- 640x480 body (wrong size) → 400 or visible distortion (depends on decoder strictness).

**Status**: ⬜ pending.

---

## M6 — State + Idle Timer

**Acceptance**: `POST /state` toggles wake/sleep; `/frame` is 409 when asleep; idle timer fires after `idle_timeout_ms`.

**How to verify**

```bash
curl -X POST -d '{"state":"sleep"}' http://<device-ip>/state   # backlight off
curl -X POST -d '{"state":"wake"}'  http://<device-ip>/state   # backlight on, loading

# /frame while asleep:
curl -X POST -d '{"state":"sleep"}' http://<device-ip>/state
curl -i -X POST -H "Content-Type: image/jpeg" \
  --data-binary @test-480x800.jpg \
  http://<device-ip>/frame
# expect: HTTP/1.1 409 Conflict

# Idle timer (assuming default 60000ms):
curl -X POST -d '{"state":"wake"}' http://<device-ip>/state
# wait 65s without sending /frame
curl http://<device-ip>/state | jq .state
# expect: "asleep"
```

**Status**: ⬜ pending.

---

## M7 — Touch + Outbound `/state` POST

**Acceptance**: tap toggles wake/sleep locally; device POSTs `{viewport,state}` to `<scrypted>/state`.

**How to verify**

Stand up a test HTTP receiver:

```bash
# In a separate terminal on Scrypted host:
python3 -m http.server 11080  # or a small flask app that logs body
```

Configure the device to point at it:

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"viewport":"mudroom","scrypted":"http://<host>:11080"}' \
  http://<device-ip>/config
```

Then:
- Tap while asleep → device wakes, receiver logs `POST /state {"viewport":"mudroom","state":"wake"}`.
- Tap while awake → device sleeps, receiver logs `POST /state {"viewport":"mudroom","state":"sleep"}`.
- Wait `idle_timeout_ms` with no `/frame` → receiver logs same sleep POST.
- Kill receiver, tap → `/state` `state_post_failures` increments.

**Status**: ⬜ pending.

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
