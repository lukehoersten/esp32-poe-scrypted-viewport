# Scrypted-side script (v1)

The ESP32 firmware needs something on the Scrypted side that:
- registers each viewport via `POST /config` on startup,
- receives device-initiated `POST /state` callbacks (tap, idle timeout),
- starts a JPEG stream to a viewport when its bound camera fires an event (doorbell ring, motion, person), and
- stops the stream when the viewport reports `sleep` or its own per-stream timer expires.

`scrypted-viewport.ts` in this directory does all of that as a single-file TypeScript script for the **Scripts plugin**. v2 will replace this with a packaged plugin doing `POST /stream` over MJPEG; for now this is enough to bring up end-to-end functionality.

## Install

1. In Scrypted's web UI: **Plugins → search "Scripts" → install** (if you don't already have it).
2. **+ Add Device → Scripts plugin → New Script**.
3. Paste the entire contents of `scrypted-viewport.ts` into the editor and **Save**.
4. Edit the `BINDINGS` array at the top of the script for your setup:

   ```ts
   const BINDINGS: Binding[] = [
       {
           name: "mudroom",                            // matches /config viewport
           host: "192.168.1.42",                       // viewport IP (LAN-resolvable)
           cameraId: "abcdef0123456789",               // Scrypted device id
           orientation: "portrait",                    // 480x800
       },
   ];
   ```

   - **`name`**: must match what the script POSTs to the device's `/config` (also drives `viewport-<name>.local`).
   - **`host`**: viewport's current IP or hostname. v1 has no mDNS-SD discovery — set it manually and update if DHCP renumbers (or use a DHCP reservation).
   - **`cameraId`**: the Scrypted device id (not the human name) of the camera to bind. Grab it from the URL bar on the camera's settings page in Scrypted.
   - **`orientation`**: `portrait` (480×800) or `landscape` (800×480). Sent to the device via `/config` and used to size the snapshots.

5. Save. The script's `start()` runs immediately. Within a few seconds you should see lines in the Scrypted log:

   ```
   Scrypted Viewport script up. Callback URL base: http://scrypted.local:11080/endpoint/<scriptId>
   Bindings: 1 viewport(s)
   subscribed to "Front Door" events for viewport "mudroom"
   ```

6. On the viewport, `GET /config` should now show the populated `scrypted` URL and the `viewport` name. `GET /state` should show `configured: true` and `state: "asleep"`.

## Tuning constants

Defined right above the class:

| Constant | Default | What it controls |
| --- | --- | --- |
| `IDLE_TIMEOUT_MS` | 60000 | Sent to the device in `/config`. Both sides use this value independently. |
| `STREAM_TIMEOUT_MS` | == `IDLE_TIMEOUT_MS` | Scrypted-side per-stream cutoff. Keep it the same as the device's idle timeout so both ends agree. |
| `FRAME_INTERVAL_MS` | 1000 | Snapshot push cadence during an active stream. ~1 fps is fine for ambient camera viewing. |
| `REREGISTER_INTERVAL_MS` | 300000 | Re-issue `/config` to every viewport every 5 minutes so a viewport that rebooted or moved IPs re-syncs without manual intervention. |
| `HTTP_TIMEOUT_MS` | 1000 | Per-call timeout for outbound POSTs. Matches the device's own 1 s timeout. |

## What the script does on each event

- **Camera event (doorbell ring / motion / person)** → call `startStream(binding.name)`:
  - cancel any previous stream for that viewport,
  - POST `{state: "wake"}` to the device,
  - start the snapshot interval,
  - arm a `STREAM_TIMEOUT_MS` safety timer.
- **Device-initiated `{state: "wake"}` callback** (operator tapped the panel) → same `startStream` path.
- **Device-initiated `{state: "sleep"}` callback** → `stopStream` with `sendSleep=false` (the device already knows).
- **Per-stream safety timer fires** → `stopStream` with `sendSleep=true` (tell the device to sleep).
- **`POST /frame` returns 409** → device went to sleep on its own (tap-to-sleep, or its idle timer); `stopStream` with `sendSleep=false`.

## v1 limitations

- Snapshot-rate only (~1 fps). Live MJPEG over `POST /stream` is v2.
- Manual IP per viewport (no mDNS-SD discovery yet — that's v1.1).
- Camera must respect `picture.width` / `picture.height` in `PictureOptions` or be paired with a snapshot plugin that resizes. If the camera returns the wrong size, the device rejects `/frame` with 400 and you'll see warning logs. Workaround: configure the camera plugin's snapshot size, or wait for v2 which will resize Scrypted-side via FFmpeg.
- No retry on transport errors. Best-effort matches the device's own semantics; the next event or callback re-syncs.

## End-to-end smoke test

Once installed and bindings are filled in:

1. **Viewport boots** with no `/config` yet → IP screen on panel.
2. Script runs → `POST /config` lands → viewport → `state: asleep`, backlight off.
3. Tap viewport → device POSTs `{viewport: "mudroom", state: "wake"}` → script logs `recv mudroom -> wake` → snapshots start flowing for `STREAM_TIMEOUT_MS`.
4. Tap viewport again → device POSTs `{state: "sleep"}` → script stops streaming.
5. Trigger the bound camera (doorbell, motion sensor, or person detection) → script POSTs `{state: "wake"}` to viewport → snapshots flow for `STREAM_TIMEOUT_MS`, then both sides time out and sleep.
