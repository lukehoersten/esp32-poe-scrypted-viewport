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
//       Viewport name    e.g. "mudroom"  (becomes the routing key + mDNS name)
//       IP or hostname   e.g. "192.168.1.42"
//       Camera           pick from the dropdown of Camera devices
//       Orientation      portrait (480×800) or landscape (800×480)
// 5. The script POSTs /config to the device immediately and re-issues it
//    every 5 minutes so a reboot or DHCP renumber re-syncs.
// 6. Edit a viewport's settings from its own device page in the UI. The
//    script re-registers and re-subscribes whenever a setting changes.
//
// v1 limits (path to v2: a packaged plugin with FFmpeg streaming)
// ---------------------------------------------------------------
// - Snapshot-rate (~1 fps) via Camera.takePicture(). Live-rate MJPEG over
//   POST /stream is v2.
// - Manual IP per viewport (no mDNS-SD discovery). Use a DHCP reservation.
// - Camera must respect picture.width/height OR be paired with a snapshot
//   plugin that resizes. Otherwise /frame returns 400.

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

const dns = require("dns").promises;

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
const DEFAULT_FRAME_INTERVAL_MS  = 1_000;
const REREGISTER_INTERVAL_MS     = 5 * 60_000;
const HTTP_TIMEOUT_MS            = 1_000;
const DEFAULT_IDLE_TIMEOUT_MS    = 60_000;
const DEFAULT_BRIGHTNESS         = 80;
const MDNS_LOOKUP_TIMEOUT_MS     = 1_500;

// Resolve `viewport-<name>.local` via the OS resolver (Bonjour on macOS,
// nss-mdns on Linux, host networking on Docker). Returns null on failure or
// timeout — caller falls back to the operator-entered host.
async function lookupMdns(hostname: string): Promise<string | null> {
    try {
        const lookup = dns.lookup(hostname, { family: 4 });
        const timeout = new Promise<null>(r => setTimeout(() => r(null), MDNS_LOOKUP_TIMEOUT_MS));
        const result = await Promise.race([lookup, timeout]);
        if (!result) return null;
        return (result as { address: string }).address || null;
    } catch {
        return null;
    }
}

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
    get mdnsAuto(): boolean {
        return this.storage.getItem("mdns_auto") !== "false";  // default true
    }

    async getSettings(): Promise<Setting[]> {
        return [
            {
                key: "host",
                title: "IP or hostname",
                description: "Viewport's address on the LAN. Auto-resolved via mDNS when the toggle below is on; otherwise set manually.",
                placeholder: "192.168.1.42",
                value: this.host,
            },
            {
                key: "mdns_auto",
                title: "Auto-resolve via mDNS",
                description: "Look up viewport-<name>.local via the OS resolver on every register and overwrite the host field with the discovered IP. Disable for cross-VLAN setups or hosts where mDNS resolution doesn't work.",
                type: "boolean",
                value: this.mdnsAuto,
            },
            {
                key: "cameraId",
                title: "Camera",
                description: "Camera whose events drive this viewport's wake/sleep, and whose snapshots get streamed.",
                type: "device",
                deviceFilter: `interfaces.includes('${ScryptedInterface.Camera}')`,
                value: this.cameraId,
            },
            {
                key: "orientation",
                title: "Orientation",
                description: "Panel orientation. Frames are sent at this effective resolution.",
                choices: ["portrait", "landscape"],
                value: this.orientation,
            },
            {
                key: "idle_timeout_ms",
                title: "Idle timeout (ms)",
                description: "Sent to the device via /config; both sides time independently. 0 disables the device-side idle timer; non-zero must be ≥ 5000.",
                type: "number",
                value: this.idleTimeoutMs,
            },
            {
                key: "brightness",
                title: "Brightness (0–100)",
                description: "Sent to the device via /config. Gamma-corrected on the panel.",
                type: "number",
                value: this.brightness,
            },
        ];
    }

    async putSetting(key: string, value: SettingValue) {
        this.storage.setItem(key, String(value ?? ""));
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
        interval: NodeJS.Timeout;
        timeout: NodeJS.Timeout;
        abort: AbortController;
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

    private get frameIntervalMs(): number {
        const v = this.storage.getItem("frame_interval_ms");
        return v ? Math.max(100, parseInt(v, 10) || DEFAULT_FRAME_INTERVAL_MS)
                 : DEFAULT_FRAME_INTERVAL_MS;
    }

    private async start() {
        const raw = await endpointManager.getInsecurePublicLocalEndpoint(this.id);
        this.scryptedBase = raw.replace(/\/$/, "");
        this.console.log(`Scrypted Viewport up. Callback URL base: ${this.scryptedBase}`);

        // Eagerly instantiate every known child so its registration + camera
        // event subscription happen at plugin load (rather than waiting for
        // some other part of Scrypted to touch the child).
        for (const nativeId of this.childIds) {
            try { await this.getDevice(nativeId); }
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
                description: 'Routing key sent back in callbacks + basis for the device\'s mDNS hostname (viewport-<name>.local). Lowercase, no spaces. Example: "mudroom".',
                placeholder: "mudroom",
            },
            {
                key: "host",
                title: "IP or hostname (optional)",
                description: "Leave blank to auto-resolve via mDNS (viewport-<name>.local). Set manually if mDNS doesn't reach this network.",
                placeholder: "auto-resolve via mDNS",
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

        // Pre-populate the child's storage so its first registration uses
        // the form values rather than empty defaults.
        const childStore = deviceManager.getDeviceStorage(nativeId);
        childStore.setItem("host",         String(settings.host || ""));
        childStore.setItem("cameraId",     String(settings.cameraId || ""));
        childStore.setItem("orientation",  String(settings.orientation || "portrait"));

        await deviceManager.onDeviceDiscovered({
            providerNativeId: this.nativeId,
            nativeId,
            name,
            type: ScryptedDeviceType.Sensor,
            interfaces: [ScryptedInterface.Settings],
        });

        this.childIds = [...this.childIds, nativeId];
        this.console.log(`created viewport "${name}" (${nativeId})`);

        // Fire-and-forget mDNS resolve so the host field is auto-populated
        // by the time the operator opens the new device's settings page.
        // getDevice() above already attempted a register; this catches the
        // case where the operator left host blank and mDNS resolution was
        // the only way to fill it.
        const child = this.viewports.get(nativeId);
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
        if (!cam) {
            this.console.warn(`viewport "${v.name}": camera ${v.cameraId} not found`);
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
        this.console.log(`viewport "${v.name}": subscribed to "${cam.name}"`);
    }

    private detachListener(nativeId: string) {
        const reg = this.listeners.get(nativeId);
        if (reg) {
            try { reg.removeListener(); } catch {}
            this.listeners.delete(nativeId);
        }
    }

    private async refreshHostFromMdns(v: Viewport): Promise<void> {
        if (!v.mdnsAuto || !v.name) return;
        const ip = await lookupMdns(`viewport-${v.name}.local`);
        if (!ip || ip === v.host) return;
        this.console.log(`mDNS: "${v.name}" host "${v.host || "(empty)"}" -> "${ip}"`);
        v.storage.setItem("host", ip);
    }

    private async registerViewport(v: Viewport) {
        await this.refreshHostFromMdns(v);
        if (!v.host) {
            this.console.warn(`register "${v.name}" skipped — no host (set one manually or check mDNS)`);
            return;
        }
        try {
            await this.postJSON(`http://${v.host}/config`, {
                viewport: v.name,
                scrypted: this.scryptedBase,
                idle_timeout_ms: v.idleTimeoutMs,
                orientation: v.orientation,
                brightness: v.brightness,
            });
            this.console.log(`registered "${v.name}" (${v.host})`);
        } catch (e) {
            this.console.warn(`register "${v.name}" failed:`, (e as Error).message);
        }
    }

    // ------------------------------------------------------------------------
    // Camera event → stream
    // ------------------------------------------------------------------------

    private handleCameraEvent(v: Viewport, details: any, data: any) {
        const iface = details.eventInterface;
        let trigger = false;
        if (iface === ScryptedInterface.BinarySensor && data === true) trigger = true;
        if (iface === ScryptedInterface.MotionSensor && data === true) trigger = true;
        if (iface === ScryptedInterface.ObjectDetector) {
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

        const abort = new AbortController();
        const interval = setInterval(() => {
            this.pushFrame(v, abort).catch(e => {
                if (!abort.signal.aborted) this.console.warn(`pushFrame "${v.name}":`, (e as Error).message);
            });
        }, this.frameIntervalMs);

        const timeoutMs = v.idleTimeoutMs > 0 ? v.idleTimeoutMs : DEFAULT_IDLE_TIMEOUT_MS;
        const timeout = setTimeout(() => {
            this.console.log(`"${v.name}": Scrypted-side stream timeout — stopping`);
            this.stopStream(v.name);
        }, timeoutMs);

        this.streams.set(v.name, { interval, timeout, abort });
    }

    private stopStream(name: string, sendSleep = true) {
        const s = this.streams.get(name);
        if (!s) return;
        s.abort.abort();
        clearInterval(s.interval);
        clearTimeout(s.timeout);
        this.streams.delete(name);
        if (sendSleep) {
            const v = this.findByName(name);
            if (v?.host) {
                this.postJSON(`http://${v.host}/state`, { state: "sleep" }).catch(() => {});
            }
        }
    }

    private findByName(name: string): Viewport | undefined {
        for (const v of this.viewports.values()) if (v.name === name) return v;
        return undefined;
    }

    private async pushFrame(v: Viewport, abort: AbortController) {
        if (abort.signal.aborted) return;
        const cam: any = systemManager.getDeviceById(v.cameraId);
        if (!cam) return;

        const w = v.orientation === "portrait" ? 480 : 800;
        const h = v.orientation === "portrait" ? 800 : 480;

        const picture = await cam.takePicture({
            picture: { width: w, height: h },
            reason: "event",
        });
        const buf: Buffer = await mediaManager.convertMediaObjectToBuffer(picture, "image/jpeg");

        const res = await fetch(`http://${v.host}/frame`, {
            method: "POST",
            headers: { "Content-Type": "image/jpeg" },
            body: buf,
            signal: abort.signal,
        });

        if (res.status === 409) {
            this.console.log(`"${v.name}" returned 409 — device went to sleep, stopping stream`);
            this.stopStream(v.name, /*sendSleep=*/ false);
        } else if (res.status === 400) {
            const reason = await res.text().catch(() => "");
            this.console.warn(`"${v.name}" returned 400: ${reason}`);
        } else if (!res.ok) {
            this.console.warn(`"${v.name}" /frame -> ${res.status}`);
        }
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
    // Parent Settings — global tuning only
    // ------------------------------------------------------------------------

    async getSettings(): Promise<Setting[]> {
        return [
            {
                key: "frame_interval_ms",
                title: "Frame push interval (ms)",
                description: "How often a snapshot is pushed to each viewport during an active stream. 1000 = 1 fps.",
                type: "number",
                value: this.frameIntervalMs,
            },
        ];
    }

    async putSetting(key: string, value: SettingValue) {
        this.storage.setItem(key, String(value ?? ""));
    }

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
