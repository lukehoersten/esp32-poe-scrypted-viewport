// Scrypted Viewport — v1 Scripts-plugin script
//
// SCRIPT_VERSION is bumped on every commit that touches this file.
// The boot log emits it so we can verify the user re-pasted the
// latest version when reading the plugin console. Format is the
// short git hash of the commit that added this constant — if the
// hash in the log doesn't match the HEAD this file came from, the
// Scrypted Script editor is still on stale code.
const SCRIPT_VERSION = "504360a";
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

// Tuning constants. Frame interval is also exposed on the parent's
// Settings page so it can be tweaked without editing the script.
const DEFAULT_FRAME_INTERVAL_MS  = 500;   // ~2 fps — snapshot-based; cameras typically can't sustain faster
const REREGISTER_INTERVAL_MS     = 5 * 60_000;
// 5s gives /state + /config posts enough headroom to slip in between
// /frame POSTs when the device is mid-stream. The firmware's single
// httpd task processes one connection at a time; under heavy /frame
// load a /state {wake} can queue behind 1–3 in-flight /frames before
// landing. 1s was tripping when a burst of camera events triggered
// back-to-back startStream calls.
const HTTP_TIMEOUT_MS            = 5_000;
const DEFAULT_IDLE_TIMEOUT_MS    = 60_000;
const DEFAULT_BRIGHTNESS         = 80;

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
    get frameIntervalMs(): number {
        const v = this.storage.getItem("frame_interval_ms");
        const parsed = v ? parseInt(v, 10) : NaN;
        return Number.isFinite(parsed) ? Math.max(33, parsed) : DEFAULT_FRAME_INTERVAL_MS;
    }
    // Which camera-event types wake this viewport. Empty = tap-only,
    // never woken by Scrypted. Default = all three (doorbell + motion +
    // person detection).
    get triggers(): Set<string> {
        const v = this.storage.getItem("triggers");
        if (v === null) return new Set(["doorbell", "motion", "person"]);
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
                description: "Which camera-event types automatically wake the viewport. Clear all of them for tap-only mode (the viewport never wakes from Scrypted; user must tap to see the camera).",
                choices: ["doorbell", "motion", "person"],
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
                key: "frame_interval_ms",
                title: "Frame push interval (ms)",
                description: `How often a JPEG snapshot is POSTed during an active stream. Lower = smoother but more network + CPU; clamped to ≥ 33 ms (~30 fps max). Default ${DEFAULT_FRAME_INTERVAL_MS}.`,
                type: "number",
                value: this.frameIntervalMs,
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

        // Live device snapshot: GET /state + /config in parallel with a
        // short timeout, then render as a read-only "Status" section. If
        // the device is offline we still surface the binding fields so the
        // operator can change them — the status fields just say "offline".
        if (this.host) {
            try {
                const ctrl = AbortSignal.timeout(1500);
                const [stateRes, configRes] = await Promise.all([
                    fetch(`http://${this.host}/state`,  { signal: ctrl }).then(r => r.json()),
                    fetch(`http://${this.host}/config`, { signal: ctrl }).then(r => r.json()),
                ]);
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
            // multi-select arrives as array; serialise to JSON for storage
            this.storage.setItem("triggers", JSON.stringify(Array.isArray(value) ? value : []));
        } else {
            this.storage.setItem(key, String(value ?? ""));
        }
        await this.provider.onBindingChanged(this);
    }
}

// ============================================================================
// Parent: provider + HTTP handler + global tuning
// ============================================================================

class ScryptedViewportProvider extends ScryptedDeviceBase
    implements DeviceProvider, DeviceCreator, HttpRequestHandler, Settings {

    private viewports = new Map<string, Viewport>();             // nativeId -> child instance
    private listeners = new Map<string, EventListenerRegister>(); // nativeId -> camera event listener
    streams = new Map<string, {                                   // viewport name -> stream control (accessed by Viewport.putSetting for manual wake/sleep)
        timeout:   NodeJS.Timeout;
        abort:     AbortController;       // also tears down the ffmpeg child via its listener
        interval?: NodeJS.Timeout;        // legacy snapshot-poll mode
    }>();
    private scryptedBase = "";

    // Per-host undici Agent so /frame POSTs reuse the TCP connection
    // and pipeline two-in-flight. Reusing the socket saves the SYN +
    // SYN-ACK + ACK round-trip (~1ms on LAN, more painful on Wi-Fi),
    // and the connections:2 cap lets us overlap upload of frame N+1
    // with the firmware's decode-and-paint of frame N. pipelining:0
    // because esp_http_server doesn't support HTTP/1.1 request
    // pipelining on a single connection — we want two independent
    // sockets, not two requests stacked on one.
    private dispatchers = new Map<string, any>();
    private undiciAvailable: boolean | undefined;
    private dispatcherFor(host: string): any {
        // Try undici exactly once per provider lifetime. Scrypted's
        // plugin sandbox doesn't always expose it as a require()-able
        // module — when that's the case we silently fall back to
        // plain fetch (no socket pooling). Functionally still works,
        // just costs the SYN+SYN-ACK+ACK round-trip per POST.
        if (this.undiciAvailable === false) return undefined;
        let d = this.dispatchers.get(host);
        if (d !== undefined) return d;
        try {
            const { Agent } = require("undici");
            d = new Agent({
                keepAliveTimeout:    30_000,
                keepAliveMaxTimeout: 60_000,
                connections:         2,
                pipelining:          0,
            });
            this.dispatchers.set(host, d);
            this.undiciAvailable = true;
            return d;
        } catch {
            this.undiciAvailable = false;
            this.console.warn(`undici not available in this Scrypted runtime — falling back to per-POST sockets (no keep-alive, no 2-way pipelining at the TCP layer)`);
            return undefined;
        }
    }

    constructor(nativeId?: string) {
        super(nativeId);
        this.start().catch(e => this.console.error("start failed", e));
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


    private async start() {
        // endpointManager.getInsecurePublicLocalEndpoint() takes a nativeId
        // (string) — passing this.id (numeric Scrypted DB ID) throws
        // "invalid nativeId N". this.nativeId is the right key, and an
        // omitted nativeId falls back to the plugin's own endpoint.
        const raw = await endpointManager.getInsecurePublicLocalEndpoint(this.nativeId);
        this.scryptedBase = raw.replace(/\/$/, "");
        this.console.log(`Scrypted Viewport up (script=${SCRIPT_VERSION}). Callback URL base: ${this.scryptedBase}`);

        // Re-discover every known child so Scrypted reattaches its storage
        // to the nativeId. Without this, `new Viewport(...)` instantiates
        // with `this.storage === undefined` and every storage-backed getter
        // (host / cameraId / orientation / ...) throws on script reload.
        // Then eagerly instantiate so each child's registration + camera
        // event subscription happen at plugin load.
        for (const nativeId of this.childIds) {
            try {
                // Use the persisted display_name as the canonical device
                // name so a script reload doesn't reset it to the nativeId.
                // First-time provision falls back to the nativeId.
                const displayName =
                    deviceManager.getDeviceStorage(nativeId).getItem("display_name") || nativeId;
                await deviceManager.onDeviceDiscovered({
                    providerNativeId: this.nativeId,
                    nativeId,
                    name: displayName,
                    type: ScryptedDeviceType.SmartDisplay,
                    interfaces: [ScryptedInterface.Settings],
                });
                await this.getDevice(nativeId);
            }
            catch (e) { this.console.warn(`load child ${nativeId} failed:`, (e as Error).message); }
        }

        // Periodic re-register so a device that rebooted or got a new IP
        // re-syncs without manual intervention.
        setInterval(() => {
            for (const v of this.viewports.values()) {
                this.registerViewport(v).catch(() => {});
            }
        }, REREGISTER_INTERVAL_MS);
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
        if (!v.cameraId) return;
        const cam = systemManager.getDeviceById(v.cameraId);
        const tag = v.name || v.storage.getItem("display_name") || v.nativeId;
        if (!cam) {
            this.console.warn(`viewport "${tag}": camera ${v.cameraId} not found`);
            return;
        }
        const ifaces = [
            ScryptedInterface.BinarySensor,    // doorbell
            ScryptedInterface.MotionSensor,    // motion
            ScryptedInterface.ObjectDetector,  // person / etc
        ];
        const reg = cam.listen(ifaces, (source, details, data) => {
            this.handleCameraEvent(v, details, data);
        });
        this.listeners.set(v.nativeId!, reg);
        this.console.log(`viewport "${tag}": subscribed to "${cam.name}"`);
    }

    private detachListener(nativeId: string) {
        const reg = this.listeners.get(nativeId);
        if (reg) {
            try { reg.removeListener(); } catch {}
            this.listeners.delete(nativeId);
        }
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
        const iface = details.eventInterface;
        const allowed = v.triggers;
        let trigger = false;
        if (allowed.has("doorbell") && iface === ScryptedInterface.BinarySensor && data === true) trigger = true;
        if (allowed.has("motion")   && iface === ScryptedInterface.MotionSensor && data === true) trigger = true;
        if (allowed.has("person")   && iface === ScryptedInterface.ObjectDetector) {
            const detections = data?.detections ?? [];
            if (detections.some((d: any) => d?.className === "person")) trigger = true;
        }
        if (!trigger) return;
        this.console.log(`event ${iface} -> "${v.name}": wake`);
        // If a stream is already in flight for this viewport, the event
        // is just reinforcement — the existing ffmpeg child is already
        // pushing frames. We do NOT relaunch (would race with previous).
        if (this.streams.has(v.name)) return;
        if (this.streamStarting.has(v.nativeId!)) return;
        this.streamStarting.add(v.nativeId!);
        this.startStream(v)
            .catch(e => this.console.error("startStream failed", e))
            .finally(() => this.streamStarting.delete(v.nativeId!));
    }

    async startStream(v: Viewport) {
        // Race rule: cancel pending operations on every callback before
        // beginning a fresh stream.
        this.stopStream(v.name, /*sendSleep=*/ false);

        if (!v.host || !v.cameraId) return;

        await this.postJSON(`http://${v.host}/state`, { state: "wake" });

        const cam: any = systemManager.getDeviceById(v.cameraId);
        if (!cam) return;

        // Snapshot-then-stream: fire takePicture in parallel with the
        // main ffmpeg spawn below. takePicture often hits a cached
        // image and resolves in 50–300ms, vs. 0.5–3s before the first
        // ffmpeg-emitted frame lands (ffmpeg startup + RTSP connect +
        // first H.264 keyframe wait). The snapshot fills the gap so
        // the panel shows the camera near-instantly on tap/event.
        // Fire-and-forget — if it loses the race to the first stream
        // frame (which has a higher X-Frame-Seq), the firmware will
        // stale-drop it. Errors are silent so a missing snapshot path
        // doesn't break the stream start.
        this.pushSnapshot(v, cam).catch(() => {});

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
        const vf = v.orientation === "portrait"
            ? `transpose=1,scale=${panelW}:${panelH}:flags=lanczos,setsar=1`
            : `scale=${panelW}:${panelH}:flags=lanczos,setsar=1`;
        const fps = Math.max(1, Math.round(1000 / v.frameIntervalMs));
        // Diagnostic — confirms which filter chain the *currently loaded*
        // script is actually using. If you don't see this line in the
        // plugin log, the Scrypted Script editor is still on stale code
        // and a re-paste/save didn't take. If you do see it but the
        // firmware still rejects 480x800, the rotation didn't apply
        // (very rare ffmpeg build issue) and we'd need to look at
        // installed ffmpeg version.
        this.console.log(`stream "${v.name}": orientation=${v.orientation} panel=${panelW}x${panelH} vf="${vf}"`);

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
        let stream: any;
        const destOrder = ["low-resolution", "medium-resolution", "local", "remote", "remote-recorder"];
        for (const destination of destOrder) {
            try { stream = await cam.getVideoStream({ destination }); break; }
            catch { /* try next */ }
        }
        if (!stream) stream = await cam.getVideoStream();
        const ffmpegInputBuf: Buffer = await mediaManager.convertMediaObjectToBuffer(
            stream, "x-scrypted/x-ffmpeg-input");
        let ffmpegInput: any;
        try { ffmpegInput = JSON.parse(ffmpegInputBuf.toString("utf8")); }
        catch (e) {
            this.console.warn(`"${v.name}" no usable video stream for ffmpeg — skipping`);
            return;
        }

        const { spawn } = require("child_process");
        const ffmpegPath =
            (mediaManager.getFFmpegPath ? await mediaManager.getFFmpegPath() : undefined) ||
            "ffmpeg";

        const abort = new AbortController();

        // Single-flight: only one POST /frame in flight at a time. The
        // firmware's JPEG decoder mutex returns 503 otherwise; we just
        // drop the spare frames silently — natural rate-limiter.
        // Allow up to MAX_INFLIGHT concurrent /frame POSTs so that the
        // network upload of frame N+1 overlaps with the firmware's
        // decode+paint of frame N. The firmware's JPEG decoder mutex
        // serialises the decode itself; this only buys us the upload
        // overlap (≈ half of total /frame wall time). Keep this in
        // sync with cfg.max_open_sockets in main/http_api.c — that
        // sits at 4 (2 for streaming + 2 spare for /state, /config).
        const MAX_INFLIGHT = 2;
        let inFlight = 0;
        let droppedFrames = 0;
        let sentFrames    = 0;
        let lastLogUs = Date.now();
        let workBuf: Buffer = Buffer.alloc(0);

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
                "-hide_banner", "-loglevel", "error",
                // Latency tuning on the INPUT side: don't buffer, don't
                // probe, decode straight through. probesize/analyzeduration
                // at the minimum keeps ffmpeg from sitting on the first
                // ~5s of source to learn the stream layout.
                "-fflags", "+genpts+nobuffer+discardcorrupt",
                "-flags", "low_delay",
                "-avioflags", "direct",
                "-probesize", "32",
                "-analyzeduration", "0",
                ...(ffmpegInput.inputArguments || []),
                "-an", "-sn",
                "-vf", `${vf},fps=${fps}`,
                // -fps_mode drop: when the decoder is behind, throw the
                // late frame on the floor instead of queueing it. Without
                // this, ffmpeg's output queue fills up and the displayed
                // image lags further and further behind reality.
                "-fps_mode", "drop",
                "-c:v", "mjpeg", "-q:v", "2",
                "-f", "image2pipe", "-flush_packets", "1",
                "pipe:1",
            ]);
            currentProc = p;

            p.stdout.on("data", (chunk: Buffer) => {
                if (abort.signal.aborted) return;
                workBuf = workBuf.length === 0 ? chunk : Buffer.concat([workBuf, chunk]);
                while (true) {
                    const eoi = workBuf.indexOf(Buffer.from([0xff, 0xd9]));
                    if (eoi < 0) break;
                    const frame = workBuf.subarray(0, eoi + 2);
                    workBuf = workBuf.subarray(eoi + 2);
                    if (frame.length < 4 || frame[0] !== 0xff || frame[1] !== 0xd8) continue;

                    if (inFlight >= MAX_INFLIGHT) { droppedFrames++; continue; }
                    const emitMs        = Date.now();
                    const depthAtQueue  = inFlight;       // BEFORE the increment
                    inFlight++;
                    sentFrames++;
                    this.pushStreamFrame(v, frame, abort, emitMs, depthAtQueue)
                        .catch(e => {
                            if (abort.signal.aborted) return;
                            const err = (e as Error);
                            const cause = (err as any)?.cause?.code || "";
                            this.console.warn(`pushStreamFrame "${v.name}":`, err.message, cause ? `(${cause})` : "");
                        })
                        .finally(() => { inFlight--; });
                }
            });

            p.stderr.on("data", (chunk: Buffer) => {
                if (abort.signal.aborted) return;
                const text = chunk.toString("utf8").trim();
                if (!text) return;
                if (text.includes("Immediate exit requested")) return;
                this.console.warn(`ffmpeg "${v.name}": ${text}`);
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

        // Periodic skip-rate log. The metric that matters is *delivered*
        // fps vs. requested rate, not raw drop count — at low intervals
        // ffmpeg's fps filter emits timestamp-clumped pairs and our
        // single-flight guard correctly skips the second, but the
        // first still painted on time. Only warn if delivered fps falls
        // below 75% of target.
        const skipLogger = setInterval(() => {
            const now = Date.now();
            const window = (now - lastLogUs) / 1000;
            if (window > 0 && (sentFrames > 0 || droppedFrames > 0)) {
                const sentRate   = sentFrames / window;
                const targetRate = 1000 / v.frameIntervalMs;
                const dropRate   = droppedFrames / window;
                // Two distinct symptoms to call out:
                //  - drops > 25% of target → backpressure: HTTP POST or
                //    panel is the bottleneck, advice is to raise interval.
                //  - delivered < 60% AND drops are negligible → ffmpeg's
                //    fps filter / camera substream simply isn't producing
                //    the requested rate. Raising interval won't help;
                //    surface a different message.
                if (dropRate > targetRate * 0.25) {
                    this.console.log(`"${v.name}": backpressure — delivered ~${sentRate.toFixed(1)} fps vs target ${targetRate.toFixed(1)} fps, dropped ~${dropRate.toFixed(1)} fps over ${window.toFixed(1)}s (POST stack can't keep up; raise frame_interval_ms slightly to flatten this)`);
                } else if (sentRate < targetRate * 0.6 && sentRate > 0) {
                    this.console.log(`"${v.name}": source-limited — delivered ~${sentRate.toFixed(1)} fps vs target ${targetRate.toFixed(1)} fps over ${window.toFixed(1)}s (camera substream / ffmpeg fps filter producing below requested rate; raising frame_interval_ms won't help)`);
                }
                droppedFrames = 0;
                sentFrames    = 0;
                lastLogUs     = now;
            }
        }, 10_000);
        abort.signal.addEventListener("abort", () => clearInterval(skipLogger));

        const timeoutMs = v.idleTimeoutMs > 0 ? v.idleTimeoutMs : DEFAULT_IDLE_TIMEOUT_MS;
        const timeout = setTimeout(() => {
            this.console.log(`"${v.name}": Scrypted-side stream timeout — stopping`);
            this.stopStream(v.name);
        }, timeoutMs);

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
        if (v) this.frameSeq.delete(v.nativeId!);
        if (sendSleep && v?.host) {
            this.postJSON(`http://${v.host}/state`, { state: "sleep" }).catch(() => {});
        }
    }

    // Scrypted-side per-fetch timing. We log aggregated stats every
    // 10 fetches instead of a single-sample line — single samples were
    // hiding tail latency. Each fetch records four buckets:
    //   emit→post : ms from ffmpeg-emitted-the-jpeg to fetch() called.
    //               Nonzero means the inFlight queue gated us.
    //   req       : fetch() called → Response headers arrive. Body
    //               upload + server processing + status line round-trip.
    //   body-read : Response received → response body drained. Should
    //               be near-zero (firmware returns empty 204/200/409).
    //   wall      : total of all three for sanity.
    // Plus inflight-at-queue (0/1/2) so we can see whether pipelining
    // is actually overlapping anything, and a stale-drop counter for
    // X-Frame-Drop responses from the firmware.
    private fetchCount = new Map<string, number>();
    private fetchSamples = new Map<string, {
        // Script-side wall-clock spans, ms.
        emit:    number[];   // ffmpeg emit → fetch() call
        wall:    number[];   // emit → fetch resolved (end-to-end)
        req:     number[];   // fetch() → Response headers
        bread:   number[];   // Response → body drained
        // Firmware-side spans from Server-Timing (already ms).
        fw_recv: number[];   // body bytes off the wire
        fw_dec:  number[];   // hardware JPEG decode
        fw_paint:number[];   // back buffer flip
        fw_post: number[];   // state counters + unlock + resp headers
        fw_tot:  number[];   // firmware total (recv + dec + paint + post)
        // Derived: time fetch() spent in flight that firmware DIDN'T
        // see. net_up = req - fw_tot ≈ TCP setup + body wire time
        // + httpd dispatch.
        net_up:  number[];
        depths:  { d0: number; d1: number; d2: number };
        staleDrops: number;
    }>();

    private bucketsFor(nid: string) {
        let s = this.fetchSamples.get(nid);
        if (!s) {
            s = { emit: [], wall: [], req: [], bread: [],
                  fw_recv: [], fw_dec: [], fw_paint: [], fw_post: [], fw_tot: [],
                  net_up: [],
                  depths: { d0: 0, d1: 0, d2: 0 }, staleDrops: 0 };
            this.fetchSamples.set(nid, s);
        }
        return s;
    }

    // Parse a Server-Timing header like
    //   recv;dur=12.3, dec;dur=4.5, paint;dur=0.1, post;dur=0.2, handle;dur=17.1
    // into a record of name→duration_ms. Robust to whitespace and
    // missing entries; returns {} on malformed input.
    private parseServerTiming(h: string | null): Record<string, number> {
        const out: Record<string, number> = {};
        if (!h) return out;
        for (const tok of h.split(",")) {
            const parts = tok.trim().split(";");
            const name  = parts[0]?.trim();
            if (!name) continue;
            for (let i = 1; i < parts.length; i++) {
                const [k, v] = parts[i].trim().split("=");
                if (k === "dur") {
                    const n = Number(v);
                    if (Number.isFinite(n)) out[name] = n;
                }
            }
        }
        return out;
    }

    // First-paint fast path. takePicture → quick ffmpeg one-shot to
    // transpose+scale to panel-native dims → POST /frame. Shares the
    // viewport's frameSeq counter so a slow snapshot can't paint over
    // a faster stream frame — whichever loses the race gets stale-
    // dropped by the firmware.
    private async pushSnapshot(v: Viewport, cam: any) {
        const t0 = Date.now();
        let mo: any;
        try { mo = await cam.takePicture({ reason: "event" }); }
        catch (e) { return; }                 // camera doesn't support snapshots
        if (!mo) return;

        const srcJpeg: Buffer = await mediaManager.convertMediaObjectToBuffer(mo, "image/jpeg");
        if (!srcJpeg || srcJpeg.length < 4) return;

        // Cached dims from prior /state read; falls back to 800x480.
        const panelW = parseInt(v.storage.getItem("panel_w") || "0", 10) || 800;
        const panelH = parseInt(v.storage.getItem("panel_h") || "0", 10) || 480;
        const vf = v.orientation === "portrait"
            ? `transpose=1,scale=${panelW}:${panelH}:flags=lanczos,setsar=1`
            : `scale=${panelW}:${panelH}:flags=lanczos,setsar=1`;

        const { spawn } = require("child_process");
        const ffmpegPath =
            (mediaManager.getFFmpegPath ? await mediaManager.getFFmpegPath() : undefined) ||
            "ffmpeg";

        const transformed: Buffer = await new Promise<Buffer>((resolve, reject) => {
            const p = spawn(ffmpegPath, [
                "-hide_banner", "-loglevel", "error",
                "-f", "image2pipe", "-i", "pipe:0",
                "-vf", vf,
                "-frames:v", "1",
                "-c:v", "mjpeg", "-q:v", "2",
                "-f", "image2pipe", "pipe:1",
            ]);
            const chunks: Buffer[] = [];
            p.stdout.on("data", (c: Buffer) => chunks.push(c));
            p.on("close", (code: number) => {
                if (code !== 0) reject(new Error(`ffmpeg snapshot exit ${code}`));
                else resolve(Buffer.concat(chunks));
            });
            p.on("error", reject);
            p.stdin.on("error", () => {});   // suppress EPIPE if ffmpeg fails early
            p.stdin.end(srcJpeg);
            // Hard cap so a stuck ffmpeg can't delay the main stream
            // (which is starting in parallel anyway).
            setTimeout(() => { try { p.kill("SIGTERM"); } catch {} }, 2000);
        }).catch(() => Buffer.alloc(0));

        if (transformed.length < 4) return;

        const seq = (this.frameSeq.get(v.nativeId!) || 0) + 1;
        this.frameSeq.set(v.nativeId!, seq);
        try {
            const res = await fetch(`http://${v.host}/frame`, {
                method: "POST",
                headers: { "Content-Type": "image/jpeg", "X-Frame-Seq": String(seq) },
                body: transformed,
                signal: AbortSignal.timeout(2000),
            });
            await res.text().catch(() => "");
            const wasStale = res.headers.get("X-Frame-Drop") === "stale-seq";
            this.console.log(`snapshot "${v.name}": ${Date.now() - t0}ms total (${(transformed.length / 1024).toFixed(0)}KB)${wasStale ? " — beaten by stream frame" : ""}`);
        } catch { /* stream is starting anyway */ }
    }

    // Monotonic per-viewport sequence number paired with X-Frame-Seq
    // so the firmware can drop pipelined-out-of-order frames. Reset
    // on every stopStream so each stream session starts fresh; the
    // firmware also resets its comparator on /state {wake}, so the
    // two stay in step.
    private frameSeq = new Map<string, number>();

    private async pushStreamFrame(v: Viewport, jpeg: Buffer, abort: AbortController,
                                  emitMs: number, depthAtQueue: number) {
        if (abort.signal.aborted) return;
        const seq = (this.frameSeq.get(v.nativeId!) || 0) + 1;
        this.frameSeq.set(v.nativeId!, seq);
        const tFetchStart = Date.now();
        const opts: any = {
            method: "POST",
            headers: { "Content-Type": "image/jpeg", "X-Frame-Seq": String(seq) },
            body: jpeg,
            signal: abort.signal,
        };
        // undici dispatcher: keep-alive + 2 connections per host so
        // frame N+1 begins uploading on socket B while frame N is
        // still being decoded/painted on the device via socket A.
        // dispatcherFor returns undefined when undici isn't reachable
        // from the Scrypted plugin sandbox.
        const dispatcher = this.dispatcherFor(v.host);
        if (dispatcher) opts.dispatcher = dispatcher;
        const res = await fetch(`http://${v.host}/frame`, opts);
        const tHeaders = Date.now();
        const wasStale = res.headers.get("X-Frame-Drop") === "stale-seq";
        const fwTiming = this.parseServerTiming(res.headers.get("Server-Timing"));
        // Drain body so the socket can be released back to the pool.
        // Body is empty for 200/204/409; reading is essentially free
        // but the await pins our body-read measurement.
        await res.text().catch(() => "");
        const tDone = Date.now();

        const b = this.bucketsFor(v.nativeId!);
        const req_ms  = tHeaders - tFetchStart;
        const fw_tot  = fwTiming.handle ?? 0;
        b.emit.push (tFetchStart - emitMs);
        b.req.push  (req_ms);
        b.bread.push(tDone       - tHeaders);
        b.wall.push (tDone       - emitMs);
        if (fwTiming.recv  != null) b.fw_recv.push(fwTiming.recv);
        if (fwTiming.dec   != null) b.fw_dec.push(fwTiming.dec);
        if (fwTiming.paint != null) b.fw_paint.push(fwTiming.paint);
        if (fwTiming.post  != null) b.fw_post.push(fwTiming.post);
        if (fw_tot         >  0)    b.fw_tot.push(fw_tot);
        // net_up = the slice of req that the firmware DIDN'T see —
        // TCP handshake (when no keep-alive), body bytes on the wire,
        // and httpd dispatch from socket-readable to handler entry.
        // Can go slightly negative under clock skew; clamp at 0.
        if (fw_tot > 0) b.net_up.push(Math.max(0, req_ms - fw_tot));
        if (depthAtQueue === 0) b.depths.d0++;
        else if (depthAtQueue === 1) b.depths.d1++;
        else b.depths.d2++;
        if (wasStale) b.staleDrops++;

        const n = (this.fetchCount.get(v.nativeId!) || 0) + 1;
        this.fetchCount.set(v.nativeId!, n);
        if (n % 10 === 0) {
            const p = (arr: number[], q: number) => {
                if (!arr.length) return 0;
                const sorted = arr.slice().sort((a, b) => a - b);
                return sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * q))];
            };
            const stats = (arr: number[]) => {
                if (!arr.length) return null;
                let mn = arr[0], mx = arr[0];
                for (const v of arr) { if (v < mn) mn = v; if (v > mx) mx = v; }
                return { min: mn, p50: p(arr, 0.5), p95: p(arr, 0.95), max: mx };
            };
            const fmt = (arr: number[]) => {
                const s = stats(arr);
                if (!s) return `(no data)`;
                return `min=${s.min.toFixed(1)} p50=${s.p50.toFixed(1)} p95=${s.p95.toFixed(1)} max=${s.max.toFixed(1)}ms`;
            };
            // Identify the worst-wall sample in the window and decompose
            // it into its own stages — answers "what made the slowest
            // frame slow?" without us having to guess from percentiles.
            let worstIdx = 0;
            for (let i = 1; i < b.wall.length; i++) if (b.wall[i] > b.wall[worstIdx]) worstIdx = i;
            const at = (arr: number[], i: number) =>
                (i < arr.length && arr[i] != null) ? arr[i].toFixed(1) : "n/a";
            this.console.log(
                `fetch "${v.name}" #${n} (jpeg=${(jpeg.length / 1024).toFixed(0)}KB)\n` +
                `   wall       ${fmt(b.wall)}\n` +
                `   emit→post  ${fmt(b.emit)}     (queue wait before fetch() called)\n` +
                `   req        ${fmt(b.req)}     (fetch start → Response headers)\n` +
                `     net_up   ${fmt(b.net_up)}     (req − fw_total: TCP setup + body wire + dispatch)\n` +
                `     fw_recv  ${fmt(b.fw_recv)}     (firmware body read off the wire)\n` +
                `     fw_dec   ${fmt(b.fw_dec)}     (hardware JPEG → BGR888)\n` +
                `     fw_paint ${fmt(b.fw_paint)}     (backbuffer flip)\n` +
                `     fw_post  ${fmt(b.fw_post)}     (state counters + unlock)\n` +
                `   body-read  ${fmt(b.bread)}     (Response → drained)\n` +
                `   inflight   d0=${b.depths.d0} d1=${b.depths.d1} d2=${b.depths.d2}    (queue depth at fetch start)\n` +
                `   worst (#${worstIdx + 1}/${b.wall.length} in window): wall=${at(b.wall, worstIdx)}ms = ` +
                `emit→post ${at(b.emit, worstIdx)} + net_up ${at(b.net_up, worstIdx)} + ` +
                `fw_recv ${at(b.fw_recv, worstIdx)} + fw_dec ${at(b.fw_dec, worstIdx)} + ` +
                `fw_paint ${at(b.fw_paint, worstIdx)} + fw_post ${at(b.fw_post, worstIdx)} + ` +
                `body-read ${at(b.bread, worstIdx)}\n` +
                `   stale-drops=${b.staleDrops}`);
            this.fetchSamples.delete(v.nativeId!);   // reset window
        }
        if (res.status === 409) {
            this.console.log(`"${v.name}" returned 409 — device went to sleep, stopping stream`);
            this.stopStream(v.name, /*sendSleep=*/ false);
        } else if (res.status === 400) {
            // Body already drained; we lost the reason text. Log status
            // alone — repeat 400s in steady state should be rare.
            this.console.warn(`"${v.name}" /frame -> 400`);
        } else if (!res.ok && !wasStale) {
            this.console.warn(`"${v.name}" /frame -> ${res.status}`);
        }
        // Do NOT reset the idle timer on successful paint. The timer is
        // anchored to the triggering CAMERA EVENT, not to "frames are
        // flowing" — otherwise a continuously-streaming source means the
        // stream never times out. Repeated events naturally cancel-and-
        // restart the stream via startStream → stopStream(false), which
        // covers the "still active" case.
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
        const host = new URL(url).host.split(":")[0];
        const opts: any = {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(body),
            signal: AbortSignal.timeout(HTTP_TIMEOUT_MS),
        };
        const dispatcher = this.dispatcherFor(host);
        if (dispatcher) opts.dispatcher = dispatcher;
        const res = await fetch(url, opts);
        if (!res.ok && res.status !== 204) {
            const text = await res.text().catch(() => "");
            throw new Error(`POST ${url} -> ${res.status} ${text}`);
        }
    }
}

export default ScryptedViewportProvider;
