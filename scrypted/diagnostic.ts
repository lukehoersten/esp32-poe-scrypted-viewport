// Paste into the diagnostic / probe Scrypted script. Save + Run.
//
// Probe 1: which Node/mDNS modules the scriptedEval sandbox lets us
// require() — drives whether the auto-discovery feature can use a
// proper library or has to fall back to raw multicast UDP.
//
// Probe 2: one-shot mDNS browse for `_scrypted-viewport._tcp` using the
// exact dgram legacy-unicast approach the main plugin uses (ephemeral
// source port; responders reply unicast per RFC 6762 §6.7). Validates,
// from inside the real sandbox: (a) dgram works, (b) multicast TX
// reaches the LAN, (c) the firmware answers. Expected output: one `rx`
// line + one parsed line per viewport on the LAN. Zero `rx` lines from
// a Docker install usually means bridge networking — switch the
// container to host networking.

declare const require: any;

console.log("------ scriptedEval require() probe ------");
for (const mod of [
    "dns",              // built-in; we already use this for dns.lookup
    "dgram",            // built-in; needed for raw mDNS multicast UDP
    "multicast-dns",
    "bonjour",
    "bonjour-service",
    "mdns",
]) {
    try {
        const m = require(mod);
        const top = Object.keys(m || {}).slice(0, 8).join(",");
        console.log(`  ${mod.padEnd(16)} -> ok; top keys: ${top}`);
    } catch (e: any) {
        console.log(`  ${mod.padEnd(16)} -> ${e?.code || "ERR"}: ${e?.message || e}`);
    }
}

// ---- Probe 2: mDNS browse (same code path as the plugin's mdnsBrowse) ----

const MDNS_SERVICE = "_scrypted-viewport._tcp.local";

function buildMdnsQuery(): Buffer {
    const labels = MDNS_SERVICE.split(".");
    let qnameLen = 1;
    for (const l of labels) qnameLen += 1 + l.length;
    const buf = Buffer.alloc(12 + qnameLen + 4);
    buf.writeUInt16BE(Date.now() & 0xffff, 0);
    buf.writeUInt16BE(1, 4);
    let off = 12;
    for (const l of labels) {
        buf.writeUInt8(l.length, off++);
        buf.write(l, off, "ascii");
        off += l.length;
    }
    buf.writeUInt8(0, off++);
    buf.writeUInt16BE(12, off);      // PTR
    buf.writeUInt16BE(1, off + 2);   // IN
    return buf;
}

function readDnsName(msg: Buffer, off: number): { name: string; next: number } {
    const parts: string[] = [];
    let next = -1, jumps = 0;
    while (off < msg.length) {
        const len = msg[off];
        if (len === 0) { if (next < 0) next = off + 1; break; }
        if ((len & 0xc0) === 0xc0) {
            if (next < 0) next = off + 2;
            if (++jumps > 16) break;
            off = ((len & 0x3f) << 8) | msg[off + 1];
            continue;
        }
        parts.push(msg.subarray(off + 1, off + 1 + len).toString("ascii"));
        off += 1 + len;
    }
    return { name: parts.join("."), next: next < 0 ? off : next };
}

function parseMdnsResponse(msg: Buffer): any[] {
    try {
        if (msg.length < 12) return [];
        const qd = msg.readUInt16BE(4);
        const total = msg.readUInt16BE(6) + msg.readUInt16BE(8) + msg.readUInt16BE(10);
        let off = 12;
        for (let i = 0; i < qd; i++) off = readDnsName(msg, off).next + 4;
        const ptrs: string[] = [];
        const srvs = new Map<string, { target: string; port: number }>();
        const txts = new Map<string, Record<string, string>>();
        const addrs = new Map<string, string>();
        for (let i = 0; i < total && off < msg.length; i++) {
            const rec = readDnsName(msg, off);
            off = rec.next;
            if (off + 10 > msg.length) break;
            const type = msg.readUInt16BE(off);
            const rdlen = msg.readUInt16BE(off + 8);
            off += 10;
            if (off + rdlen > msg.length) break;
            const key = rec.name.toLowerCase();
            if (type === 12 && key === MDNS_SERVICE) {
                ptrs.push(readDnsName(msg, off).name);
            } else if (type === 33) {
                srvs.set(key, { target: readDnsName(msg, off + 6).name,
                                port: msg.readUInt16BE(off + 4) });
            } else if (type === 16) {
                const kv: Record<string, string> = {};
                for (let t = off; t < off + rdlen; ) {
                    const l = msg[t];
                    const s = msg.subarray(t + 1, t + 1 + l).toString("utf8");
                    const eq = s.indexOf("=");
                    if (eq > 0) kv[s.slice(0, eq)] = s.slice(eq + 1);
                    t += 1 + l;
                }
                txts.set(key, kv);
            } else if (type === 1 && rdlen === 4) {
                addrs.set(key, `${msg[off]}.${msg[off + 1]}.${msg[off + 2]}.${msg[off + 3]}`);
            }
            off += rdlen;
        }
        const out: any[] = [];
        for (const inst of ptrs) {
            const key = inst.toLowerCase();
            const srv = srvs.get(key);
            const txt = txts.get(key) || {};
            const ip = srv ? addrs.get(srv.target.toLowerCase()) : undefined;
            if (!srv || !ip) continue;
            out.push({ inst, ip, port: srv.port, hostname: srv.target, txt });
        }
        return out;
    } catch { return []; }
}

(async () => {
    console.log("------ mdns browse probe (_scrypted-viewport._tcp) ------");
    try {
        const dgram = require("dgram");
        const sock = dgram.createSocket({ type: "udp4", reuseAddr: true });
        let rx = 0, parsed = 0;
        sock.on("error", (e: Error) => console.log(`  socket error: ${e.message}`));
        sock.on("message", (msg: Buffer, rinfo: any) => {
            rx++;
            console.log(`  rx ${rinfo.address}:${rinfo.port} ${msg.length}B`);
            for (const v of parseMdnsResponse(msg)) {
                parsed++;
                console.log(`  -> ${v.ip}:${v.port} name=${v.txt.name} mac=${v.txt.mac ?? "(pre-mac fw)"} ` +
                            `v=${v.txt.version} ${v.txt.resolution} ${v.txt.orientation} host=${v.hostname}`);
            }
        });
        const query = buildMdnsQuery();
        const send = () => { try { sock.send(query, 5353, "224.0.0.251"); } catch (e: any) { console.log(`  send failed: ${e?.message}`); } };
        sock.bind(0, () => {
            console.log(`  bound ephemeral port ${sock.address().port}; querying 224.0.0.251:5353 ...`);
            send();
        });
        setTimeout(send, 400);
        setTimeout(() => {
            try { sock.close(); } catch {}
            console.log(`  browse done: ${rx} response packet(s), ${parsed} viewport record(s)`);
            if (rx === 0) console.log("  (0 responses: if Scrypted runs in Docker, check it uses host networking)");
        }, 1500);
    } catch (e: any) {
        console.log(`  probe failed: ${e?.message || e}`);
    }
})();
