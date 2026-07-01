// Scrypted Viewport — v1 Scripts-plugin script
//
// SCRIPT_VERSION is bumped on every commit that touches this file.
// The boot log emits it so we can verify the user re-pasted the
// latest version when reading the plugin console. Format is the
// short git hash of the commit that added this constant — if the
// hash in the log doesn't match the HEAD this file came from, the
// Scrypted Script editor is still on stale code.
const SCRIPT_VERSION = "1bd5e9f";
//
// Architecture
// ------------
// One parent device (DeviceProvider + DeviceCreator + HttpRequestHandler)
// spawns N child "Viewport" devices via the Scrypted UI. Each child holds
// the per-viewport binding (host, camera, orientation, idle timeout,
// brightness) as plain device settings and is editable from its own
// Settings page in the Scrypted UI. The parent owns the camera event
// subscriptions, the snapshot push loop, the per-stream safety timer,
// and the inbound `POST <base>/state` handler.
//
// Install
// -------
// 1. In Scrypted: Plugins → install "Scripts" if needed.
// 2. + Add Device → Scripts plugin → New Script.
// 3. Paste this entire file. Save.
// 4. Open the new "Scrypted Viewport" device. Click "+ Add Device" on
//    its page to create a viewport binding:
//       Viewport name    e.g. "mudroom"  (friendly routing key)
//       IP or hostname   e.g. "192.168.1.42"
//       Camera           pick from the dropdown of Camera devices
//       Orientation      portrait (480×800) or landscape (800×480)
// 5. The script POSTs /config to the device immediately and re-issues it
//    every 5 minutes so a reboot or DHCP renumber re-syncs.
// 6. Edit a viewport's settings from its own device page in the UI. The
//    script re-registers and re-subscribes whenever a setting changes.
//
// Streaming
// ---------
// On wake we subscribe to the camera's video stream, spawn one ffmpeg
// child that scales + re-encodes to MJPEG (q:v 2, lanczos) at the
// viewport's configured fps, demux JPEG frames out of stdout (FFD8…FFD9)
// and POST each one to the firmware's existing /frame endpoint. Single-
// flight semantics gate against the firmware's in-flight mutex; surplus
// frames are dropped silently and counted for a periodic skip-rate log.
//
// Limits
// ------
// - Manual IP per viewport (see README for how to find it via mDNS from
//   your shell). DHCP reservation recommended so the IP stays stable.
// - Camera must expose a video stream; pure-snapshot cameras need a
//   transcoder mixin upstream.

// The Scripts plugin (in @scrypted/core) evaluates this file inside the
// scryptedEval sandbox. The runtime pre-injects the SDK names as scope
// locals — no `import` is needed (or allowed: any `import ... from
// "@scrypted/sdk"` compiles to a require() that fails to resolve).
// All we do here is `declare` each one so TypeScript is happy; the
// declarations erase at compile time and the values come from the
// runtime scope.
declare const sdk: any;
declare const ScryptedDeviceBase: any;
declare const ScryptedDeviceType: any;
declare const ScryptedInterface:  any;
declare const systemManager:      any;
declare const endpointManager:    any;
declare const mediaManager:       any;
declare const deviceManager:      any;
declare const log:                any;
declare const device:             any;
declare const require:            any;


// Loose type aliases — purely cosmetic for the rest of the script's
// signatures, since the runtime values are `any`.
type DeviceCreator           = any;
type DeviceCreatorSettings   = any;
type DeviceProvider          = any;
type EventListenerRegister   = any;
type HttpRequest             = any;
type HttpRequestHandler      = any;
type HttpResponse            = any;
type Setting                 = any;
type Settings                = any;
type SettingValue            = any;
type StartStop               = any;

// ---------------------------------------------------------------------------
// Cross-reload shutdown cleaners.
//
// Scrypted's Scripts sandbox doesn't release prior-load resources before
// constructing a new instance, so anything long-lived (setInterval, sockets,
// ffmpeg children, AbortControllers, camera event registrations) leaks across
// every re-paste of the script unless we explicitly tear it down. Every such
// resource pushes a cleanup closure here; the constructor drains the list at
// the very start of `start()` so the new load begins with empty state.
//
// One unified array keeps the cleanup semantics simple: register the closure
// that knows how to release the resource (clearInterval / abort.abort() /
// reg.removeListener()), and we walk and call them on the next reload.
// Drain and run every registered cleanup, leaving the array empty so the
// next phase can start populating it again. Called from two paths:
//   - script load: tears down resources left over from a previous Provider
//     instance the Scrypted sandbox didn't release
//   - StartStop.stop(): explicit user-driven cleanup via the Scripts UI
function drainShutdownCleaners(console: any, reason: string) {
    const G = globalThis as any;
    if (Array.isArray(G.__viewportShutdownCleaners) && G.__viewportShutdownCleaners.length > 0) {
        // Snapshot + reset BEFORE walking — some cleanups (stream
        // aborts) fire abort listeners that splice themselves out of
        // the array, which would skip elements if we iterated directly.
        const prior = G.__viewportShutdownCleaners.slice();
        G.__viewportShutdownCleaners = [];
        console.log(`${reason}: tearing down ${prior.length} resources`);
        for (const cleanup of prior) {
            try { cleanup(); } catch (e) {
                try { console.warn("shutdown cleanup failed:", (e as Error).message); } catch {}
            }
        }
    } else {
        G.__viewportShutdownCleaners = [];
    }
}
function pushShutdownCleaner(cleanup: () => void) {
    const G = globalThis as any;
    if (!Array.isArray(G.__viewportShutdownCleaners)) G.__viewportShutdownCleaners = [];
    G.__viewportShutdownCleaners.push(cleanup);
}

// Tuning constants.
// Stream rate is paced by camera + TCP backpressure; no app-level fps
// cap, no per-frame pipelining semaphore.
const REREGISTER_INTERVAL_MS     = 5 * 60_000;
// 5s is generous for snapshot POSTs (the only HTTP traffic during a
// stream) and survives long-tail latency on a busy Scrypted host.
const HTTP_TIMEOUT_MS            = 5_000;
const DEFAULT_IDLE_TIMEOUT_MS    = 60_000;
const DEFAULT_BRIGHTNESS         = 100;

// ============================================================================
// Child: one viewport binding
// ============================================================================

class Viewport extends ScryptedDeviceBase implements Settings {
    constructor(public provider: ScryptedViewportProvider, nativeId: string) {
        super(nativeId);
    }

    get host(): string         { return this.storage.getItem("host") || ""; }
    get cameraId(): string     { return this.storage.getItem("cameraId") || ""; }
    get orientation(): "portrait" | "landscape" {
        const v = this.storage.getItem("orientation");
        return v === "landscape" ? "landscape" : "portrait";
    }
    get idleTimeoutMs(): number {
        const v = this.storage.getItem("idle_timeout_ms");
        return v ? Math.max(0, parseInt(v, 10) || 0) : DEFAULT_IDLE_TIMEOUT_MS;
    }
    get brightness(): number {
        const v = this.storage.getItem("brightness");
        return v ? Math.max(0, Math.min(100, parseInt(v, 10) || 0)) : DEFAULT_BRIGHTNESS;
    }
    // ffmpeg mjpeg encoder -q:v. Valid range 1..31, lower = higher
    // quality + bigger JPEG (1 ≈ visually lossless, 31 ≈ very lossy).
    // Default 1 — with HTTP keep-alive + NODELAY we have plenty of
    // body-upload headroom for the bigger frames. Bump up only if
    // you're chasing fewer bytes on the wire at the cost of visible
    // artifacts.
    get jpegQuality(): number {
        const v = this.storage.getItem("jpeg_quality");
        const parsed = v ? parseInt(v, 10) : NaN;
        return Number.isFinite(parsed) ? Math.max(1, Math.min(31, parsed)) : 1;
    }
    // Emergency cap on Node's TCP send buffer for the stream socket,
    // in MB. Skip-oldest backpressure handling (see stream send loop)
    // keeps only one in-flight frame plus one pending — Node's queue
    // should stay at ~1 frame steady-state. This cap is the safety net
    // for a stuck connection that never fires 'drain': when exceeded,
    // the socket is destroyed and reconnected so a fresh frame can land.
    get maxNodeBufMb(): number {
        const v = this.storage.getItem("max_node_buf_mb");
        const parsed = v ? parseInt(v, 10) : NaN;
        return Number.isFinite(parsed) ? Math.max(1, Math.min(200, parsed)) : 20;
    }
    // Cold-start mitigation: request a prebuffer from the rebroadcast
    // plugin so getVideoStream hands ffmpeg a buffer that already begins on
    // a recent keyframe — eliminating the wait for the camera's NEXT
    // keyframe (the ~5-6s first-frame gap we measured). Because the whole
    // pipeline is skip-to-freshest (ffmpeg unpaced, Scrypted drop-oldest,
    // firmware FIONREAD-skip), the prebuffered burst collapses: the panel
    // catches up to live within a fraction of a second and steady-state
    // g2g is unchanged — it only kills the startup gap, no permanent lag.
    // Default 3000ms: large enough to reliably contain a keyframe for the
    // ~5s GOP measured here, small enough that the startup burst is modest.
    // To work, the prebuffer must be ≥ the source GOP; raise it if the
    // spawned→first-byte gap doesn't shrink. 0 = off (live edge).
    get streamPrebufferMs(): number {
        const v = this.storage.getItem("stream_prebuffer_ms");
        const parsed = v ? parseInt(v, 10) : NaN;
        // Default 6000ms — must exceed the camera's keyframe interval (measured
        // 5.044s on this Unifi cam) so the requested backfill is guaranteed to
        // contain at least one IDR; otherwise ffmpeg still waits for the next
        // live keyframe. The Scrypted-side prebuffer for the chosen stream must
        // ALSO be enabled and ≥ that interval (enable it under the camera's
        // Stream Management → per-stream tab). 0 = off (cold live-edge, ~6s).
        return Number.isFinite(parsed) ? Math.max(0, Math.min(12000, parsed)) : 6000;
    }
    // True when the bound camera can emit a doorbell ring — i.e. it
    // advertises BinarySensor (Unifi pushes BinarySensor onto the camera
    // device when isDoorbell) or self-reports as a Doorbell. Drives both
    // the default triggers and whether "doorbell" is even offered as a
    // wake option: a plain camera never rings, so don't show the choice.
    get cameraIsDoorbell(): boolean {
        const id = this.cameraId;
        if (!id) return false;
        try {
            const cam: any = systemManager.getDeviceById(id);
            const ifaces: string[] = cam?.interfaces || [];
            return ifaces.includes(ScryptedInterface.BinarySensor) ||
                   cam?.type === ScryptedDeviceType.Doorbell;
        } catch { return false; }
    }
    // The wake options applicable to the bound camera. "doorbell" only
    // appears for doorbell-capable cameras.
    get triggerChoices(): string[] {
        return this.cameraIsDoorbell ? ["doorbell", "person", "motion"] : ["person", "motion"];
    }
    // Which camera-event types wake this viewport. Empty = tap-only, never
    // woken by Scrypted. Default = person + doorbell (doorbell only when the
    // camera is doorbell-capable); motion is opt-in since doorbell cameras
    // are very chatty with motion and would wake the panel constantly.
    get triggers(): Set<string> {
        const v = this.storage.getItem("triggers");
        if (v === null) return new Set(this.cameraIsDoorbell ? ["doorbell", "person"] : ["person"]);
        try { return new Set(JSON.parse(v)); } catch { return new Set(); }
    }

    async getSettings(): Promise<Setting[]> {
        const settings: Setting[] = [
            {
                group: "Binding",
                key: "host",
                title: "IP or hostname",
                description: "Viewport's address on the LAN. Set this manually — find it via your DHCP table, or `dns-sd -G v4 viewport-<mac>.local` on macOS, or `avahi-resolve -n viewport-<mac>.local` on Linux. The info screen on the device itself shows its MAC + IP.",
                placeholder: "192.168.1.42",
                value: this.host,
            } as any,
            {
                group: "Binding",
                key: "cameraId",
                title: "Camera",
                description: "Camera whose events drive this viewport's wake/sleep, and whose snapshots get streamed.",
                type: "device",
                deviceFilter: `interfaces.includes('${ScryptedInterface.Camera}')`,
                value: this.cameraId,
            } as any,
            {
                group: "Binding",
                key: "triggers",
                title: "Wake triggers",
                description: "Which camera-event types automatically wake the viewport. Defaults to person + doorbell; motion is opt-in (doorbell cameras fire motion constantly). \"doorbell\" only appears for doorbell-capable cameras. Clear all for tap-only mode (never woken by Scrypted; user must tap).",
                choices: this.triggerChoices,
                multiple: true,
                value: Array.from(this.triggers),
            } as any,
            {
                group: "Display",
                key: "orientation",
                title: "Orientation",
                description: "Panel orientation. Frames are sent at this effective resolution.",
                choices: ["portrait", "landscape"],
                value: this.orientation,
            } as any,
            {
                group: "Display",
                key: "brightness",
                title: "Brightness (0–100)",
                description: "Sent to the device via /config. Gamma-corrected on the panel.",
                type: "number",
                value: this.brightness,
            } as any,
            {
                group: "Display",
                key: "idle_timeout_ms",
                title: "Idle timeout (ms)",
                description: "How long the device stays awake after the last paint before it sleeps itself. 0 disables; non-zero must be ≥ 5000.",
                type: "number",
                value: this.idleTimeoutMs,
            } as any,
            {
                group: "Display",
                key: "jpeg_quality",
                title: "JPEG quality (1–31, lower = better)",
                description: "ffmpeg mjpeg encoder -q:v. 1 ≈ visually lossless (~140KB at panel-native), 5 ≈ good (~70KB), 10+ noticeably lossy. Default 1.",
                type: "number",
                value: this.jpegQuality,
            } as any,
            {
                group: "Display",
                key: "max_node_buf_mb",
                title: "Max Scrypted-side buffer (MB)",
                description: "Emergency cap on Node's TCP send queue for the stream socket. Skip-oldest backpressure handling normally keeps the queue at ~1 in-flight frame, so this should rarely trigger; if it does (stuck connection that never fires 'drain') the socket is destroyed and reconnected. Default 20.",
                type: "number",
                value: this.maxNodeBufMb,
            } as any,
            {
                group: "Display",
                key: "stream_prebuffer_ms",
                title: "Stream prebuffer (ms)",
                description: "Cold-start mitigation. Requests this much prebuffer from the rebroadcast plugin so the live stream opens on an already-buffered keyframe instead of waiting for the camera's next one — cuts the ~5–6s first-frame gap. Because the pipeline is skip-to-freshest, the buffered burst collapses to live within a fraction of a second, so this does NOT add steady-state latency — it only removes the startup gap. Must be ≥ the camera's keyframe interval (GOP) to help. Default 3000; raise toward the GOP if the spawned→first-byte gap doesn't shrink. 0 = off (live edge).",
                type: "number",
                value: this.streamPrebufferMs,
            } as any,
            {
                group: "Actions",
                key: "action_wake",
                title: "Wake now",
                description: "Toggle on to POST /state {wake} and start streaming the bound camera. Resets automatically after firing.",
                type: "boolean",
                value: false,
            } as any,
            {
                group: "Actions",
                key: "action_sleep",
                title: "Sleep now",
                description: "Toggle on to POST /state {sleep} and stop the active stream. Resets automatically after firing.",
                type: "boolean",
                value: false,
            } as any,
        ];

        // Live device snapshot: GET /state then /config sequentially
        // (parallel ate both httpd slots simultaneously after Phase 2
        // dropped max_open_sockets to 2, and could collide with an
        // in-flight stream-socket cap-flush reconnect). 3s timeout is
        // generous but not so long that an offline device feels
        // unresponsive in the UI.
        //
        // One automatic retry on failure: "fetch failed" at the socket
        // level usually means the firmware's httpd worker pool was
        // briefly saturated (the stream connection plus an /state poll
        // can starve a third request on max_open_sockets=2). A 250 ms
        // pause then try again before giving up — eliminates the
        // sporadic "offline / unreachable" the Settings page would
        // otherwise show during normal streaming.
        const fetchJsonRetry = async (url: string): Promise<any> => {
            let lastErr: any;
            for (let attempt = 0; attempt < 2; attempt++) {
                try {
                    const r = await fetch(url, { signal: AbortSignal.timeout(3000) });
                    return await r.json();
                } catch (e) {
                    lastErr = e;
                    if (attempt === 0) await new Promise(res => setTimeout(res, 250));
                }
            }
            throw lastErr;
        };
        if (this.host) {
            try {
                const stateRes  = await fetchJsonRetry(`http://${this.host}/state`);
                const configRes = await fetchJsonRetry(`http://${this.host}/config`);
                settings.push(
                    { group: "Status (live)", key: "_st_name",   title: "name",                value: stateRes.name,                                                 readonly: true } as any,
                    { group: "Status (live)", key: "_st_mac",    title: "mac",                 value: stateRes.mac,                                                  readonly: true } as any,
                    { group: "Status (live)", key: "_st_ip",     title: "ip",                  value: stateRes.ip,                                                   readonly: true } as any,
                    { group: "Status (live)", key: "_st_state",  title: "state",               value: stateRes.state,                                                readonly: true } as any,
                    { group: "Status (live)", key: "_st_cfg",    title: "configured",          value: String(stateRes.configured),                                   readonly: true } as any,
                    { group: "Status (live)", key: "_st_uptime", title: "uptime (ms)",         value: String(stateRes.uptime_ms),                                    readonly: true } as any,
                    { group: "Status (live)", key: "_st_last",   title: "last frame (ms ago)", value: String(stateRes.last_frame_ms_ago ?? "(none)"),                readonly: true } as any,
                    { group: "Status (live)", key: "_st_fr",     title: "frames received",     value: String(stateRes.frames_received),                              readonly: true } as any,
                    { group: "Status (live)", key: "_st_err",    title: "decode errors",       value: String(stateRes.decode_errors),                                readonly: true } as any,
                    { group: "Status (live)", key: "_st_post",   title: "state post failures", value: String(stateRes.state_post_failures),                          readonly: true } as any,
                    { group: "Status (live)", key: "_st_res",    title: "resolution",          value: stateRes.resolution,                                           readonly: true } as any,
                    { group: "Status (live)", key: "_st_heap",   title: "free heap (bytes)",   value: String(stateRes.free_heap),                                    readonly: true } as any,
                    { group: "Status (live)", key: "_st_psram",  title: "free PSRAM (bytes)",  value: String(stateRes.free_psram),                                   readonly: true } as any,
                    { group: "Status (live)", key: "_st_ver",    title: "firmware",            value: stateRes.version,                                              readonly: true } as any,
                    { group: "Status (live)", key: "_cfg_scrypt",title: "config: scrypted URL",value: configRes.scrypted ?? "(not set)",                             readonly: true } as any,
                );
            } catch (e) {
                settings.push({ group: "Status (live)", key: "_st_err", title: "device", value: `offline / unreachable (${(e as Error).message})`, readonly: true } as any);
            }
        }

        return settings;
    }

    async putSetting(key: string, value: SettingValue) {
        if (key.startsWith("_")) return;                 // ignore read-only status fields
        if (key === "action_wake" || key === "action_sleep") {
            // Manual override from the Scrypted UI. Wake also starts a
            // stream so the user sees the camera immediately; Sleep
            // tears down the live ffmpeg and POSTs sleep.
            // Boolean acts as a one-shot trigger — fire on truthy then
            // re-render with the toggle cleared so it's ready to fire
            // again next time.
            const truthy = value === true || value === "true";
            if (!truthy) return;
            if (!this.host) return;
            if (key === "action_wake") {
                if (!this.provider.streams.has(this.name) &&
                    !this.provider.streamStarting.has(this.nativeId!)) {
                    this.provider.streamStarting.add(this.nativeId!);
                    this.provider.startStream(this)
                        .catch(e => this.console.error("manual wake failed", e))
                        .finally(() => this.provider.streamStarting.delete(this.nativeId!));
                }
            } else {
                this.provider.stopStream(this.name, /*sendSleep=*/ true);
            }
            return;
        }
        if (key === "triggers") {
            // multi-select arrives as array; serialise to JSON for storage.
            // Strip "doorbell" if the bound camera can't ring (mirror of
            // createDevice + triggerChoices) so a stale/forced selection
            // can't smuggle doorbell onto a plain camera.
            let arr = Array.isArray(value) ? (value as string[]) : [];
            if (!this.cameraIsDoorbell) arr = arr.filter(t => t !== "doorbell");
            this.storage.setItem("triggers", JSON.stringify(arr));
        } else {
            this.storage.setItem(key, String(value ?? ""));
            if (key === "cameraId") {
                // The camera binding drives which wake triggers are valid
                // (doorbell only for doorbell cameras). Reconcile the stored
                // selection to the new camera's choices so we never persist
                // a trigger the camera can't emit.
                const valid = new Set(this.triggerChoices);
                const reconciled = Array.from(this.triggers).filter(t => valid.has(t));
                this.storage.setItem("triggers", JSON.stringify(reconciled));
            }
        }
        await this.provider.onBindingChanged(this);
        // After a camera change, tell the Scrypted console the Settings
        // interface changed so it re-fetches getSettings() and re-renders
        // the Wake-triggers choices live (showing/hiding doorbell) instead
        // of waiting for a manual page reload.
        if (key === "cameraId") {
            try { await (this as any).onDeviceEvent?.(ScryptedInterface.Settings, undefined); } catch {}
        }
    }
}

// ============================================================================
// Parent: provider + HTTP handler + global tuning
// ============================================================================

class ScryptedViewportProvider extends ScryptedDeviceBase
    implements DeviceProvider, DeviceCreator, HttpRequestHandler, Settings, StartStop {

    private viewports = new Map<string, Viewport>();             // nativeId -> child instance
    private listeners = new Map<string, EventListenerRegister[]>(); // nativeId -> all event listeners for this viewport (camera + child devices)
    // nativeId -> cameraId we currently have a listener attached for.
    // Source of truth for attachListener idempotency: lets it self-heal
    // (re-attach after the reload storage-race) without stacking duplicate
    // listeners on every register cycle. "" = processed-but-no-camera
    // (suppresses repeated "no camera assigned" warnings); absent = never
    // processed.
    private attachedCameraId = new Map<string, string>();
    streams = new Map<string, {                                   // viewport name -> stream control (accessed by Viewport.putSetting for manual wake/sleep)
        timeout:   NodeJS.Timeout;
        abort:     AbortController;       // also tears down the ffmpeg child via its listener
        interval?: NodeJS.Timeout;        // legacy snapshot-poll mode
    }>();
    private scryptedBase = "";

    constructor(nativeId?: string) {
        super(nativeId);
        // Initialise running=false synchronously so the device record
        // has a defined value at registration time.
        this.running = false;
        // Auto-start on script load. start() is idempotent — if the user
        // later clicks STOP in the Scripts UI, stop() drains everything
        // and start() doesn't re-fire until they click START. Across a
        // script re-paste the constructor runs again, start() drains any
        // prior load's resources first and re-bootstraps cleanly.
        this.start().catch(e => this.console.error("start failed", e));
    }

    // ------------------------------------------------------------------------
    // StartStop — public lifecycle controls exposed to the Scripts UI
    // ------------------------------------------------------------------------
    //
    // Scrypted's Scripts plugin (plugins/core/src/scrypted-eval.ts mergeHandler)
    // auto-detects this interface from the start()/stop() method names; the
    // 'Status and Controls' panel's STOP/START buttons call these directly.
    // Confirmed empirically: clicking STOP fires our stop() — verified in
    // the v101fb3e session that explored OnOff alongside StartStop and
    // observed only stop() being called.
    //
    // Semantics:
    //   start(): drain any leftover resources (previous load or current
    //            in-flight), then bootstrap. Idempotent.
    //   stop():  drain every resource (streams, listeners, intervals)
    //            and clear in-memory maps. Idempotent.

    async start() {
        this.console.log(`StartStop.start() invoked (running=${this.running})`);
        if (this.running) return;
        await this.bootstrap();
        this.running = true;
    }

    async stop() {
        // ALWAYS drain — never gate on this.running. Live resources
        // (ffmpeg children, sockets, intervals, listeners) live in the
        // global __viewportShutdownCleaners array, which is the real
        // source of truth. The Scrypted Scripts sandbox leaks instances
        // across re-pastes, so the instance that receives this Stop click
        // can have this.running===false (a newer load owns the resources,
        // or bootstrap() is mid-await) while subprocesses are still alive.
        // The old `if (!this.running) return` short-circuited those teardowns
        // and orphaned ffmpeg/sockets — the "Stop does nothing" symptom.
        this.console.log(`StartStop.stop() invoked (running=${this.running})`);
        // 1. Drain the global cleaner array: aborts every stream (→ ffmpeg
        //    SIGTERM, socket destroy, streamLogger + idle-timeout clear),
        //    removes every camera + system event listener, clears the
        //    reregister interval and the diagnostic listener.
        drainShutdownCleaners(this.console, "stop");
        // 2. Cancel any pending per-viewport debounce timers — these aren't
        //    in the cleaner array and would otherwise fire ~300ms later and
        //    re-attach/re-register against a stopped provider.
        for (const t of this.bindingDebounce.values()) { try { clearTimeout(t); } catch {} }
        this.bindingDebounce.clear();
        // 3. Drop all in-memory state so a later start() rebuilds from
        //    scratch — identical to a fresh load. (Child DEVICE records and
        //    their storage persist; only our live instances/listeners go.)
        this.viewports.clear();
        this.listeners.clear();
        this.streams.clear();
        this.attachedCameraId.clear();
        this.running = false;
    }

    // ------------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------------

    private get childIds(): string[] {
        try { return JSON.parse(this.storage.getItem("childIds") || "[]"); }
        catch { return []; }
    }
    private set childIds(ids: string[]) {
        this.storage.setItem("childIds", JSON.stringify(ids));
    }


    private async bootstrap() {
        // endpointManager.getInsecurePublicLocalEndpoint() takes a nativeId
        // (string) — passing this.id (numeric Scrypted DB ID) throws
        // "invalid nativeId N". this.nativeId is the right key, and an
        // omitted nativeId falls back to the plugin's own endpoint.
        const raw = await endpointManager.getInsecurePublicLocalEndpoint(this.nativeId);
        this.scryptedBase = raw.replace(/\/$/, "");
        this.console.log(`Scrypted Viewport up (script=${SCRIPT_VERSION}). Callback URL base: ${this.scryptedBase}`);

        // Override the device type that the @scrypted/core Scripts plugin
        // hardcodes (`ScryptedDeviceType.Unknown` at plugins/core/src/
        // script.ts:65) so the UI displays a meaningful label above the
        // Status and Controls panel instead of "Unknown". This Provider
        // semantically bridges multiple child viewport devices, so Bridge
        // fits. The call happens after Scripts plugin's postRunScript-
        // driven discovery, so this update wins.
        //
        // We pass the full interface set explicitly: passing a partial
        // list would drop interfaces the auto-detection found. Order
        // mirrors the class's implements clause.
        try {
            await deviceManager.onDeviceDiscovered({
                providerNativeId: "scriptcore",
                nativeId: this.nativeId,
                name: this.name || this.providedName,
                type: ScryptedDeviceType.Bridge,
                interfaces: [
                    ScryptedInterface.Scriptable,
                    ScryptedInterface.Program,
                    ScryptedInterface.Settings,
                    ScryptedInterface.DeviceProvider,
                    ScryptedInterface.DeviceCreator,
                    ScryptedInterface.HttpRequestHandler,
                    ScryptedInterface.StartStop,
                ],
            });
        } catch (e) {
            this.console.warn(`type override failed: ${(e as Error).message}`);
        }

        // Re-discover every known child so Scrypted reattaches its storage
        // to the nativeId. Without this, `new Viewport(...)` instantiates
        // with `this.storage === undefined` and every storage-backed getter
        // (host / cameraId / orientation / ...) throws on script reload.
        // Then eagerly instantiate so each child's registration + camera
        // event subscription happen at plugin load.
        const staleChildIds: string[] = [];
        for (const nativeId of this.childIds) {
            try {
                // A childId whose storage container is gone is a stale entry
                // — the device was deleted in the UI (or never persisted) but
                // its id lingered in our childIds list. Re-discovering it would
                // resurrect a ghost the user deleted, so prune it instead.
                const store = deviceManager.getDeviceStorage(nativeId);
                if (!store) {
                    this.console.warn(`pruning stale child ${nativeId} — no storage (deleted or never persisted)`);
                    staleChildIds.push(nativeId);
                    continue;
                }
                // Use the persisted display_name as the canonical device
                // name so a script reload doesn't reset it to the nativeId.
                // First-time provision falls back to the nativeId.
                const displayName = store.getItem("display_name") || nativeId;
                await deviceManager.onDeviceDiscovered({
                    providerNativeId: this.nativeId,
                    nativeId,
                    name: displayName,
                    type: ScryptedDeviceType.SmartDisplay,
                    interfaces: [ScryptedInterface.Settings],
                });
                await this.getDevice(nativeId);
            }
            // Other (transient) load errors are logged but NOT pruned — only a
            // missing storage container is a definitive "device gone" signal.
            catch (e) { this.console.warn(`load child ${nativeId} failed:`, (e as Error).message); }
        }
        if (staleChildIds.length) {
            this.childIds = this.childIds.filter(id => !staleChildIds.includes(id));
            this.console.log(`pruned ${staleChildIds.length} stale child id(s): ${staleChildIds.join(", ")}`);
        }

        // Unified script-reload cleanup.
        //
        // Scrypted's Scripts sandbox does NOT release a previous load's
        // resources before constructing a new instance. setInterval
        // handles, TCP sockets, ffmpeg children, AbortControllers, and
        // camera event registrations all survive a script re-paste and
        // accumulate over time. Observable symptoms:
        //   - duplicate "registered ..." log lines every 5 minutes
        //     after N reloads
        //   - duplicate stream-start log lines (and the racing
        //     pushSnapshots that degrade panel image quality) because
        //     the old instance's event listener is still subscribed
        //   - orphaned ffmpeg processes + half-open TCP sockets to the
        //     firmware after a reload during an active stream
        //
        // We solve all of these uniformly: every long-lived resource
        // pushes a cleanup closure onto a single globalThis-anchored
        // array. The next constructor drains that array first, which
        // walks every resource the previous load created and tears it
        // down via the closure that knows how (clearInterval / abort /
        // removeListener). The new load then starts with an empty
        // resource set.
        drainShutdownCleaners(this.console, "start");
        const reregisterHandle = setInterval(() => {
            for (const v of this.viewports.values()) {
                this.registerViewport(v).catch(() => {});
            }
        }, REREGISTER_INTERVAL_MS);
        pushShutdownCleaner(() => clearInterval(reregisterHandle));

        // DIAGNOSTIC (doorbell/motion plumbing). The per-camera listener
        // logs nothing on a real doorbell ring, yet HomeKit receives the
        // event — so the event fires on a device and/or interface we
        // aren't subscribed to. systemManager.listen with no filter sees
        // EVERY event from EVERY device. We log only the sensor interfaces
        // we care about, with the SOURCE device id + name, so a single
        // ring reveals exactly which device id emits BinarySensor — then
        // we compare that to each viewport's selected cameraId. Remove
        // once the source device is confirmed. Cleaned up on re-paste.
        try {
            // Literal strings (not ScryptedInterface.*) so this diagnostic
            // still works even if the injected ScryptedInterface global is
            // itself the bug — that global is the suspect, so don't trust it.
            const SENSOR_IFACES = ["BinarySensor", "MotionSensor", "ObjectDetector"];
            const diagReg = (systemManager as any).listen((source: any, details: any, data: any) => {
                const iface = details?.eventInterface;
                if (SENSOR_IFACES.includes(iface)) {
                    this.console.log(
                        `evtscan: iface=${iface} srcId=${source?.id} srcName="${source?.name}" ` +
                        `prop=${details?.property} data=${typeof data === "object" ? JSON.stringify(data) : String(data)}`);
                }
            });
            pushShutdownCleaner(() => { try { diagReg.removeListener(); } catch {} });
            // Confirm the injected ScryptedInterface global resolves the
            // names our real listener filters on. If any print as
            // "undefined", that global is broken and the real listen()
            // filter matches nothing — explaining the silence directly.
            this.console.log(
                `evtscan: armed. ScryptedInterface resolves: ` +
                `BinarySensor=${ScryptedInterface?.BinarySensor} ` +
                `MotionSensor=${ScryptedInterface?.MotionSensor} ` +
                `ObjectDetector=${ScryptedInterface?.ObjectDetector}`);
        } catch (e) {
            this.console.warn(`evtscan arm failed: ${(e as Error).message}`);
        }
    }

    // ------------------------------------------------------------------------
    // DeviceProvider
    // ------------------------------------------------------------------------

    async getDevice(nativeId: string): Promise<Viewport> {
        let v = this.viewports.get(nativeId);
        if (!v) {
            v = new Viewport(this, nativeId);
            this.viewports.set(nativeId, v);
            this.attachListener(v);
            await this.registerViewport(v);
        }
        return v;
    }

    async releaseDevice(id: string, nativeId: string) {
        const v = this.viewports.get(nativeId);
        if (v) {
            this.stopStream(v.name, /*sendSleep=*/ false);
            this.detachListener(nativeId);
            this.viewports.delete(nativeId);
        }
        this.childIds = this.childIds.filter(x => x !== nativeId);
    }

    // ------------------------------------------------------------------------
    // DeviceCreator — "+ Add Device" form on the parent
    // ------------------------------------------------------------------------

    async getCreateDeviceSettings(): Promise<Setting[]> {
        return [
            {
                key: "name",
                title: "Viewport name",
                description: 'Friendly routing key sent back in callbacks. Lowercase, no spaces. Example: "mudroom".',
                placeholder: "mudroom",
            },
            {
                key: "host",
                title: "IP or hostname",
                description: "Where the firmware lives on the LAN — either an IP or `viewport-<mac>.local` (the device prints its own MAC on the info screen). The script POSTs to this string directly; no auto-resolution.",
                placeholder: "192.168.1.42",
            },
            {
                key: "cameraId",
                title: "Camera",
                type: "device",
                deviceFilter: `interfaces.includes('${ScryptedInterface.Camera}')`,
            },
            {
                key: "triggers",
                title: "Wake triggers",
                description: "Which camera-event types automatically wake the viewport. Defaults to person + doorbell; motion is opt-in (doorbell cameras fire motion constantly). If the camera isn't a doorbell, the doorbell trigger is dropped automatically on save. Clear all for tap-only mode.",
                // Choices can't depend on the camera picked in this same
                // (static) form, so all three are offered here; createDevice
                // strips "doorbell" when the chosen camera can't ring, and
                // the per-device settings page hides it thereafter.
                choices: ["doorbell", "motion", "person"],
                multiple: true,
                value: ["doorbell", "person"],
            } as any,
            {
                key: "orientation",
                title: "Orientation",
                choices: ["portrait", "landscape"],
                value: "portrait",
            },
        ];
    }

    async createDevice(settings: DeviceCreatorSettings): Promise<string> {
        const name = String(settings.name || "viewport").trim();
        const nativeId = `vp_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 6)}`;

        // 1. Register the device with Scrypted FIRST. deviceManager
        //    materialises the storage container only after discovery —
        //    calling getDeviceStorage before this returns undefined and
        //    setItem() throws "Cannot read properties of undefined".
        await deviceManager.onDeviceDiscovered({
            providerNativeId: this.nativeId,
            nativeId,
            name,
            type: ScryptedDeviceType.SmartDisplay,
            interfaces: [ScryptedInterface.Settings],
        });

        // 2. Now safe to seed the child's storage from the form values.
        //    display_name is the canonical user-facing name; v.name (the
        //    ScryptedDeviceBase one) is async-loaded from Scrypted's record
        //    and races with our first registerViewport call, so we mirror
        //    it into storage as a stable fallback for register/log paths.
        const childStore = deviceManager.getDeviceStorage(nativeId);
        childStore.setItem("display_name", name);
        childStore.setItem("host",         String(settings.host || ""));
        childStore.setItem("cameraId",     String(settings.cameraId || ""));
        childStore.setItem("orientation",  String(settings.orientation || "portrait"));
        // settings.triggers arrives as an array from the multi-select.
        // JSON-encode to match how Viewport.putSetting stores it on
        // subsequent edits. Strip "doorbell" if the chosen camera can't
        // ring — the static add-form can't filter choices by camera, so we
        // enforce it here (mirror of Viewport.triggerChoices).
        let camIsDoorbell = false;
        if (settings.cameraId) {
            try {
                const cam: any = systemManager.getDeviceById(String(settings.cameraId));
                const ifaces: string[] = cam?.interfaces || [];
                camIsDoorbell = ifaces.includes(ScryptedInterface.BinarySensor) ||
                                cam?.type === ScryptedDeviceType.Doorbell;
            } catch { /* leave false */ }
        }
        let trigs = Array.isArray(settings.triggers)
            ? settings.triggers
            : (camIsDoorbell ? ["doorbell", "person"] : ["person"]);
        if (!camIsDoorbell) trigs = trigs.filter((t: string) => t !== "doorbell");
        childStore.setItem("triggers",     JSON.stringify(trigs));

        this.childIds = [...this.childIds, nativeId];
        this.console.log(`created viewport "${name}" (${nativeId})`);

        // Kick off the first register cycle (POST /config to the device).
        // Fire-and-forget — the new device shows up immediately either way.
        const child = await this.getDevice(nativeId);
        if (child) this.registerViewport(child).catch(() => {});

        return nativeId;
    }

    // ------------------------------------------------------------------------
    // Per-binding plumbing (camera subscription + /config registration)
    // ------------------------------------------------------------------------

    // Per-viewport debounce timer. Scrypted's Settings UI does one
    // putSetting per field on save, so a typical "Save" with 5 fields
    // changed used to register 5 times. Coalesce into a single apply.
    private bindingDebounce = new Map<string, NodeJS.Timeout>();

    onBindingChanged = async (v: Viewport): Promise<void> => {
        const nid = v.nativeId!;
        const pending = this.bindingDebounce.get(nid);
        if (pending) clearTimeout(pending);
        this.bindingDebounce.set(nid, setTimeout(() => {
            this.bindingDebounce.delete(nid);
            const tag = v.name || v.storage.getItem("display_name") || nid;
            this.console.log(`onBindingChanged "${tag}": re-attach (host=${v.host || "?"} cameraId=${v.cameraId || "?"})`);
            this.detachListener(nid);
            // Any active stream for this viewport is now stale (camera
            // or orientation may have changed). Stop cleanly; if it
            // was live we relaunch immediately under the new settings
            // so the user sees the change without waiting for the next
            // camera event.
            const wasStreaming = this.streams.has(v.name);
            this.stopStream(v.name, /*sendSleep=*/ false);
            this.attachListener(v);
            this.registerViewport(v)
                .then(() => {
                    if (wasStreaming) {
                        if (this.streamStarting.has(nid)) return;
                        this.streamStarting.add(nid);
                        this.startStream(v)
                            .catch(e => this.console.error("restart after setting change failed", e))
                            .finally(() => this.streamStarting.delete(nid));
                    }
                })
                .catch(() => {});
        }, 300));
    };

    private attachListener(v: Viewport) {
        const tag = v.name || v.storage.getItem("display_name") || v.nativeId;
        const nid  = v.nativeId!;
        const want = v.cameraId || "";
        const have = this.attachedCameraId.get(nid);

        // Idempotent fast-path: already listening on the right camera and
        // the listeners are still installed → nothing to do. This is what
        // lets registerViewport call us on every (5-min) cycle to self-heal
        // the reload storage-race without restacking listeners or spamming
        // logs. (The empty-camera case dedups on `have === want` below.)
        if (want === have && (want === "" || (this.listeners.get(nid)?.length ?? 0) > 0)) return;

        // State changed (first attach, camera swapped, or listeners lost):
        // tear down whatever was there before re-deciding.
        this.detachListener(nid);

        if (!want) {
            // Record the empty state so subsequent register cycles don't
            // re-warn every 5 minutes. Cleared by detachListener on rebind.
            this.attachedCameraId.set(nid, "");
            this.console.warn(`viewport "${tag}": no camera assigned — open Settings and pick a camera; subscription skipped`);
            return;
        }
        const cam = systemManager.getDeviceById(v.cameraId);
        if (!cam) {
            // Leave attachedCameraId unset so the next register cycle retries
            // (the camera device may simply not be loaded yet).
            this.console.warn(`viewport "${tag}": camera ${v.cameraId} not found`);
            return;
        }
        // DIAGNOSTIC: dump the selected camera's id + advertised interface
        // list. If BinarySensor/MotionSensor/ObjectDetector are NOT present
        // here, the doorbell/motion events live on a different Scrypted
        // device than the one picked — and our listen() can never fire.
        // Cross-reference srcId from the evtscan log on a real ring.
        this.console.log(
            `attach "${tag}": cameraId=${v.cameraId} cam.id=${(cam as any).id} ` +
            `cam.name="${(cam as any).name}" type=${(cam as any).type} ` +
            `interfaces=[${((cam as any).interfaces || []).join(", ")}]`);
        // Scrypted's ScryptedDevice.listen(event, cb) takes a SINGLE
        // interface (or EventListenerOptions {event}), never an array.
        // Confirmed in sdk/types/src/types.input.ts:21. Passing an array
        // stringifies to "BinarySensor,MotionSensor,..." which matches
        // nothing — events leak through with broken filtering. One
        // listen() per interface is the right shape.
        //
        // For Unifi doorbells the bell-press lives on the camera device
        // itself (unifi-protect/src/main.ts pushes BinarySensor onto the
        // camera's interfaces when isDoorbell). So listening on `cam`
        // for BinarySensor is all that's needed — no child traversal.
        const ifaces = [
            ScryptedInterface.BinarySensor,    // doorbell ring
            ScryptedInterface.MotionSensor,    // motion
            ScryptedInterface.ObjectDetector,  // person / etc
        ];

        const regs: EventListenerRegister[] = [];
        for (const iface of ifaces) {
            const reg = (cam as any).listen(iface, (source: any, details: any, data: any) => {
                this.handleCameraEvent(v, details, data);
            });
            regs.push(reg);
            // Register for cross-reload cleanup so a re-paste removes
            // every listener. Without this each re-paste stacks an extra
            // callback per event source.
            pushShutdownCleaner(() => { try { reg.removeListener(); } catch {} });
        }
        const targetNames = [`${cam.name || cam.id} (${ifaces.join("+")})`];
        this.listeners.set(v.nativeId!, regs);
        this.attachedCameraId.set(nid, want);
        this.console.log(`viewport "${tag}": subscribed to [${targetNames.join(", ")}]`);
    }

    private detachListener(nativeId: string) {
        const regs = this.listeners.get(nativeId);
        if (regs) {
            for (const r of regs) { try { r.removeListener(); } catch {} }
            this.listeners.delete(nativeId);
        }
        // Clear the idempotency tracker so the next attachListener re-decides
        // from scratch (re-attach after a rebind / camera swap).
        this.attachedCameraId.delete(nativeId);
    }

    private async registerViewport(v: Viewport) {
        // display_name is the canonical user-facing name (written on
        // createDevice and on every Settings save). v.name is just a
        // render of it from the Scrypted device record and can briefly
        // drift to the nativeId on script reload, so prefer the storage
        // value as the source of truth.
        const stored = v.storage.getItem("display_name");
        const name = (stored && stored.trim()) || (v.name && v.name.trim()) || "";
        if (!name) {
            this.console.warn(`register skipped — empty name on ${v.nativeId}; will retry on next event`);
            return;
        }
        if (!v.host) {
            this.console.warn(`register "${name}" skipped — no host. Set the viewport's "IP or hostname" field; see the README for how to find it via mDNS from your shell.`);
            return;
        }
        try {
            await this.postJSON(`http://${v.host}/config`, {
                viewport: name,
                scrypted: this.scryptedBase,
                idle_timeout_ms: v.idleTimeoutMs,
                orientation: v.orientation,
                brightness: v.brightness,
            });
            // Cache the name in storage so a future empty-.name event can
            // still find it. createDevice + putSetting always update this.
            v.storage.setItem("display_name", name);
            this.console.log(`registered "${name}" (${v.host})`);
            // Self-heal the camera subscription. A successful /config POST
            // proves storage is attached (host is readable) — the exact
            // moment the bootstrap attachListener may have run too early
            // (storage race) and skipped with an empty cameraId, never to
            // retry. attachListener is idempotent, so this is a no-op once
            // the right listener is in place; on the racy reload it's what
            // finally wires the doorbell/motion subscription without the
            // user having to re-save the setting.
            this.attachListener(v);
        } catch (e) {
            this.console.warn(`register "${name}" failed:`, (e as Error).message);
        }
    }

    // ------------------------------------------------------------------------
    // Camera event → stream
    // ------------------------------------------------------------------------

    // Per-viewport "stream is actively starting" guard. handleCameraEvent
    // can fire multiple times in the same second (MotionSensor often
    // re-asserts every ~500 ms while motion is sustained); without this,
    // each event launches its own startStream which races with the
    // previous one and saturates the firmware's httpd. If a stream is
    // already live we just leave it running — the per-stream timeout
    // anchored to the event still fires correctly.
    streamStarting = new Set<string>();

    private handleCameraEvent(v: Viewport, details: any, data: any) {
        // Ignore ALL events while a stream is already live OR starting for
        // this viewport. The wake window is anchored to the FIRST event;
        // subsequent events (motion re-asserts every ~500ms, person detect
        // fires repeatedly, the doorbell ring lands mid-motion) must not
        // queue, relaunch, or extend it. Cheapest possible early-out — before
        // trigger evaluation and before any logging — so a live stream sees
        // zero per-event work or log noise. (The system-wide evtscan listener
        // still records everything for diagnostics, independent of this.)
        if (this.streams.has(v.name) || this.streamStarting.has(v.nativeId!)) return;

        const iface = details.eventInterface;
        // TRACE: every event the camera emits on the interfaces we
        // listen to. Use to diagnose doorbell/motion/person plumbing.
        // Remove once root cause is confirmed.
        this.console.log(
            `trace "${v.name}": iface=${iface} typeof=${typeof data} ` +
            `data=${typeof data === "object" ? JSON.stringify(data) : String(data)} ` +
            `details=${JSON.stringify(details)}`);
        const allowed = v.triggers;
        let trigger = false;
        if (allowed.has("doorbell") && iface === ScryptedInterface.BinarySensor && data === true) trigger = true;
        if (allowed.has("motion")   && iface === ScryptedInterface.MotionSensor && data === true) trigger = true;
        if (allowed.has("person")   && iface === ScryptedInterface.ObjectDetector) {
            const detections = data?.detections ?? [];
            if (detections.some((d: any) => d?.className === "person")) trigger = true;
        }
        if (!trigger) return;
        // Capture the wall-clock at event arrival so every downstream
        // log line can rebase onto it (+Xms since event). This is the
        // anchor for measuring glass-to-glass and confirming that
        // snapshot + stream start truly in parallel.
        const tEvent = Date.now();
        this.console.log(`event ${iface} -> "${v.name}": fired at +0ms (wake)`);
        this.streamStarting.add(v.nativeId!);
        this.startStream(v, tEvent)
            .catch(e => this.console.error("startStream failed", e))
            .finally(() => this.streamStarting.delete(v.nativeId!));
    }

    async startStream(v: Viewport, tEvent: number = Date.now()) {
        const since = () => Date.now() - tEvent;
        this.console.log(`stream "${v.name}": start +${since()}ms`);
        // (event_us_low is stamped per frame at emit time inside the
        // demux loop — see the writeUInt32BE call below. This gives
        // "age of the currently-displayed frame" semantics for g2g,
        // not "time since wake".)

        // Race rule: cancel pending operations on every callback before
        // beginning a fresh stream.
        this.stopStream(v.name, /*sendSleep=*/ false);

        if (!v.host || !v.cameraId) return;

        await this.postJSON(`http://${v.host}/state`, { state: "wake" });

        const cam: any = systemManager.getDeviceById(v.cameraId);
        if (!cam) return;

        // Snapshot-then-stream: fire takePicture in parallel with the
        // main ffmpeg spawn below. takePicture often hits a cached
        // image and resolves in 50–300ms, vs. the ~5s before the first
        // ffmpeg-emitted frame lands. The snapshot fills the gap so the panel
        // shows the camera near-instantly on tap/event. Fire-and-forget —
        // runs in parallel with stream socket bring-up. If the stream's first
        // frame arrives before the snapshot finishes, the snapshot just
        // overpaints stale data on top of a fresher frame for ~1 paint cycle.
        // Errors are silent so a missing snapshot path doesn't break start.
        this.pushSnapshot(v, cam, tEvent).catch(() => {});

        // Fetch the panel's native dimensions from the firmware and
        // cache them on the viewport's storage. Falls back to 800x480
        // if /state is unreachable (e.g. mid-reboot). Panel dims never
        // change for a given device so this only really needs to run
        // once per discovery; refreshing on every wake costs ~5 ms and
        // self-heals if the firmware is replaced.
        const pw = parseInt(v.storage.getItem("panel_w") || "0", 10);
        const ph = parseInt(v.storage.getItem("panel_h") || "0", 10);
        let panelW = pw || 800;
        let panelH = ph || 480;
        try {
            const st = await fetch(`http://${v.host}/state`, { signal: AbortSignal.timeout(1500) }).then(r => r.json());
            if (st?.panel_width && st?.panel_height) {
                panelW = Number(st.panel_width);
                panelH = Number(st.panel_height);
                v.storage.setItem("panel_w", String(panelW));
                v.storage.setItem("panel_h", String(panelH));
            }
        } catch { /* keep cached values */ }

        // Always send panel-native dimensions (panelW x panelH). For a
        // portrait viewport we scale to the logical (panelH x panelW)
        // target then transpose 90° CW so the buffer that arrives at the
        // panel is already in the right rotation. The firmware never
        // touches pixels — the hardware JPEG decoder writes BGR888
        // straight into a DMA buffer that gets handed to the DSI engine.
        // Filter order matters here. Earlier we did
        //   scale=480:800,transpose=1
        // which intermittently produced a JPEG with a SOF marker
        // reporting 480x800 — the firmware then rejected it with
        // "expected 800x480, got 480x800". Rotating *first* and then
        // scaling to an EXPLICIT panelWxpanelH (with setsar to clear
        // any leftover aspect-ratio metadata) makes the final encoded
        // dimensions deterministic regardless of source resolution.
        const vf  = this.buildVf(v.orientation, panelW, panelH);
        const qv  = String(v.jpegQuality);
        // Diagnostic — confirms which filter chain the *currently loaded*
        // script is actually using. If you don't see this line in the
        // plugin log, the Scrypted Script editor is still on stale code
        // and a re-paste/save didn't take. If you do see it but the
        // firmware still rejects 480x800, the rotation didn't apply
        // (very rare ffmpeg build issue) and we'd need to look at
        // installed ffmpeg version.
        // (stream config log emitted after substream selection below)

        // Pull the camera's video stream, convert to ffmpeg input args, and
        // pipe through a single ffmpeg child: input → scale(lanczos) →
        // mjpeg q:v 2 → image2pipe. We then framer the raw MJPEG bytes
        // out of stdout into individual JPEGs and POST each one. This
        // beats the snapshot path because:
        //   - the camera's main encoder is producing keyframes anyway, so
        //     we're paying ~zero extra on the source side,
        //   - ffmpeg sustains real fps; the takePicture loop never could,
        //   - quality stays high (lanczos + q:v 2 ≈ visually lossless).
        // Stream-source choice drives end-to-end latency more than
        // anything else. remote-recorder hands us the high-bitrate
        // main encoder with a large GOP and the camera's own ~10s
        // prebuffer baked in — we'd watch the past, not the present.
        // Walk substreams from lowest-latency → highest-latency and
        // take the first one that resolves.
        // Source substream selection. We always want the highest-fps
        // highest-quality option that still has acceptable latency.
        // The wire cost is unaffected — we re-encode to panel-native
        // 800x480 mjpeg q:v 1 regardless of input resolution — so the
        // only tradeoff is Scrypted-side ffmpeg CPU (irrelevant here).
        //
        // Order of preference:
        //   medium-resolution  — usually the camera's main 1080p
        //                        stream at 15-30 fps, low latency
        //   local              — main stream for local clients
        //   remote             — main stream for remote clients
        //   (camera default)   — last-ditch fallback
        //
        // Explicitly NOT trying:
        //   low-resolution — preview substream, capped 5-8 fps
        //   remote-recorder — has ~10s prebuffer baked in (we'd watch
        //                     the past, not the present)
        let stream: any;
        let pickedDest = "(default)";
        // prebuffer>0 asks the rebroadcast plugin to seek back to a
        // recent keyframe so ffmpeg starts decoding immediately instead
        // of waiting for the camera's next IDR. Omitted entirely when 0
        // so the request shape is byte-identical to the old live-edge
        // behavior (no accidental prebuffer if the plugin defaults it).
        const prebufferMs = v.streamPrebufferMs;
        // Only ONE stream typically carries a maintained prebuffer (the one
        // HomeKit/NVR keeps hot — e.g. "High" with 10s here). The Medium/Low
        // substreams have none, so requesting them forces a cold RTSP connect
        // that waits for the next live keyframe (~6s). To get an instant
        // start we must pick the prebuffered stream BY ID. We downscale every
        // source to 800x480, so its native resolution is irrelevant to output
        // — the only cost is a little extra Scrypted-side decode CPU.
        let prebufferedId: string | null = null;
        let streamOptsList: any[] = [];
        try {
            streamOptsList = (await cam.getVideoStreamOptions?.()) || [];
            for (const o of streamOptsList) {
                this.console.log(
                    `streamopt "${v.name}": id=${o.id} name=${o.name} prebuffer=${o.prebuffer ?? "-"} ` +
                    `container=${o.container ?? "-"} ${o.video?.width ?? "?"}x${o.video?.height ?? "?"}@${o.video?.fps ?? "?"} ` +
                    `dest=${JSON.stringify(o.destinations ?? o.destination ?? "-")}`);
            }
            // Among streams that carry a maintained prebuffer, pick the SMALLEST
            // one that still covers the panel — minimizes Scrypted-side decode
            // cost (we downscale to 800x480 regardless). Picking the largest
            // (e.g. 5MP "High") needlessly halves the achievable fps; picking
            // below panel res would force an upscale. panel long/short edges
            // guard against the sub-panel "Low" substream.
            const panelPixels = panelW * panelH;
            const prebuffered = streamOptsList.filter(o => (o.prebuffer ?? 0) > 0);
            const covers = prebuffered
                .filter(o => (o.video?.width ?? 0) * (o.video?.height ?? 0) >= panelPixels)
                .sort((a, b) => (a.video.width * a.video.height) - (b.video.width * b.video.height));
            // Prefer smallest-that-covers; else the largest available prebuffered
            // (better an upscale than no prebuffer at all).
            const best = covers[0]
                ?? prebuffered.sort((a, b) => ((b.video?.width ?? 0) * (b.video?.height ?? 0)) - ((a.video?.width ?? 0) * (a.video?.height ?? 0)))[0];
            if (best) prebufferedId = best.id;
        } catch (e) {
            this.console.warn(`stream "${v.name}": getVideoStreamOptions failed: ${(e as Error).message}`);
        }

        // Preferred path: the prebuffered stream by id, with our backfill
        // request — hands ffmpeg a buffered keyframe immediately.
        if (prebufferMs > 0 && prebufferedId != null) {
            try {
                stream = await cam.getVideoStream({ id: prebufferedId, prebuffer: prebufferMs });
                pickedDest = `id:${prebufferedId}(prebuffered)`;
            } catch { /* fall through to destination loop */ }
        }
        // Fallback: original destination preference (no prebuffer available,
        // or the id request failed).
        if (!stream) {
            const streamOpts = (destination: string): any =>
                prebufferMs > 0 ? { destination, prebuffer: prebufferMs } : { destination };
            for (const destination of ["medium-resolution", "local", "remote"]) {
                try { stream = await cam.getVideoStream(streamOpts(destination)); pickedDest = destination; break; }
                catch { /* try next */ }
            }
        }
        if (!stream) { stream = await cam.getVideoStream(); pickedDest = "(camera-default)"; }
        // True only when we actually got the prebuffered-by-id stream — drives
        // the burst-friendly ffmpeg input flags below.
        const usingPrebuffer = pickedDest.includes("prebuffered");
        this.console.log(`stream "${v.name}": orientation=${v.orientation} panel=${panelW}x${panelH} vf="${vf}" substream=${pickedDest} prebuffer=${prebufferMs}ms usingPrebuffer=${usingPrebuffer}`);
        const ffmpegInputBuf: Buffer = await mediaManager.convertMediaObjectToBuffer(
            stream, "x-scrypted/x-ffmpeg-input");
        let ffmpegInput: any;
        try { ffmpegInput = JSON.parse(ffmpegInputBuf.toString("utf8")); }
        catch (e) {
            this.console.warn(`"${v.name}" no usable video stream for ffmpeg — skipping`);
            return;
        }
        // DIAGNOSTIC (cold-start). prebuffer=3000 didn't move first-byte, so
        // determine what source we actually got. A rebroadcast/prebuffer feed
        // is a hot localhost socket (tcp://127.0.0.1:PORT) and starts in ms;
        // a direct camera RTSP connect (rtsp://<cam-ip>) is the slow ~6s path.
        // Log the input URL (credentials stripped) + the returned stream
        // options so we can see whether prebuffer engaged.
        try {
            const args: string[] = ffmpegInput.inputArguments || [];
            const i = args.indexOf("-i");
            const rawUrl = i >= 0 && i + 1 < args.length ? String(args[i + 1]) : "(no -i)";
            const safeUrl = rawUrl.replace(/\/\/[^@/]*@/, "//***@");   // strip user:pass@
            const mso = ffmpegInput.mediaStreamOptions || {};
            this.console.log(
                `stream "${v.name}": src container=${ffmpegInput.container} url=${safeUrl} ` +
                `msoId=${mso.id} msoName=${mso.name} mso.prebuffer=${mso.prebuffer} ` +
                `mso.refreshAt=${mso.refreshAt ?? "-"} tool=${ffmpegInput.mediaStreamOptions?.tool ?? "-"}`);
            // Full input args (credentials stripped) — reveals rtsp_transport,
            // probe flags, and any source-side latency knobs Scrypted set.
            const safeArgs = args.map(a => a.replace(/\/\/[^@/]*@/, "//***@"));
            this.console.log(`stream "${v.name}": inputArgs=${JSON.stringify(safeArgs)}`);
        } catch (e) {
            this.console.warn(`stream "${v.name}": src introspection failed: ${(e as Error).message}`);
        }

        const { spawn } = require("child_process");
        const ffmpegPath =
            (mediaManager.getFFmpegPath ? await mediaManager.getFFmpegPath() : undefined) ||
            "ffmpeg";

        const abort = new AbortController();
        // Register with the cross-reload cleanup so a script re-paste
        // during an active stream tears down ffmpeg + the firmware
        // socket + the stats interval via the abort listeners below
        // — instead of orphaning them against a stale Provider.
        const releaseShutdownCleanup = (() => {
            const cleanup = () => { try { abort.abort(); } catch {} };
            pushShutdownCleaner(cleanup);
            // Once the stream ends normally (timeout / stopStream), drop
            // its cleanup so it doesn't run later against a long-dead
            // AbortController. We splice rather than mark-dead so the
            // cleanup list stays compact across many stream cycles.
            return () => {
                const G = globalThis as any;
                if (!Array.isArray(G.__viewportShutdownCleaners)) return;
                const i = G.__viewportShutdownCleaners.indexOf(cleanup);
                if (i >= 0) G.__viewportShutdownCleaners.splice(i, 1);
            };
        })();
        abort.signal.addEventListener("abort", releaseShutdownCleanup);

        // ── DATA PLANE: raw TCP socket to firmware port 81 ────────────
        // Replaces per-frame HTTP POSTs. One socket per stream session.
        // Frame format on the wire (big-endian, 16-byte v1 header):
        //   ["VPRT"][4 bytes jpeg_len][4 bytes seq][4 bytes event_us_low]
        //   followed by jpeg_len bytes of JPEG body.
        //
        // Backpressure strategy — skip-oldest:
        //   sock.write() returns false → mark backpressured, hold the
        //   NEXT frame in a single-slot `pendingFrame`. New frames
        //   arriving during backpressure REPLACE the held one (drop
        //   oldest). On 'drain', flush the held frame and clear the
        //   flag. The in-flight write is already past us — we never
        //   queue more than (1 in-flight + 1 pending) ≈ ~400KB at our
        //   frame sizes, regardless of sustained mismatch.
        //
        // No HTTP headers, no per-frame ACK round-trip, no Nagle/
        // delayed-ACK dance, no httpd worker churn.
        const net = require("net");
        let sock: any = null;
        let socketReady = false;
        // While true, we hold the most recent frame in `pendingFrame`
        // instead of calling write(). On 'drain' we flush the held
        // frame (if any) and clear the flag. Skip-oldest semantics:
        // a new ffmpeg frame arriving during backpressure replaces
        // whatever was held — the older pending frame is dropped.
        let socketBackpressured = false;
        let pendingFrame: Buffer | null = null;
        let seq = 0;
        let droppedFrames     = 0;   // dropped because socket wasn't open yet
        let droppedOldest     = 0;   // dropped from pending slot when a newer frame replaced it
        let sentFrames        = 0;
        let bytesSent         = 0;
        let flushCount        = 0;   // socket destroy+reconnect count due to buffer cap (safety net)
        // Duty cycle: framesSampled = every ffmpeg frame we considered
        // sending this window; framesUnderBp = the subset for which the
        // socket was backpressured at decision time. Ratio = % of frames
        // the link couldn't accept on demand — a continuous measure that
        // complements the point-in-time backpressured= flag.
        let framesSampled     = 0;
        let framesUnderBp     = 0;
        let lastLogUs = Date.now();
        let workBuf: Buffer = Buffer.alloc(0);

        // Latency probe — wall-clock from "ffmpeg emitted the JPEG"
        // to "kernel accepted the socket.write". With TCP_NODELAY on
        // the stream socket this is sub-millisecond steady state;
        // double-digit ms numbers here mean kernel send buffer is
        // full (= firmware can't ingest fast enough), which is fine
        // — we don't gate on it and the firmware's FIONREAD skip
        // drops the surplus before decode.
        const writeLatencies: number[] = [];

        // Write a fully-framed buffer (header + jpeg already concatenated)
        // and update accounting. Returns whatever sock.write() returned
        // so the caller can flip the backpressure flag.
        const writeFramed = (buf: Buffer): boolean => {
            const t0 = Date.now();
            const ok = sock.write(buf);
            writeLatencies.push(Date.now() - t0);
            if (writeLatencies.length > 200) writeLatencies.shift();
            bytesSent += buf.length;
            sentFrames++;
            return ok;
        };

        const openStreamSocket = () => {
            if (abort.signal.aborted) return;
            socketReady = false;
            socketBackpressured = false;
            // Drop any frame held from the previous socket — it's stale
            // and addressed to a dead connection.
            if (pendingFrame) { droppedOldest++; pendingFrame = null; }
            this.console.log(`stream "${v.name}": socket connect requested +${since()}ms`);
            sock = net.createConnection({
                host:    v.host,
                port:    81,
                noDelay: true,        // TCP_NODELAY on the outbound socket
            });
            sock.on("connect", () => {
                socketReady = true;
                this.console.log(`stream "${v.name}": socket connect open +${since()}ms`);
            });
            sock.on("drain", () => {
                socketBackpressured = false;
                if (pendingFrame && socketReady) {
                    const buf = pendingFrame;
                    pendingFrame = null;
                    const ok = writeFramed(buf);
                    if (!ok) socketBackpressured = true;
                }
            });
            sock.on("error", (e: Error) => {
                this.console.warn(`stream "${v.name}" socket: ${e.message}`);
                socketReady = false;
                if (!abort.signal.aborted) setTimeout(openStreamSocket, 500);
            });
            sock.on("close", () => {
                socketReady = false;
                if (!abort.signal.aborted) setTimeout(openStreamSocket, 500);
            });
        };
        openStreamSocket();
        abort.signal.addEventListener("abort", () => {
            try { sock?.destroy(); } catch {}
        });

        // Auto-restart accounting: cameras occasionally end their RTSP
        // stream mid-event (network blip, source rotation, etc.) and
        // ffmpeg exits clean. If the stream-timeout hasn't fired yet
        // we respawn so the panel doesn't freeze on a stale frame.
        // Capped at 5 restarts per 60s — past that we give up and
        // wait for the next camera event.
        let currentProc: any = null;
        let restartCount  = 0;
        let restartWindow = Date.now();

        const spawnFfmpeg = () => {
            if (abort.signal.aborted) return;
            workBuf = Buffer.alloc(0);   // reset framer state on each respawn
            const p = spawn(ffmpegPath, [
                // DIAGNOSTIC: "info" (was "error") surfaces ffmpeg's RTSP
                // connect/parse milestones (Input #0, stream mapping, first
                // swscaler line) which our stderr handler stamps with since()
                // — that's how we located the ~6s as ~1.1s RTSP + ~4.2s
                // keyframe wait. "-nostats" suppresses the per-~500ms frame=
                // progress flood while keeping those milestone lines. Revert
                // to "error" (and drop -nostats) once done diagnosing.
                "-hide_banner", "-loglevel", "info", "-nostats",
                // INPUT flags depend on the source path:
                //  • live-edge: aggressive low-latency tuning — no input
                //    buffering, unbuffered direct I/O, minimal probe — so a
                //    live RTSP stream is decoded straight through with the
                //    least added latency.
                //  • prebuffered: the rebroadcaster hands us a ~6s BURST of
                //    buffered frames (starting on a keyframe). "-avioflags
                //    direct" + "-fflags nobuffer" throttle that burst to tiny
                //    unbuffered socket reads — measured first-frame scaled
                //    linearly with prebuffer size, i.e. ffmpeg was draining
                //    the burst slowly, not waiting for a keyframe. Dropping
                //    those two lets ffmpeg gulp the burst and emit the buffered
                //    keyframe almost immediately. Keep genpts+discardcorrupt;
                //    let Scrypted's own probesize/analyzeduration (in
                //    inputArguments) stand.
                ...(usingPrebuffer
                    ? ["-fflags", "+genpts+discardcorrupt"]
                    : ["-fflags", "+genpts+nobuffer+discardcorrupt",
                       "-flags", "low_delay",
                       "-avioflags", "direct",
                       "-probesize", "32",
                       "-analyzeduration", "0"]),
                ...(ffmpegInput.inputArguments || []),
                "-an", "-sn",
                "-vf", vf,
                // -fps_mode drop: when the decoder is behind, throw the
                // late frame on the floor instead of queueing it. Without
                // this, ffmpeg's output queue fills up and the displayed
                // image lags further and further behind reality.
                "-fps_mode", "drop",
                "-c:v", "mjpeg", "-q:v", qv,
                "-f", "image2pipe", "-flush_packets", "1",
                "pipe:1",
            ]);
            currentProc = p;
            // Cold-start latency breakdown. The user-visible gap is
            // "first ffmpeg frame" (~5s observed), masked by the snapshot.
            // To attribute it we stamp three points:
            //   spawned     — process created (ffmpeg startup cost begins)
            //   first byte  — first stdout byte = RTSP connect + first
            //                 keyframe decoded + first re-encoded JPEG
            //                 started flushing. The spawned→first-byte gap
            //                 is the keyframe/GOP wait we're chasing.
            //   first frame — first complete JPEG framed out (≈ first byte
            //                 + one frame's worth of pipe drain)
            this.console.log(`stream "${v.name}": ffmpeg spawned +${since()}ms (substream=${pickedDest})`);

            let firstFfmpegFrameLogged = false;
            let firstSocketWriteLogged = false;
            let firstByteLogged        = false;
            p.stdout.on("data", (chunk: Buffer) => {
                if (abort.signal.aborted) return;
                if (!firstByteLogged) {
                    this.console.log(`stream "${v.name}": ffmpeg first stdout byte +${since()}ms (${chunk.length}B)`);
                    firstByteLogged = true;
                }
                workBuf = workBuf.length === 0 ? chunk : Buffer.concat([workBuf, chunk]);
                while (true) {
                    const eoi = workBuf.indexOf(Buffer.from([0xff, 0xd9]));
                    if (eoi < 0) break;
                    const frame = workBuf.subarray(0, eoi + 2);
                    workBuf = workBuf.subarray(eoi + 2);
                    if (frame.length < 4 || frame[0] !== 0xff || frame[1] !== 0xd8) continue;

                    if (!firstFfmpegFrameLogged) {
                        this.console.log(`stream "${v.name}": first ffmpeg frame +${since()}ms (jpeg=${(frame.length / 1024).toFixed(0)}KB)`);
                        firstFfmpegFrameLogged = true;
                    }

                    // Drop only when the socket isn't connected yet
                    // (initial-open race) — once it's up we keep writing
                    // through normal backpressure and let the firmware's
                    // FIONREAD skip shed superseded frames before decode.
                    if (!socketReady) {
                        droppedFrames++;
                        continue;
                    }

                    framesSampled++;
                    if (socketBackpressured) framesUnderBp++;

                    // Emergency safety net: if writableLength somehow
                    // grows past the cap (e.g. a stuck connection that
                    // never fires 'drain'), destroy the socket so the
                    // reconnect path clears the backlog. Should be rare
                    // with skip-oldest in place — steady-state writableLength
                    // stays ~1 in-flight frame.
                    const queued = sock.writableLength ?? 0;
                    if (queued > v.maxNodeBufMb * 1024 * 1024) {
                        flushCount++;
                        this.console.log(
                            `stream "${v.name}": buffer ${(queued / (1024 * 1024)).toFixed(1)}MB > ` +
                            `${v.maxNodeBufMb}MB cap — destroying socket to drop backlog (flush #${flushCount})`);
                        try { sock.destroy(); } catch {}
                        droppedFrames++;
                        continue;
                    }
                    seq++;
                    // 16-byte v1 header. Magic "VPRT" (0x56505254) lets
                    // the firmware autodetect old-vs-new clients during
                    // the rollout window. event_us_low stamped at capture
                    // time (here), not write time — so a frame held in
                    // the pending slot keeps its true age and g2g reflects
                    // any hold latency we added.
                    const header = Buffer.alloc(16);
                    header.writeUInt32BE(0x56505254, 0);   // "VPRT"
                    header.writeUInt32BE(frame.length, 4);
                    header.writeUInt32BE(seq, 8);
                    header.writeUInt32BE((Date.now() * 1000) >>> 0, 12);
                    // Single combined buffer so header + body hit the wire
                    // as one TCP segment when possible.
                    const framed = Buffer.concat([header, frame]);

                    if (socketBackpressured) {
                        // Skip-oldest: replace whatever's in the pending slot.
                        // The in-flight frame (last one we called write() on)
                        // is already past us — kernel/Node buffer is draining
                        // it. We only ever hold the freshest "next to send".
                        if (pendingFrame) droppedOldest++;
                        pendingFrame = framed;
                        continue;
                    }

                    const ok = writeFramed(framed);
                    if (!firstSocketWriteLogged) {
                        this.console.log(`stream "${v.name}": first socket.write +${since()}ms (jpeg=${(frame.length / 1024).toFixed(0)}KB)`);
                        firstSocketWriteLogged = true;
                    }
                    if (!ok) socketBackpressured = true;
                }
            });

            p.stderr.on("data", (chunk: Buffer) => {
                if (abort.signal.aborted) return;
                const text = chunk.toString("utf8").trim();
                if (!text) return;
                if (text.includes("Immediate exit requested")) return;
                // Prefix with the since() clock so each ffmpeg milestone
                // (Opening, SDP, Input #0, first frame) is placed on the
                // same timeline as spawned/first-byte — that's how we locate
                // the ~6s. Multi-line stderr chunks get one stamp.
                this.console.warn(`ffmpeg "${v.name}" +${since()}ms: ${text.replace(/\n/g, " | ")}`);
            });
            p.on("error", (e: any) => {
                if (!abort.signal.aborted) this.console.warn(`ffmpeg "${v.name}" spawn error:`, e.message);
            });
            p.on("close", (code: number) => {
                if (abort.signal.aborted) return;
                const now = Date.now();
                if (now - restartWindow > 60_000) {  // rolling 60s window
                    restartCount  = 0;
                    restartWindow = now;
                }
                if (restartCount >= 5) {
                    this.console.warn(`ffmpeg "${v.name}" exited (code=${code}) and has restarted ≥5x in the last 60s — giving up; next camera event will retry`);
                    this.stopStream(v.name);
                    return;
                }
                restartCount++;
                this.console.log(`ffmpeg "${v.name}" exited (code=${code}) — respawning (#${restartCount}/5 within window)`);
                setTimeout(spawnFfmpeg, 250);
            });
        };

        spawnFfmpeg();

        abort.signal.addEventListener("abort", () => {
            try { currentProc?.kill("SIGTERM"); } catch {}
        });

        // Unified stream-health log every 10s: Scrypted-side sent fps
        // + firmware-side painted fps side-by-side, so the user can
        // see at a glance "we sent N, the panel showed M." The gap
        // (sent - painted) is what the firmware's FIONREAD skip
        // dropped to keep the panel showing the freshest frame.
        // Includes firmware-side per-stage timings (recv/dec/paint/
        // idle min/avg/max) + glass-to-glass age of the most recent
        // painted frame. /state poll is folded in so there's one log
        // line per window instead of two interleaved timelines.
        const streamLogger = setInterval(async () => {
            const now = Date.now();
            const window = (now - lastLogUs) / 1000;
            if (window <= 0 || (sentFrames === 0 && droppedFrames === 0)) return;
            const sentRate = sentFrames / window;
            const dropRate = droppedFrames / window;
            const mbPerSec = (bytesSent / window) / (1024 * 1024);
            const sortedW  = writeLatencies.slice().sort((a, b) => a - b);
            const p50 = sortedW.length ? sortedW[Math.floor(sortedW.length * 0.5)] : 0;
            const p95 = sortedW.length ? sortedW[Math.floor(sortedW.length * 0.95)] : 0;
            const max = sortedW.length ? sortedW[sortedW.length - 1] : 0;

            // Best-effort firmware-side snapshot. Timeout < 1s so a
            // missed /state never wedges the logger.
            let painted = "?", paintedMb = "?", g2g = "?", paintedNum = -1;
            let recvStr = "?", decStr = "?", paintStr = "?", idleStr = "?";
            try {
                const st: any = await fetch(`http://${v.host}/state`, {
                    signal: AbortSignal.timeout(800),
                }).then(r => r.json());
                const fs = st?.stream;
                if (fs?.frames && fs.window_us > 0) {
                    paintedNum = fs.frames / (fs.window_us / 1e6);
                    painted    = paintedNum.toFixed(1);
                    paintedMb  = ((fs.bytes / (fs.window_us / 1e6)) / (1024 * 1024)).toFixed(2);
                    recvStr    = `${fs.recv_min_us}/${fs.recv_avg_us}/${fs.recv_max_us}`;
                    decStr     = `${fs.dec_min_us}/${fs.dec_avg_us}/${fs.dec_max_us}`;
                    paintStr   = `${fs.paint_min_us}/${fs.paint_avg_us}/${fs.paint_max_us}`;
                    idleStr    = `${fs.idle_min_us}/${fs.idle_avg_us}/${fs.idle_max_us}`;
                }
                if (fs?.last_paint_event_us_low) {
                    const nowUsLow = (Date.now() * 1000) >>> 0;
                    const diff = (nowUsLow - fs.last_paint_event_us_low) >>> 0;
                    if (diff < 30_000_000) g2g = (diff / 1000).toFixed(0) + "ms";
                }
            } catch { /* keep the local stats; firmware-side just shows ? */ }

            const skipped = paintedNum >= 0 ? Math.max(0, sentRate - paintedNum).toFixed(1) : "?";
            // node_buf = bytes Scrypted has handed to socket.write but
            // the kernel send buffer hasn't accepted yet — they sit in
            // Node's internal queue, NOT on the wire. Divide by the
            // current send rate to estimate how many seconds of
            // already-emitted frames are waiting at the source. This
            // is the load-bearing piece of the g2g "buffer depth"
            // story: when this is large, the firmware can't possibly
            // be showing the freshest bytes because we haven't even
            // sent them yet.
            const nodeBufBytes = sock?.writableLength ?? 0;
            const sentBps      = mbPerSec * 1024 * 1024;
            const nodeBufMs    = sentBps > 0 ? (nodeBufBytes / sentBps * 1000).toFixed(0) : "?";
            this.console.log(
                `stream "${v.name}": sent=${sentRate.toFixed(1)}fps painted=${painted}fps ` +
                `(fw-skipped=${skipped}fps, drop-oldest=${droppedOldest}, drops=${droppedFrames}, flushes=${flushCount}) ` +
                `${mbPerSec.toFixed(2)}MB/s sent / ${paintedMb}MB/s painted | ` +
                `socket.write p50=${p50}ms p95=${p95}ms max=${max}ms backpressured=${socketBackpressured} bp=${framesSampled > 0 ? ((framesUnderBp / framesSampled) * 100).toFixed(0) : "?"}% ` +
                `node_buf=${(nodeBufBytes / 1024).toFixed(0)}KB≈${nodeBufMs}ms/${v.maxNodeBufMb}MB cap | ` +
                `recv=${recvStr}us dec=${decStr}us paint=${paintStr}us idle=${idleStr}us | g2g=${g2g}`);

            droppedFrames = 0;
            droppedOldest = 0;
            framesSampled = 0;
            framesUnderBp = 0;
            sentFrames    = 0;
            bytesSent     = 0;
            writeLatencies.length = 0;
            lastLogUs     = now;
        }, 10_000);
        abort.signal.addEventListener("abort", () => clearInterval(streamLogger));

        const timeoutMs = v.idleTimeoutMs > 0 ? v.idleTimeoutMs : DEFAULT_IDLE_TIMEOUT_MS;
        const timeout = setTimeout(() => {
            this.console.log(`"${v.name}": Scrypted-side stream timeout — stopping`);
            this.stopStream(v.name);
        }, timeoutMs);
        // Clear the idle timer on ANY teardown path, not just stopStream.
        // On a StartStop.stop()/re-paste drain the stream is killed via its
        // abort cleaner (not stopStream), which would otherwise leave this
        // setTimeout dangling for up to idleTimeoutMs (default 60s).
        abort.signal.addEventListener("abort", () => clearTimeout(timeout));

        // The latest spawned ffmpeg child is held in the spawnFfmpeg
        // closure (currentProc); the abort signal listener kills it on
        // shutdown. We don't store the proc in the streams entry
        // because it can change across auto-restarts.
        this.streams.set(v.name, { timeout, abort });
    }

    stopStream(name: string, sendSleep = true) {
        const s = this.streams.get(name);
        if (!s) return;
        s.abort.abort();   // aborts the in-flight ffmpeg child via its listener
        if (s.interval) clearInterval(s.interval);
        clearTimeout(s.timeout);
        this.streams.delete(name);
        const v = this.findByName(name);
        if (sendSleep && v?.host) {
            this.postJSON(`http://${v.host}/state`, { state: "sleep" }).catch(() => {});
        }
    }

    // First-paint fast path. takePicture → resize/rotate to panel
    // native → POST /frame. Tries three transforms in order of cost:
    //   1. sharp   — libvips bindings, ~5-15ms per image, handles
    //                resize + rotate in one call. Not always present
    //                in Scrypted's plugin sandbox.
    //   2. mediaManager.convertMediaObjectToBuffer with size hint —
    //                Scrypted's native converter (often vips-backed).
    //                Resize-capable; rotation support varies. We only
    //                use it for landscape (no rotate needed).
    //   3. ffmpeg one-shot — old slow path, ~500-700ms cold start.
    //                Always works; the safety net.
    private async pushSnapshot(v: Viewport, cam: any, tEvent: number = Date.now()) {
        const since = () => Date.now() - tEvent;
        this.console.log(`snapshot "${v.name}": start +${since()}ms`);
        let mo: any;
        try { mo = await cam.takePicture({ reason: "event" }); }
        catch (e) { return; }                 // camera doesn't support snapshots
        if (!mo) return;

        const srcJpeg: Buffer = await mediaManager.convertMediaObjectToBuffer(mo, "image/jpeg");
        if (!srcJpeg || srcJpeg.length < 4) return;
        this.console.log(`snapshot "${v.name}": takePicture +${since()}ms`);

        // Cached dims from prior /state read; falls back to 800x480.
        const panelW = parseInt(v.storage.getItem("panel_w") || "0", 10) || 800;
        const panelH = parseInt(v.storage.getItem("panel_h") || "0", 10) || 480;
        const needsRotate = v.orientation === "portrait";

        let transformed: Buffer = Buffer.alloc(0);
        let path = "";

        // Path 1: sharp. require()-fail caught at the boundary so a
        // missing native module just falls through.
        //
        // Quality math: ffmpeg's mjpeg -q:v 1 corresponds to sharp JPEG
        // quality ~99-100. At jpegQuality=1 emit 100; at 10 emit ~82;
        // at 31 emit ~40. chromaSubsampling 4:4:4 at the top end (≤2)
        // so colored edges don't smear — sharp's default 4:2:0 is
        // half-rate chroma and was the dominant visible artifact at
        // panel-native resolution.
        //
        // mozjpeg: false intentionally. mozjpeg gave us ~3-4× slower
        // encode (sharp transform 1.6s vs 400ms) for a maybe-5% file
        // size win that we can't perceive at 800x480. libjpeg-turbo
        // default is the right call when first-paint latency matters.
        if (!transformed.length) {
            try {
                const sharp = require("sharp");
                let img = sharp(srcJpeg, { failOnError: false });
                if (needsRotate) img = img.rotate(90);
                const sharpQuality = Math.min(100, 102 - v.jpegQuality * 2);
                const chroma = v.jpegQuality <= 2 ? "4:4:4" : "4:2:0";
                transformed = await img
                    .resize(panelW, panelH, { fit: "fill", kernel: "lanczos3" })
                    .jpeg({ quality: sharpQuality, chromaSubsampling: chroma })
                    .toBuffer();
                path = "sharp";
            } catch { /* fall through */ }
        }

        // Path 2: Scrypted's native converter. Only used for landscape
        // because the mime-parameter spec has no documented rotation
        // and most implementations don't support it.
        if (!transformed.length && !needsRotate) {
            try {
                transformed = await mediaManager.convertMediaObjectToBuffer(
                    mo, `image/jpeg;width=${panelW};height=${panelH}`);
                if (transformed?.length) path = "media-mgr";
            } catch { /* fall through */ }
        }

        // Path 3: ffmpeg fallback. The slow ~500ms cold-start path.
        if (!transformed.length) {
            const vf = this.buildVf(v.orientation, panelW, panelH);
            const { spawn } = require("child_process");
            const ffmpegPath =
                (mediaManager.getFFmpegPath ? await mediaManager.getFFmpegPath() : undefined) ||
                "ffmpeg";
            transformed = await new Promise<Buffer>((resolve, reject) => {
                const p = spawn(ffmpegPath, [
                    "-hide_banner", "-loglevel", "error",
                    "-f", "image2pipe", "-i", "pipe:0",
                    "-vf", vf,
                    "-frames:v", "1",
                    "-c:v", "mjpeg", "-q:v", String(v.jpegQuality),
                    "-f", "image2pipe", "pipe:1",
                ]);
                const chunks: Buffer[] = [];
                p.stdout.on("data", (c: Buffer) => chunks.push(c));
                p.on("close", (code: number) => {
                    if (code !== 0) reject(new Error(`ffmpeg snapshot exit ${code}`));
                    else resolve(Buffer.concat(chunks));
                });
                p.on("error", reject);
                p.stdin.on("error", () => {});
                p.stdin.end(srcJpeg);
                setTimeout(() => { try { p.kill("SIGTERM"); } catch {} }, 2000);
            }).catch(() => Buffer.alloc(0));
            if (transformed.length) path = "ffmpeg";
        }

        if (transformed.length < 4) return;
        this.console.log(`snapshot "${v.name}": transform +${since()}ms via ${path} (${(transformed.length / 1024).toFixed(0)}KB)`);

        try {
            this.console.log(`snapshot "${v.name}": post sent +${since()}ms`);
            const res = await fetch(`http://${v.host}/frame`, {
                method: "POST",
                headers: { "Content-Type": "image/jpeg" },
                body: transformed,
                signal: AbortSignal.timeout(2000),
            });
            await res.text().catch(() => "");
            // post_acked is the snapshot's true glass-to-glass — /frame
            // returns after display_flip_back_buffer, so the firmware
            // has the new pixels queued for the DPI scanout by then.
            this.console.log(`snapshot "${v.name}": post acked +${since()}ms ← first user-visible paint`);
        } catch { /* stream is starting anyway */ }
    }

    // ffmpeg -vf filter chain producing panel-native 800x480 BGR888.
    // Used by both startStream (live) and pushSnapshot (one-shot
    // ffmpeg fallback). Rotation goes FIRST so the final mjpeg encoder
    // sees the exact target dimensions — earlier we observed mjpeg
    // writing pre-rotation dims into the JPEG SOF marker when scale
    // came first, breaking the firmware's strict dim check.
    private buildVf(orientation: string, panelW: number, panelH: number): string {
        return orientation === "portrait"
            ? `transpose=1,scale=${panelW}:${panelH}:flags=lanczos,setsar=1`
            : `scale=${panelW}:${panelH}:flags=lanczos,setsar=1`;
    }

    private findByName(name: string): Viewport | undefined {
        for (const v of this.viewports.values()) if (v.name === name) return v;
        return undefined;
    }

    // ------------------------------------------------------------------------
    // Inbound: device → Scrypted POST /state
    // ------------------------------------------------------------------------

    async onRequest(request: HttpRequest, response: HttpResponse) {
        if (request.method !== "POST") { response.send("", { code: 405 }); return; }
        if (!request.url.endsWith("/state")) { response.send("", { code: 404 }); return; }

        let body: any;
        try { body = JSON.parse(request.body); }
        catch { response.send("invalid JSON", { code: 400 }); return; }

        const { viewport, state } = body ?? {};
        const v = typeof viewport === "string" ? this.findByName(viewport) : undefined;
        if (!v) { response.send(`unknown viewport: ${viewport}`, { code: 404 }); return; }
        if (state !== "wake" && state !== "sleep") {
            response.send("state must be wake or sleep", { code: 400 });
            return;
        }

        this.console.log(`recv "${viewport}" -> ${state} (device-initiated)`);

        if (state === "wake") {
            await this.startStream(v);
        } else {
            this.stopStream(v.name, /*sendSleep=*/ false);
        }
        response.send("", { code: 204 });
    }

    // ------------------------------------------------------------------------
    // Parent Settings — informational only; per-viewport tuning lives on each
    // child's own Settings page.
    // ------------------------------------------------------------------------

    async getSettings(): Promise<Setting[]> {
        const count = this.viewports.size;
        return [
            {
                key: "viewport_count",
                title: "Registered viewports",
                description: "Number of child viewport bindings under this parent. Each one's host / camera / brightness / orientation / fps lives on its own Settings page.",
                value: String(count),
                readonly: true,
            } as any,
            {
                key: "callback_base",
                title: "Callback base URL",
                description: "Endpoint the firmware POSTs back to for tap-initiated wake/sleep.",
                value: this.scryptedBase || "(not yet resolved)",
                readonly: true,
            } as any,
        ];
    }

    async putSetting(_key: string, _value: SettingValue) {}

    // ------------------------------------------------------------------------
    // Tiny HTTP helper
    // ------------------------------------------------------------------------

    private async postJSON(url: string, body: any) {
        const res = await fetch(url, {
            method:  "POST",
            headers: { "Content-Type": "application/json" },
            body:    JSON.stringify(body),
            signal:  AbortSignal.timeout(HTTP_TIMEOUT_MS),
        });
        if (!res.ok && res.status !== 204) {
            throw new Error(`POST ${url} -> ${res.status}`);
        }
    }
}

export default ScryptedViewportProvider;
