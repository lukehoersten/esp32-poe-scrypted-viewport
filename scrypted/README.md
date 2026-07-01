# Scrypted-side script (v1)

The ESP32 firmware needs something on the Scrypted side that:
- registers each viewport via `POST /config` on startup,
- receives device-initiated `POST /state` callbacks (tap, idle timeout),
- starts a live MJPEG stream to a viewport when its bound camera fires an event (doorbell ring, motion, person), and
- stops the stream when the viewport reports `sleep` or its own per-stream timer expires.

The stream is real video: one `ffmpeg` child pulls the camera's substream, scales/rotates to panel-native 800×480, and pipes MJPEG frames straight to the firmware over a raw TCP data socket (port 81) at ~24 fps — no per-frame HTTP.

`scrypted-viewport.ts` in this directory does all of that as a single-file TypeScript script for the **Scripts plugin**. Each viewport is a child Scrypted device under the script — you add, remove, and edit viewports entirely through the Scrypted UI; no script editing required after the initial paste.

v2 will repackage this single-file script as a proper installable plugin; the streaming path is already in place.

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
| **IP or hostname** | The device's LAN address — an IP or a hostname string (e.g. `viewport-<mac>.local`). Set it manually; the script POSTs to this string directly and does **not** auto-resolve mDNS. See [Finding a viewport's IP / hostname](#finding-a-viewports-ip--hostname). |
| **Camera** | Dropdown — pick the camera whose events should wake this viewport. The dropdown is filtered to devices implementing `Camera`. |
| **Orientation** | `portrait` (480×800, default) or `landscape` (800×480). Tells the device + script what dimensions to send. |

Click **Create**. The script immediately POSTs `/config` to the device. Within a second or two, the viewport device should show up in Scrypted with its own page.

## Fast wake — camera prebuffer (required)

Wake-to-live-video is ~0.7 s **only if** the streamed substream keeps a rebroadcast prebuffer; otherwise the panel waits a full keyframe interval (~5 s on Unifi) for the next IDR before the first frame decodes. This is a per-camera Scrypted setting, not something the script can set:

1. Open the **camera device** in Scrypted → **Stream Management** (the `@scrypted/prebuffer-mixin` / rebroadcast settings).
2. Select the **STREAM: MEDIUM** tab (the script streams the smallest substream that still covers the panel — usually "Medium" — and downscales to 800×480, so its native resolution doesn't matter). **Enable Prebuffer** on it. By default many cameras only prebuffer the High stream (for HomeKit/NVR); Medium/Low have none.
3. The prebuffer duration must be **≥ the camera's keyframe interval** so the buffer always contains a keyframe. That tab shows a read-only **"Detected Keyframe Interval"** (e.g. 5.044 s on the Unifi doorbell) — the enabled default of 10 s is comfortably above it. The keyframe interval itself is usually **not** settable in the camera's own app (e.g. Unifi Protect).
4. On the viewport's Settings, leave **Stream prebuffer (ms)** at its default (`6000`, i.e. above the ~5 s GOP). If it was ever saved as `0`, clear/reset it — a stored `0` disables the prebuffer request.

To verify: on a wake, the plugin log's `substream=` shows `id:N(prebuffered)` and `usingPrebuffer=true`, and `first ffmpeg frame` lands at ~0.7 s instead of ~5–6 s. If you see `usingPrebuffer=false` or a ~5 s first frame, the chosen substream isn't prebuffered — revisit step 2.

## Editing a viewport

Open the viewport device's page → **Settings**. The fields are grouped:

**Binding**
- **IP or hostname** — where the firmware lives on the LAN (set manually; no mDNS auto-resolve).
- **Camera** — which Scrypted camera drives the wake events and provides the video stream.
- **Wake triggers** — multi-select of `doorbell`, `motion`, `person`. Default: **person + doorbell** (motion is opt-in — doorbell cameras fire it constantly). The `doorbell` option only appears for doorbell-capable cameras. Clear all of them for tap-only mode (the viewport never wakes from Scrypted; user must tap the panel to see the camera).

**Display**
- **Orientation** — `portrait` (480×800) or `landscape` (800×480). Sent to the device in `/config`.
- **Brightness (0–100)** — gamma-corrected on the panel. Default 100.
- **Idle timeout (ms)** — how long the device stays awake after the last paint before it sleeps itself. `0` disables; non-zero must be ≥ 5000. Default 60000.
- **JPEG quality (1–31, lower = better)** — ffmpeg mjpeg `-q:v`. 1 ≈ visually lossless (~140 KB/frame); 5 ≈ good (~70 KB); 10+ noticeably lossy. Default 1.
- **Max Scrypted-side buffer (MB)** — emergency cap on Node's TCP send queue for the stream socket; skip-oldest backpressure normally keeps this near one in-flight frame. Default 20.
- **Stream prebuffer (ms)** — cold-start mitigation; see [Fast wake — camera prebuffer](#fast-wake--camera-prebuffer-required). Default 6000.

**Actions** — momentary toggles: **Wake now** POSTs `{wake}` and starts a stream; **Sleep now** stops the stream and POSTs `{sleep}`. Each resets itself after firing.

**Status (live)** — read-only fields that fetch `/state` + `/config` from the device every time you open the Settings page: name, MAC, IP, awake/asleep, configured flag, uptime, frame counters, error counters, resolution, free heap, free PSRAM, firmware version, and the registered scrypted callback URL. If the device is offline you'll just see "device: offline / unreachable".

Changing any binding setting triggers an immediate re-register + re-subscribes the camera listener if the camera changed.

## Removing a viewport

Open the viewport device's page → **Settings** menu → **Delete Device**. The script stops any active stream, unsubscribes from the camera, and forgets the binding. The physical device keeps its NVS-stored config until it gets a fresh `/config` from somewhere — to fully wipe it, plug USB and run `idf.py erase-flash` then reflash.

## What the script does on each event

- **Camera event (doorbell ring / motion / person)** → look up the viewport bound to that camera → `startStream(viewport)`:
  - cancel any prior stream + safety timer for that viewport,
  - POST `{state: "wake"}` to the device,
  - open the TCP data socket and spawn the ffmpeg child that streams MJPEG frames,
  - arm a per-stream safety timer at the viewport's `idle_timeout_ms`.
  - Events that arrive while a stream is already live or starting are ignored — the wake window is anchored to the first event and isn't extended or restarted.
- **Device-initiated `{state: "wake"}` callback** (operator tapped the panel) → same `startStream` path.
- **Device-initiated `{state: "sleep"}` callback** → `stopStream` without echoing sleep back (the device already knows). This is how the script learns the device slept itself (tap-to-sleep or its own idle timer).
- **Per-stream safety timer fires** → `stopStream` and POST `{state: "sleep"}` to the device.

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

- Packaged as a single-file Scripts paste rather than an installable plugin (v2 repackages it). Updating means re-pasting the file.
- Manual IP per viewport (no mDNS-SD discovery). A DHCP reservation is the simplest way to keep it stable.
- Fast wake depends on a rebroadcast prebuffer on the streamed substream — see [Fast wake — camera prebuffer](#fast-wake--camera-prebuffer-required). Without it the stream still works but the first live frame waits a full keyframe interval (~5 s).
- No retry on transport errors. Best-effort matches the device's own semantics; the next event or callback re-syncs.

## Finding a viewport's IP / hostname

The script does NOT auto-resolve mDNS — it just POSTs to the host string you enter. The host field accepts either an IP or a hostname (the OS resolver handles `.local` lookups inside the Scrypted container).

On a fresh device the mDNS hostname is `viewport-<mac>.local` (the MAC with colons stripped). You can read the MAC straight off the device's info screen at boot, or discover it from your shell:

| OS | Browse all viewports on the LAN | Resolve a hostname → IP |
|---|---|---|
| macOS | `dns-sd -B _scrypted-viewport._tcp local.` | `dns-sd -G v4 viewport-<mac>.local` |
| Linux (with `avahi`) | `avahi-browse -tr _scrypted-viewport._tcp` | `avahi-resolve -n viewport-<mac>.local` |

Once you've POSTed `/config` with a friendlier name (e.g. `mudroom`) via the kitchen viewport's settings, the device re-advertises as `viewport-mudroom.local` and you can use that instead. The MAC-derived hostname stays available as a fallback.

If Scrypted runs in a Docker container without host networking, `.local` resolution won't reach the LAN — enter a static IP and add a DHCP reservation so it stays stable.

## End-to-end smoke test

After installing the script and adding one viewport binding:

1. **Fresh viewport** boots → info screen on panel.
2. Script's `getDevice()` runs on script start → `POST /config` lands → viewport → `state: asleep`, backlight off.
3. **Tap the viewport** → device POSTs `{viewport, state: "wake"}` → script logs `recv "<name>" -> wake` → the live stream flows for `idle_timeout_ms`.
4. **Tap again** → device POSTs `{state: "sleep"}` → script stops streaming.
5. **Trigger the bound camera** (doorbell, motion sensor, or person detection) → script POSTs `{state: "wake"}` → panel shows "Loading…" then live video (~0.7 s with prebuffer) until either side's idle timer cuts off.
