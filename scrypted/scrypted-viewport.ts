// Scrypted Viewport — v1 Scripts-plugin script
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
const HTTP_TIMEOUT_MS            = 1_000;
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
    private streams = new Map<string, {                           // viewport name -> stream control
        timeout: NodeJS.Timeout;
        abort:   AbortController;
        proc?:   any;            // ffmpeg child process (live-stream mode)
        interval?: NodeJS.Timeout; // legacy snapshot-poll mode
    }>();
    private scryptedBase = "";

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
        this.console.log(`Scrypted Viewport up. Callback URL base: ${this.scryptedBase}`);

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

    onBindingChanged = async (v: Viewport): Promise<void> => {
        const nid = v.nativeId!;
        this.detachListener(nid);
        // Any active stream for this viewport is now stale (camera may have
        // changed). Stop it cleanly; the next event/wake will start fresh.
        this.stopStream(v.name, /*sendSleep=*/ false);
        this.attachListener(v);
        await this.registerViewport(v);
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
        this.startStream(v).catch(e => this.console.error("startStream failed", e));
    }

    private async startStream(v: Viewport) {
        // Race rule: cancel pending operations on every callback before
        // beginning a fresh stream.
        this.stopStream(v.name, /*sendSleep=*/ false);

        if (!v.host || !v.cameraId) return;

        await this.postJSON(`http://${v.host}/state`, { state: "wake" });

        const cam: any = systemManager.getDeviceById(v.cameraId);
        if (!cam) return;

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
        const vf = v.orientation === "portrait"
            ? `scale=${panelH}:${panelW}:flags=lanczos,transpose=1`   // 90° CW → panelW x panelH
            : `scale=${panelW}:${panelH}:flags=lanczos`;
        const fps = Math.max(1, Math.round(1000 / v.frameIntervalMs));

        // Pull the camera's video stream, convert to ffmpeg input args, and
        // pipe through a single ffmpeg child: input → scale(lanczos) →
        // mjpeg q:v 2 → image2pipe. We then framer the raw MJPEG bytes
        // out of stdout into individual JPEGs and POST each one. This
        // beats the snapshot path because:
        //   - the camera's main encoder is producing keyframes anyway, so
        //     we're paying ~zero extra on the source side,
        //   - ffmpeg sustains real fps; the takePicture loop never could,
        //   - quality stays high (lanczos + q:v 2 ≈ visually lossless).
        let stream: any;
        try {
            stream = await cam.getVideoStream({ destination: "remote-recorder" });
        } catch {
            stream = await cam.getVideoStream();
        }
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
        const proc = spawn(ffmpegPath, [
            "-hide_banner", "-loglevel", "error",
            "-fflags", "+genpts+nobuffer", "-flags", "low_delay",
            ...(ffmpegInput.inputArguments || []),
            "-an", "-sn",
            "-vf", `${vf},fps=${fps}`,
            "-c:v", "mjpeg", "-q:v", "2",
            "-f", "image2pipe", "-flush_packets", "1",
            "pipe:1",
        ]);

        // Single-flight: only one POST /frame in flight at a time. The
        // firmware's JPEG decoder mutex returns 503 otherwise; we just
        // drop the spare frames silently — natural rate-limiter.
        let inFlight = false;
        let droppedFrames = 0;
        let lastLogUs = Date.now();
        let workBuf: Buffer = Buffer.alloc(0);

        proc.stdout.on("data", (chunk: Buffer) => {
            if (abort.signal.aborted) return;
            workBuf = workBuf.length === 0 ? chunk : Buffer.concat([workBuf, chunk]);
            // Frame each complete JPEG (SOI = 0xFF 0xD8 ... EOI = 0xFF 0xD9).
            // ffmpeg with image2pipe + mjpeg writes back-to-back complete
            // JPEGs so we just split on EOI.
            while (true) {
                const eoi = workBuf.indexOf(Buffer.from([0xff, 0xd9]));
                if (eoi < 0) break;
                const frame = workBuf.subarray(0, eoi + 2);
                workBuf = workBuf.subarray(eoi + 2);
                if (frame.length < 4 || frame[0] !== 0xff || frame[1] !== 0xd8) continue;

                if (inFlight) { droppedFrames++; continue; }
                inFlight = true;
                this.pushStreamFrame(v, frame, abort)
                    .catch(e => {
                        if (abort.signal.aborted) return;
                        const err = (e as Error);
                        const cause = (err as any)?.cause?.code || "";
                        this.console.warn(`pushStreamFrame "${v.name}":`, err.message, cause ? `(${cause})` : "");
                    })
                    .finally(() => { inFlight = false; });
            }
        });

        proc.stderr.on("data", (chunk: Buffer) => {
            // SIGTERM teardown flushes a half-written packet and ffmpeg
            // emits "Immediate exit requested" / "Error muxing a packet"
            // to stderr. Skip the noise once we're aborting.
            if (abort.signal.aborted) return;
            const text = chunk.toString("utf8").trim();
            if (!text) return;
            if (text.includes("Immediate exit requested")) return;
            this.console.warn(`ffmpeg "${v.name}": ${text}`);
        });
        proc.on("error", (e: any) => {
            if (!abort.signal.aborted) this.console.warn(`ffmpeg "${v.name}" spawn error:`, e.message);
        });
        proc.on("close", (code: number) => {
            if (!abort.signal.aborted) this.console.log(`ffmpeg "${v.name}" exited (code=${code})`);
        });
        abort.signal.addEventListener("abort", () => {
            try { proc.kill("SIGTERM"); } catch {}
        });

        // Periodic skip-rate log so the operator sees the effective fps
        // and can decide whether to bump frame_interval_ms up or down.
        const skipLogger = setInterval(() => {
            const now = Date.now();
            if (droppedFrames > 0) {
                const window = (now - lastLogUs) / 1000;
                this.console.log(`"${v.name}": dropping ~${(droppedFrames / window).toFixed(1)} fps over the last ${window.toFixed(1)}s; firmware decode-paint is the ceiling`);
                droppedFrames = 0;
                lastLogUs     = now;
            }
        }, 10_000);
        abort.signal.addEventListener("abort", () => clearInterval(skipLogger));

        const timeoutMs = v.idleTimeoutMs > 0 ? v.idleTimeoutMs : DEFAULT_IDLE_TIMEOUT_MS;
        const timeout = setTimeout(() => {
            this.console.log(`"${v.name}": Scrypted-side stream timeout — stopping`);
            this.stopStream(v.name);
        }, timeoutMs);

        this.streams.set(v.name, { timeout, abort, proc });
    }

    private stopStream(name: string, sendSleep = true) {
        const s = this.streams.get(name);
        if (!s) return;
        s.abort.abort();
        if (s.interval) clearInterval(s.interval);
        clearTimeout(s.timeout);
        if (s.proc) { try { s.proc.kill("SIGTERM"); } catch {} }
        this.streams.delete(name);
        if (sendSleep) {
            const v = this.findByName(name);
            if (v?.host) {
                this.postJSON(`http://${v.host}/state`, { state: "sleep" }).catch(() => {});
            }
        }
    }

    private async pushStreamFrame(v: Viewport, jpeg: Buffer, abort: AbortController) {
        if (abort.signal.aborted) return;
        const res = await fetch(`http://${v.host}/frame`, {
            method: "POST",
            headers: { "Content-Type": "image/jpeg" },
            body: jpeg,
            signal: abort.signal,
        });
        if (res.status === 409) {
            this.console.log(`"${v.name}" returned 409 — device went to sleep, stopping stream`);
            this.stopStream(v.name, /*sendSleep=*/ false);
        } else if (res.status === 400) {
            const reason = await res.text().catch(() => "");
            this.console.warn(`"${v.name}" /frame -> 400: ${reason}`);
        } else if (!res.ok) {
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
        const res = await fetch(url, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(body),
            signal: AbortSignal.timeout(HTTP_TIMEOUT_MS),
        });
        if (!res.ok && res.status !== 204) {
            const text = await res.text().catch(() => "");
            throw new Error(`POST ${url} -> ${res.status} ${text}`);
        }
    }
}

export default ScryptedViewportProvider;
