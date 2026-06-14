# Scrypted-side script (v1)

The ESP32 firmware needs something on the Scrypted side that:
- registers each viewport via `POST /config` on startup,
- receives device-initiated `POST /state` callbacks (tap, idle timeout),
- starts a JPEG stream to a viewport when its bound camera fires an event (doorbell ring, motion, person), and
- stops the stream when the viewport reports `sleep` or its own per-stream timer expires.

`scrypted-viewport.ts` in this directory does all of that as a single-file TypeScript script for the **Scripts plugin**. Each viewport is a child Scrypted device under the script — you add, remove, and edit viewports entirely through the Scrypted UI; no script editing required after the initial paste.

v2 will replace this with a packaged plugin doing `POST /stream` over MJPEG; for now this is enough to bring up end-to-end functionality.

## Install

1. In Scrypted's web UI: **Plugins → search "Scripts" → install** (if you don't already have it).
2. **+ Add Device → Scripts plugin → New Script**.
3. Paste the entire contents of `scrypted-viewport.ts` into the editor and **Save**.
4. Open the newly-created "Scrypted Viewport" device.

The script is now running. Time to add viewports.

## Adding a viewport

On the "Scrypted Viewport" device page, click **+ Add Device**. You'll get a small form:

| Field | What to enter |
| --- | --- |
| **Viewport name** | Lowercase routing key, e.g. `mudroom`. Becomes the device's mDNS hostname (`viewport-mudroom.local`) and the value the firmware sends back in callbacks. |
| **IP or hostname** | Optional. Leave blank and the script auto-resolves `viewport-<name>.local` via the OS mDNS resolver (Bonjour on macOS, nss-mdns on Linux, host networking in Docker). Set manually if mDNS resolution isn't available in your network. |
| **Camera** | Dropdown — pick the camera whose events should wake this viewport. The dropdown is filtered to devices implementing `Camera`. |
| **Orientation** | `portrait` (480×800, default) or `landscape` (800×480). Tells the device + script what dimensions to send. |

Click **Create**. The script immediately POSTs `/config` to the device. Within a second or two, the viewport device should show up in Scrypted with its own page.

## Editing a viewport

Open the viewport device's page. It has its own Settings tab with the same four fields, plus two extras:

- **Idle timeout (ms)** — sent to the device in `/config`. Both sides time independently. `0` disables the device-side idle timer; non-zero must be ≥ 5000. Default 60000.
- **Brightness (0–100)** — gamma-corrected on the panel. Default 80.

Changing any setting triggers an immediate re-register and re-subscribes the camera listener if the camera changed.

## Removing a viewport

Open the viewport device's page → **Settings** menu → **Delete Device**. The script stops any active stream, unsubscribes from the camera, and forgets the binding. The physical device keeps its NVS-stored config until it gets a fresh `/config` from somewhere — to fully wipe it, plug USB and run `idf.py erase-flash` then reflash.

## Global tuning

Open the parent "Scrypted Viewport" device's Settings page:

- **Frame push interval (ms)** — how often a snapshot is pushed during an active stream. 1000 = 1 fps. Lower for faster updates at higher CPU cost.

## What the script does on each event

- **Camera event (doorbell ring / motion / person)** → look up the viewport bound to that camera → `startStream(viewport)`:
  - cancel any prior stream + safety timer for that viewport,
  - POST `{state: "wake"}` to the device,
  - start the snapshot interval,
  - arm a per-stream safety timer at the viewport's `idle_timeout_ms`.
- **Device-initiated `{state: "wake"}` callback** (operator tapped the panel) → same `startStream` path.
- **Device-initiated `{state: "sleep"}` callback** → `stopStream` without echoing sleep back (the device already knows).
- **Per-stream safety timer fires** → `stopStream` and POST `{state: "sleep"}` to the device.
- **`POST /frame` returns 409** → device went to sleep on its own (tap-to-sleep, or its idle timer); `stopStream` without echo.

## What the script does on script load + every 5 minutes

- Re-POST `/config` to every known viewport with its current settings. A device that rebooted or got a new DHCP lease re-syncs within 5 minutes without manual intervention.

## Local type-checking (optional)

If you want red squigglies in your editor instead of just trusting the runtime:

```bash
cd scrypted
npm install
```

That pulls in `@scrypted/sdk` and `@types/node` so the TS server can resolve everything. Nothing here is shipped — install is still "paste into Scrypted's web UI."

## v1 limitations

- Snapshot-rate only (~1 fps). Live MJPEG over `POST /stream` is v2.
- Manual IP per viewport (no mDNS-SD discovery yet). DHCP reservation is the simplest workaround.
- Camera must respect `picture.width` / `picture.height` in `PictureOptions` or be paired with a snapshot plugin that resizes. If the camera returns the wrong size, the device rejects `/frame` with 400 and you'll see warning logs. Workaround: configure the camera plugin's snapshot size, or wait for v2 (FFmpeg-side resize).
- No retry on transport errors. Best-effort matches the device's own semantics; the next event or callback re-syncs.

## How mDNS auto-resolve works

Every time the script registers a viewport (on plugin start, every 5 minutes, on settings change, after a fresh `+ Add Device`), it tries `dns.lookup('viewport-<name>.local')` first. If the OS returns an IP, that IP overwrites the `host` field in the viewport's storage. Subsequent `POST /config` and `POST /frame` use the resolved IP.

If the OS resolver doesn't know about `.local`:
- **macOS**: works out of the box (Bonjour).
- **Linux**: install `libnss-mdns` and ensure `mdns` is in `/etc/nsswitch.conf`'s `hosts:` line.
- **Docker**: use `--network host` (Scrypted's recommended setup). Bridge networking breaks `.local` resolution.

When mDNS doesn't resolve, the script silently falls back to the operator-entered `host`. You can disable mDNS per viewport via the **Auto-resolve via mDNS** toggle on its Settings page.

## End-to-end smoke test

After installing the script and adding one viewport binding:

1. **Fresh viewport** boots → info screen on panel.
2. Script's `getDevice()` runs on script start → `POST /config` lands → viewport → `state: asleep`, backlight off.
3. **Tap the viewport** → device POSTs `{viewport, state: "wake"}` → script logs `recv "<name>" -> wake` → snapshots start flowing for `idle_timeout_ms`.
4. **Tap again** → device POSTs `{state: "sleep"}` → script stops streaming.
5. **Trigger the bound camera** (doorbell, motion sensor, or person detection) → script POSTs `{state: "wake"}` → snapshots flow until either side's idle timer cuts off.
