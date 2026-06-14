// Scrypted Viewport — v1 Scripts-plugin script
//
// What this does
// --------------
// One Scrypted Script binds N viewport devices (the ESP32-P4 panels) to N
// Scrypted cameras. On a camera event (doorbell ring, motion, person), it
// wakes the bound viewport and streams snapshots until the viewport tells
// us to stop (or the per-stream timeout cuts us off). When the operator
// taps the viewport, the device POSTs us a wake/sleep call and we mirror
// the stream state.
//
// How to install
// --------------
// 1. In Scrypted's web UI, install the "Scripts" plugin if you haven't.
// 2. Create a new script device under that plugin.
// 3. Paste this whole file into the script editor and save.
// 4. Edit the BINDINGS constant below — one entry per viewport device,
//    matching the `viewport` name the device will use in /config and
//    the Scrypted device ID of the camera you want it bound to.
// 5. The script registers each viewport with `/config` on load and again
//    every REREGISTER_INTERVAL_MS to handle DHCP renumbering / reboots.
//
// Protocol contract is in this repo's README.md and TESTING.md.
//
// Limitations of this v1
// ----------------------
// - Snapshot-rate (~1 fps) via Camera.takePicture(). Live-rate streaming
//   over POST /stream is v2 (custom plugin with FFmpeg/MJPEG).
// - Viewport IP is operator-configured, not mDNS-discovered. Add IPs to
//   each BINDINGS entry; if a viewport gets a new DHCP lease, update the
//   IP and re-save. (Proper bonjour-service discovery is a v1.1 addition.)
// - Camera must respect `picture.width`/`picture.height` in
//   PictureOptions, OR be paired with a snapshot plugin that resizes.
//   If the camera returns a different size, /frame will reject with 400
//   and the next attempt will fail the same way until the camera is
//   reconfigured.

import sdk, {
    HttpRequest,
    HttpRequestHandler,
    HttpResponse,
    ScryptedDeviceBase,
    ScryptedInterface,
    EventListenerRegister,
} from "@scrypted/sdk";

const { systemManager, endpointManager, mediaManager } = sdk;

// ============================================================================
// CONFIG — edit these for your setup
// ============================================================================

interface Binding {
    /** Viewport name; matches the device's /config `viewport` field and its
     *  mDNS hostname (viewport-<name>.local). Must be unique. */
    name: string;
    /** Operator-set IP or hostname of the viewport. Plain IP is simplest. */
    host: string;
    /** Scrypted device id (not name) of the camera. Get it from the URL
     *  bar of the camera's settings page in Scrypted. */
    cameraId: string;
    /** Physical orientation of the panel. `portrait` = 480x800, `landscape`
     *  = 800x480. Frames are sent at this resolution. */
    orientation: "portrait" | "landscape";
}

const BINDINGS: Binding[] = [
    // {
    //     name: "mudroom",
    //     host: "192.168.1.42",
    //     cameraId: "abcdef0123456789",
    //     orientation: "portrait",
    // },
];

/** Sent to the device in /config. The device's idle timer and Scrypted's
 *  per-stream timer both use this value, independently. */
const IDLE_TIMEOUT_MS = 60_000;

/** Hard ceiling on a stream session. Stops streaming this many ms after
 *  the most recent wake event. Matches the device's idle timeout so the
 *  two sides cut off together. */
const STREAM_TIMEOUT_MS = IDLE_TIMEOUT_MS;

/** Time between snapshot pushes during an active stream. 1 fps is fine
 *  for v1 ambient viewing; tune as desired. */
const FRAME_INTERVAL_MS = 1_000;

/** Re-issue POST /config to every viewport at this cadence so a viewport
 *  that rebooted (or got a new IP) re-syncs without manual intervention. */
const REREGISTER_INTERVAL_MS = 5 * 60_000;

/** Outbound HTTP timeout per call. */
const HTTP_TIMEOUT_MS = 1_000;

// ============================================================================
// Implementation
// ============================================================================

class ScryptedViewportScript extends ScryptedDeviceBase implements HttpRequestHandler {
    private bindings = new Map<string, Binding>();
    private listeners: EventListenerRegister[] = [];
    private streams = new Map<string, {
        interval: NodeJS.Timeout;
        timeout: NodeJS.Timeout;
        abort: AbortController;
    }>();
    private scryptedBase = "";

    constructor(nativeId?: string) {
        super(nativeId);
        this.start().catch(e => this.console.error("start failed", e));
    }

    private async start() {
        for (const b of BINDINGS) this.bindings.set(b.name, b);

        // Endpoint URL the devices will POST callbacks to.
        const raw = await endpointManager.getInsecurePublicLocalEndpoint(this.id);
        this.scryptedBase = raw.replace(/\/$/, "");
        this.console.log(`Scrypted Viewport script up. Callback URL base: ${this.scryptedBase}`);
        this.console.log(`Bindings: ${BINDINGS.length} viewport(s)`);

        // Register every viewport on boot and on a slow refresh timer.
        await this.registerAll();
        setInterval(() => this.registerAll().catch(() => {}), REREGISTER_INTERVAL_MS);

        // Subscribe to camera events for every binding.
        for (const b of BINDINGS) {
            const camera = systemManager.getDeviceById(b.cameraId);
            if (!camera) {
                this.console.warn(`camera ${b.cameraId} for viewport "${b.name}" not found`);
                continue;
            }
            const ifaces = [
                ScryptedInterface.BinarySensor,    // doorbell
                ScryptedInterface.MotionSensor,    // motion
                ScryptedInterface.ObjectDetector,  // person/vehicle/...
            ];
            const reg = camera.listen(ifaces, (source, details, data) => {
                this.handleCameraEvent(b, details, data);
            });
            this.listeners.push(reg);
            this.console.log(`subscribed to "${camera.name}" events for viewport "${b.name}"`);
        }
    }

    private viewportUrl(host: string, path: string) {
        return `http://${host}${path}`;
    }

    private async registerAll() {
        for (const b of BINDINGS) {
            try {
                await this.postJSON(this.viewportUrl(b.host, "/config"), {
                    viewport: b.name,
                    scrypted: this.scryptedBase,
                    idle_timeout_ms: IDLE_TIMEOUT_MS,
                    orientation: b.orientation,
                    brightness: 80,
                });
            } catch (e) {
                this.console.warn(`register ${b.name} (${b.host}) failed:`, (e as Error).message);
            }
        }
    }

    // ------------------------------------------------------------------------
    // Camera event → wake the bound viewport
    // ------------------------------------------------------------------------

    private handleCameraEvent(b: Binding, details: any, data: any) {
        const iface = details.eventInterface as ScryptedInterface;
        let trigger = false;

        if (iface === ScryptedInterface.BinarySensor && data === true) trigger = true;
        if (iface === ScryptedInterface.MotionSensor && data === true) trigger = true;
        if (iface === ScryptedInterface.ObjectDetector) {
            const detections = data?.detections ?? [];
            // Heuristic: any person detection wakes the viewport. Customize
            // here if you want to filter by zone, confidence, class, etc.
            if (detections.some((d: any) => d?.className === "person")) trigger = true;
        }
        if (!trigger) return;

        this.console.log(`event ${iface} on ${b.name} -> wake + stream`);
        this.startStream(b.name).catch(e => this.console.error("startStream failed", e));
    }

    // ------------------------------------------------------------------------
    // Stream control
    // ------------------------------------------------------------------------

    private async startStream(name: string) {
        const b = this.bindings.get(name);
        if (!b) return;

        // Race rule from spec: cancel pending operations on every callback.
        // Stopping an already-running stream first ensures the safety timeout
        // restarts cleanly and we don't double-send /state {wake}.
        this.stopStream(name, /*sendSleep=*/ false);

        // Wake the device first so it shows the loading screen before frames arrive.
        await this.postJSON(this.viewportUrl(b.host, "/state"), { state: "wake" });

        const abort = new AbortController();
        const interval = setInterval(() => {
            this.pushFrame(b, abort).catch(e => {
                if (!abort.signal.aborted) this.console.warn(`pushFrame ${name} failed:`, (e as Error).message);
            });
        }, FRAME_INTERVAL_MS);

        const timeout = setTimeout(() => {
            this.console.log(`${name} stream timeout — stopping`);
            this.stopStream(name);
        }, STREAM_TIMEOUT_MS);

        this.streams.set(name, { interval, timeout, abort });
    }

    private stopStream(name: string, sendSleep = true) {
        const s = this.streams.get(name);
        if (!s) return;
        s.abort.abort();
        clearInterval(s.interval);
        clearTimeout(s.timeout);
        this.streams.delete(name);

        if (sendSleep) {
            const b = this.bindings.get(name);
            if (b) {
                this.postJSON(this.viewportUrl(b.host, "/state"), { state: "sleep" })
                    .catch(() => {});
            }
        }
    }

    private async pushFrame(b: Binding, abort: AbortController) {
        if (abort.signal.aborted) return;

        const camera: any = systemManager.getDeviceById(b.cameraId);
        if (!camera) return;

        const w = b.orientation === "portrait" ? 480 : 800;
        const h = b.orientation === "portrait" ? 800 : 480;

        // Ask the camera for a JPEG at the panel's effective resolution. Many
        // snapshot providers honor this; some ignore it and return native
        // resolution (in which case the device will reject with 400).
        const picture = await camera.takePicture({
            picture: { width: w, height: h },
            reason: "event",
        });
        const buf: Buffer = await mediaManager.convertMediaObjectToBuffer(picture, "image/jpeg");

        const res = await fetch(this.viewportUrl(b.host, "/frame"), {
            method: "POST",
            headers: { "Content-Type": "image/jpeg" },
            body: buf,
            signal: abort.signal,
        });

        if (res.status === 409) {
            this.console.log(`${b.name} returned 409 — device went to sleep, stopping stream`);
            this.stopStream(b.name, /*sendSleep=*/ false);
        } else if (res.status === 400) {
            // Most likely a dimension mismatch — log once per stream window.
            const reason = await res.text().catch(() => "");
            this.console.warn(`${b.name} returned 400: ${reason}`);
        } else if (!res.ok) {
            this.console.warn(`${b.name} /frame failed: ${res.status}`);
        }
    }

    // ------------------------------------------------------------------------
    // Inbound: device -> Scrypted POST /state
    // ------------------------------------------------------------------------

    async onRequest(request: HttpRequest, response: HttpResponse) {
        if (request.method !== "POST") {
            response.send("", { code: 405 });
            return;
        }
        if (!request.url.endsWith("/state")) {
            response.send("", { code: 404 });
            return;
        }

        let body: any;
        try {
            body = JSON.parse(request.body);
        } catch {
            response.send("invalid JSON", { code: 400 });
            return;
        }
        const { viewport, state } = body ?? {};
        if (typeof viewport !== "string" || !this.bindings.has(viewport)) {
            response.send(`unknown viewport: ${viewport}`, { code: 404 });
            return;
        }
        if (state !== "wake" && state !== "sleep") {
            response.send(`state must be wake or sleep`, { code: 400 });
            return;
        }

        this.console.log(`recv ${viewport} -> ${state} (device-initiated)`);

        if (state === "wake") {
            // Race rule: startStream cancels pending operations first.
            await this.startStream(viewport);
        } else {
            // Race rule: stopStream clears its safety timeout. Don't echo a
            // /state {sleep} back to the device — it already knows.
            this.stopStream(viewport, /*sendSleep=*/ false);
        }

        response.send("", { code: 204 });
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

export default ScryptedViewportScript;
