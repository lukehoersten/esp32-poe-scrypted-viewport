# Plan: convert the Scripts-plugin script into a real Scrypted plugin

Status: **planned, not started.** The script (`scrypted-viewport.ts`, deployed
by pasting into Scrypted's Scripts editor) is fully working as of v1.4.0 —
this conversion is a structural investment, not a bug fix. Written so a
future session can pick it up cold.

## Why

The Scripts sandbox does not tear down a previous load's resources before
constructing a new instance. Most of the script's hardest-won machinery
exists *only* to survive that:

- the `globalThis.__viewportShutdownCleaners` registry + drain-at-bootstrap
  ordering (a re-paste must kill the prior load's ffmpeg children, sockets,
  intervals, and camera listeners)
- listener-map invalidation inside the cleaners (so a drain by another
  instance can't strand `attachedListenerSig`/`listeners` claiming live
  registrations)
- the `display_name`-is-canonical scheme (v.name drifts to the nativeId on
  script reload)
- the storage-attach race on reload (root cause of the historical
  doorbell-wake bug) and the register-cycle self-healing re-attach
- `SCRIPT_VERSION` stamping + manual paste as the whole deploy story

A real plugin runs in its own worker process that Scrypted kills and
restarts on deploy: the cross-reload leak class disappears, and deploys
become scriptable (`npx scrypted deploy`) — no more paste step, and agent
sessions can ship plugin changes end-to-end like they already do firmware.

Secondary wins: real `import sdk from '@scrypted/sdk'` with actual types
(today everything is `declare const` + `type Foo = any`), npm deps become
possible (not currently needed), per-device consoles.

## Shape

- New top-level `plugin/` package (scaffold: `npx scrypted create` or copy
  the @scrypted/plugin template; webpack build → `dist/plugin.zip`).
  Keep `scrypted/scrypted-viewport.ts` frozen and working until cutover is
  verified, then delete it and fold `scrypted/README.md` install docs into
  the plugin's.
- Not published to npm — deployed straight at the LAN server:
  `npx scrypted login <server>` once, then `npx scrypted deploy <server>`.
- `Makefile`: add `make plugin` (build + deploy). Retire the
  `SCRIPT_VERSION` stamp convention; `package.json` version + git replace it.

## Port map (what moves, what changes, what dies)

Moves ~1:1 (the classes already implement the right interfaces):
- `ScryptedViewportProvider` (DeviceProvider, DeviceCreator,
  HttpRequestHandler, Settings, StartStop) and `Viewport` (Settings)
- the whole stream pipeline (ffmpeg spawn via `child_process`, JPEG demux,
  raw TCP :81 framing, skip-oldest backpressure, streamLogger)
- mDNS discovery (`mdnsBrowse` + wire codec — keep the hand-rolled
  dependency-free version; it's proven against real firmware responses),
  host comboboxes, auto-heal, mac seeding
- wake/sleep + trigger-scoped `attachListener`/`handleCameraEvent`

Changes:
- `declare const` globals → `import sdk, { ScryptedDeviceBase,
  ScryptedInterface, ScryptedDeviceType, ... } from '@scrypted/sdk'`;
  `systemManager` etc. become `sdk.systemManager` etc.
- Replace the `type X = any` aliases with real SDK types incrementally —
  start loose, tighten after the port compiles.
- Drop the bootstrap `onDeviceDiscovered` hack that overrides the Scripts
  plugin's hardcoded `Unknown` device type — the plugin's `package.json`
  `scrypted` section declares type/interfaces properly.
- `HttpRequestHandler` endpoint URL changes (new plugin id in the path).
  No firmware change needed: the 5-minute `registerViewport` cycle POSTs
  the new callback base via `/config` automatically.

Dies (the point of the exercise):
- `pushShutdownCleaner`/`drainShutdownCleaners` and everything that exists
  to serve them. KEEP the register-cycle `attachListener` self-heal (cheap
  idempotent belt-and-braces) and `display_name`-as-canonical (harmless,
  and protects against any residual name races).
- `SCRIPT_VERSION`.

## Open questions to verify during the port

1. **Orphaned ffmpeg on plugin restart.** The worker process is killed on
   deploy; spawned ffmpeg children survive parent death but should die on
   the next stdout write (EPIPE) since their pipe consumer is gone. Verify
   with a deploy mid-stream (`ps` for ffmpeg after). If they linger, add a
   `process.on('exit')`-adjacent kill or track children and SIGTERM them
   in a StartStop.stop path.
2. **Storage race**: confirm `deviceManager.getDeviceStorage` is reliably
   attached at plugin startup for pre-existing children (expected: yes —
   this was a Scripts-sandbox artifact). If yes, the "no camera assigned /
   no host" boot warnings disappear.
3. Settings UI parity: `combobox`/`choices`, `deviceFilter`, multi-select
   triggers, one-shot action toggles — all standard Setting fields, expect
   no changes; eyeball each page.
4. `endpointManager.getInsecurePublicLocalEndpoint(nativeId)` behavior in
   a real plugin (should be identical).

## Cutover checklist (order matters)

1. Deploy the plugin; do NOT bind any viewports yet.
2. STOP the old script via its Scripts-UI Stop (drains every listener /
   stream / interval — verified working), then delete the script device.
   Never run both live: two instances = duplicate camera listeners =
   duplicate streams to one panel (known failure mode).
3. Recreate the kitchen viewport in the plugin: "+ Add Device", pick the
   discovered host (name auto-fills from mDNS TXT), pick camera, set
   triggers. (Only one device exists today; storage migration is not worth
   building.)
4. Confirm firmware `/config` shows the NEW callback URL after the first
   register (or force it with a settings save).
5. Run the verification suite below.

## Verification suite (same bar as the script)

- boot log clean, no storage-race warnings
- doorbell press → wake + stream (`event BinarySensor ... (wake)`)
- panel tap → `recv "kitchen" -> wake (device-initiated)` + stream, full
  brightness, 204 on the device side (`state_post_failures` stays 0)
- manual Wake/Sleep toggles from settings
- rename via "Viewport name" → firmware `/config` + mDNS hostname follow
- auto-heal: set host to a wrong IP → `mdns auto-heal` log line corrects it
- deploy mid-stream → old ffmpeg exits (no orphan), next event streams
- steady-state stream health: painted ≈ 24 fps, g2g ~60-100 ms, `/state`
  stream window sane (see TESTING.md for the sampling loop)

## Estimate

Roughly one focused session: scaffold + mechanical port + build tooling in
the first half; cutover + the verification suite in the second. Firmware
is untouched throughout.
